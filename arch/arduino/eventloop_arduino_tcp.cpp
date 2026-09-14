/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 *    Copyright 2026 (c) Autonomy Logic
 */

/*
 * TCP ConnectionManager over Arduino's abstract `Client`.
 *
 * It opens no socket. The sketch accepts connections and hands them in via
 * UA_Arduino_acceptClient(); everything below works through `Client*` and
 * names no board. See open62541_arduino.h for why.
 */

#include "arduino_internal.h"
#include <string.h>

namespace {

struct Conn
{
    Client* client;
    void*   context;
    void*   application;
    UA_ConnectionManager_connectionCallback cb;
    bool    announced;
};

/** The listener's connection id.
 *
 *  open62541 models the listening socket as a connection of its own, so it
 *  needs an id that can never collide with a real one. Slots are numbered
 *  1..MAX, so MAX+1 is free by construction. */
constexpr uintptr_t kListenerId = UA_ARDUINO_MAX_CONNECTIONS + 1;

struct ArduinoTcpCM
{
    UA_ConnectionManager base;   // MUST be first: open62541 casts between them
    Conn                 conns[UA_ARDUINO_MAX_CONNECTIONS];
    uint8_t              recv[UA_ARDUINO_RECV_BUFFER_SIZE];
    bool                 listening;
    void*                listener_context;   // what accepted clients inherit
    UA_ConnectionManager_connectionCallback cb;
    void*                application;
    UA_UInt16            port;
};

/** The connection manager the accept API talks to.
 *
 *  UA_Arduino_acceptClient() is a free function because the sketch calls it
 *  from loop() with nothing but a client in hand, so it needs to find the CM
 *  somehow. A server has exactly one TCP connection manager, so a single
 *  pointer is enough; a second CM replaces the first, which is a
 *  configuration nobody has and would be wrong in more interesting ways. */
ArduinoTcpCM* g_cm = nullptr;

/* Hooks the sketch may install for the two things `Client` cannot answer. */
UA_Arduino_CanSendFn g_can_send      = nullptr;
void*                g_can_send_ctx  = nullptr;
UA_Arduino_ClosedFn  g_closed        = nullptr;
void*                g_closed_ctx    = nullptr;

/** Address published in the server's DiscoveryUrl. The library cannot know it
 *  -- the sketch is what called Ethernet.begin() -- so it is settable and
 *  defaults to the unspecified address, which clients treat as "the host I
 *  connected to". */
char g_discovery_address[40] = "0.0.0.0";

ArduinoTcpCM* self(UA_ConnectionManager* cm) { return reinterpret_cast<ArduinoTcpCM*>(cm); }

void drop(ArduinoTcpCM* m, uint8_t idx)
{
    Conn& c = m->conns[idx];
    if (c.client == nullptr)
        return;

    // Tell open62541 first, while the id is still resolvable, so it can
    // release its SecureChannel before the slot is reused.
    //
    // BUT only if the server actually took ownership of this connection. An
    // accepted client starts out carrying the LISTENER's context -- that is
    // how open62541 recognises a new arrival on its own server socket -- and
    // the server replaces it with a SecureChannel on the first callback. If
    // the peer went away before that happened, the context is still the
    // listener's UA_ServerConnection, and reporting CLOSING with it makes the
    // server believe its LISTENING SOCKET closed: it zeroes the connectionId
    // and decrements serverConnectionsSize, after which the next client
    // cannot be given a SecureChannel and gets BadInternalError at Hello.
    // Observed on hardware as perfectly alternating ACK / ERR responses to
    // identical UA-TCP Hellos.
    //
    // Nothing leaks by staying quiet: if no channel was created, there is
    // nothing for the server to release.
    const bool server_owns_it = (c.cb != nullptr && c.context != nullptr &&
                                 c.context != m->listener_context);
    if (server_owns_it)
    {
        c.cb(&m->base, (uintptr_t)(idx + 1), c.application, &c.context,
             UA_CONNECTIONSTATE_CLOSING, &UA_KEYVALUEMAP_NULL, UA_BYTESTRING_NULL);
    }

    Client* released = c.client;
    c.client    = nullptr;
    c.context   = nullptr;
    c.cb        = nullptr;
    c.announced = false;

    released->stop();
    if (g_closed != nullptr)
        g_closed(released, g_closed_ctx);
}

// ---------------------------------------------------------------------------
// UA_ConnectionManager entry points
// ---------------------------------------------------------------------------

/** open62541 calls this to listen, and would call it to dial out. */
UA_StatusCode cm_open(UA_ConnectionManager* cm, const UA_KeyValueMap* params,
                      void* application, void* context,
                      UA_ConnectionManager_connectionCallback connectionCallback)
{
    ArduinoTcpCM* m = self(cm);

    // `listen` is the real indicator, not the absence of an address: the
    // server sets `address` as an ARRAY of String when the endpoint URL has a
    // hostname, so treating its presence as "this is an outbound connect"
    // rejects perfectly good listen requests.
    const UA_Boolean* listen = (const UA_Boolean*)UA_KeyValueMap_getScalar(
        params, UA_QUALIFIEDNAME(0, (char*)"listen"), &UA_TYPES[UA_TYPES_BOOLEAN]);
    const UA_UInt16* port = (const UA_UInt16*)UA_KeyValueMap_getScalar(
        params, UA_QUALIFIEDNAME(0, (char*)"port"), &UA_TYPES[UA_TYPES_UINT16]);

    if (listen == nullptr || !*listen || port == nullptr)
    {
        // Outbound connects are the client role, reverse-connect and PubSub,
        // none of which this port implements. Refusing explicitly beats
        // half-implementing it.
        return UA_STATUSCODE_BADNOTIMPLEMENTED;
    }

    // Nothing is opened here. The sketch owns the listening socket; all we do
    // is record that the server is ready to be given clients.
    m->listening        = true;
    m->cb               = connectionCallback;
    m->application      = application;
    m->port             = *port;
    m->listener_context = context;   // NULL on the first call, by design

    // Announce the listener. The server needs `listen-port` and
    // `listen-address` to publish a DiscoveryUrl; without them a client that
    // connects is told there is no matching endpoint.
    UA_String addr = UA_STRING(g_discovery_address);
    UA_KeyValuePair kv[2];
    kv[0].key = UA_QUALIFIEDNAME(0, (char*)"listen-port");
    UA_Variant_setScalar(&kv[0].value, &m->port, &UA_TYPES[UA_TYPES_UINT16]);
    kv[1].key = UA_QUALIFIEDNAME(0, (char*)"listen-address");
    UA_Variant_setScalar(&kv[1].value, &addr, &UA_TYPES[UA_TYPES_STRING]);
    UA_KeyValueMap kvm = {2, kv};

    if (m->cb != nullptr)
    {
        m->cb(cm, kListenerId, m->application, &m->listener_context,
              UA_CONNECTIONSTATE_ESTABLISHED, &kvm, UA_BYTESTRING_NULL);
    }
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode cm_send(UA_ConnectionManager* cm, uintptr_t connectionId,
                      const UA_KeyValueMap* params, UA_ByteString* buf)
{
    (void)params;
    ArduinoTcpCM* m = self(cm);
    if (connectionId == 0 || connectionId > UA_ARDUINO_MAX_CONNECTIONS)
    {
        cm->freeNetworkBuffer(cm, connectionId, buf);
        return UA_STATUSCODE_BADCONNECTIONCLOSED;
    }
    Conn& c = m->conns[connectionId - 1];
    if (c.client == nullptr)
    {
        cm->freeNetworkBuffer(cm, connectionId, buf);
        return UA_STATUSCODE_BADCONNECTIONCLOSED;
    }

    const size_t want = buf->length;

    // Refuse rather than block.
    //
    // Client::write() waits for the stack's send buffer to drain when it is
    // full -- some cores spin on delay(1) -- and inside a sketch's loop that
    // is unbounded blocking on a remote peer's ACK. Measured at 1.27 seconds
    // in a single call. A reply we cannot send now costs one dropped
    // connection, which the client retries; a loop that stops costs whatever
    // else the sketch was responsible for.
    //
    // `availableForWrite()` is declared on concrete client classes and not on
    // `Client`, so we cannot ask ourselves -- the sketch supplies the
    // predicate. Without one we attempt the write.
    if (g_can_send != nullptr && !g_can_send(c.client, want, g_can_send_ctx))
    {
        cm->freeNetworkBuffer(cm, connectionId, buf);
        drop(m, (uint8_t)(connectionId - 1));
        return UA_STATUSCODE_BADCONNECTIONCLOSED;
    }

    const size_t sent = c.client->write(buf->data, want);
    cm->freeNetworkBuffer(cm, connectionId, buf);
    return (sent == want) ? UA_STATUSCODE_GOOD : UA_STATUSCODE_BADCONNECTIONCLOSED;
}

UA_StatusCode cm_close(UA_ConnectionManager* cm, uintptr_t connectionId)
{
    ArduinoTcpCM* m = self(cm);
    if (connectionId == kListenerId)
    {
        // The server is closing its listening socket. We never opened one, so
        // there is nothing to close -- but the state has to be reported or the
        // server waits forever for a socket that will never say it is gone.
        m->listening = false;
        if (m->cb != nullptr)
        {
            m->cb(cm, kListenerId, m->application, &m->listener_context,
                  UA_CONNECTIONSTATE_CLOSING, &UA_KEYVALUEMAP_NULL, UA_BYTESTRING_NULL);
        }
        return UA_STATUSCODE_GOOD;
    }
    if (connectionId == 0 || connectionId > UA_ARDUINO_MAX_CONNECTIONS)
        return UA_STATUSCODE_BADNOTFOUND;
    drop(m, (uint8_t)(connectionId - 1));
    return UA_STATUSCODE_GOOD;
}

UA_StatusCode cm_allocbuf(UA_ConnectionManager* cm, uintptr_t connectionId,
                          UA_ByteString* buf, size_t bufSize)
{
    (void)cm; (void)connectionId;
    return UA_ByteString_allocBuffer(buf, bufSize);
}

void cm_freebuf(UA_ConnectionManager* cm, uintptr_t connectionId, UA_ByteString* buf)
{
    (void)cm; (void)connectionId;
    UA_ByteString_clear(buf);
}

UA_StatusCode es_start(UA_EventSource* es)
{
    es->state = UA_EVENTSOURCESTATE_STARTED;
    return UA_STATUSCODE_GOOD;
}

void es_stop(UA_EventSource* es)
{
    ArduinoTcpCM* m = reinterpret_cast<ArduinoTcpCM*>(es);
    for (uint8_t i = 0; i < UA_ARDUINO_MAX_CONNECTIONS; i++)
        drop(m, i);
    m->listening = false;
    es->state = UA_EVENTSOURCESTATE_STOPPED;
}

UA_StatusCode es_free(UA_EventSource* es)
{
    ArduinoTcpCM* m = reinterpret_cast<ArduinoTcpCM*>(es);
    if (g_cm == m)
        g_cm = nullptr;
    UA_String_clear(&es->name);
    UA_free(m);
    return UA_STATUSCODE_GOOD;
}

} // namespace

// ---------------------------------------------------------------------------
// Polling: called by the EventLoop's run(), once per sketch iteration
// ---------------------------------------------------------------------------

void ua_arduino_cm_poll(UA_ConnectionManager* cm)
{
    ArduinoTcpCM* m = self(cm);
    if (!m->listening)
        return;

    for (uint8_t i = 0; i < UA_ARDUINO_MAX_CONNECTIONS; i++)
    {
        Conn& c = m->conns[i];
        if (c.client == nullptr)
            continue;
        const uintptr_t id = (uintptr_t)(i + 1);

        // Announce here rather than in acceptClient(), so every callback into
        // open62541 happens from inside the EventLoop's run() where the
        // server expects it -- not from wherever the sketch called accept.
        if (!c.announced)
        {
            c.announced = true;
            if (c.cb != nullptr)
            {
                c.cb(cm, id, c.application, &c.context,
                     UA_CONNECTIONSTATE_ESTABLISHED, &UA_KEYVALUEMAP_NULL, UA_BYTESTRING_NULL);
            }
        }

        // The only place a connection is retired. Checking available() too
        // means a peer that closed after sending a final request still gets
        // that request processed before the channel goes away.
        if (!c.client->connected() && c.client->available() == 0)
        {
            drop(m, i);
            continue;
        }

        // One read per poll, capped at the buffer. Bounding the work per
        // iteration matters more than draining a fast peer in one go: an
        // unbounded loop here is what would blow a sketch's timing budget.
        const int avail = c.client->available();
        if (avail <= 0)
            continue;
        size_t want = (size_t)avail;
        if (want > UA_ARDUINO_RECV_BUFFER_SIZE)
            want = UA_ARDUINO_RECV_BUFFER_SIZE;
        const int got = c.client->read(m->recv, want);
        if (got <= 0)
            continue;

        UA_ByteString msg;
        msg.data   = m->recv;
        msg.length = (size_t)got;
        if (c.cb != nullptr)
        {
            c.cb(cm, id, c.application, &c.context,
                 UA_CONNECTIONSTATE_ESTABLISHED, &UA_KEYVALUEMAP_NULL, msg);
        }
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

extern "C" UA_ConnectionManager*
UA_ConnectionManager_new_Arduino_TCP(const UA_String eventSourceName)
{
    ua_arduino_install_allocator();

    ArduinoTcpCM* m = (ArduinoTcpCM*)UA_calloc(1, sizeof(ArduinoTcpCM));
    if (m == nullptr)
        return nullptr;

    UA_ConnectionManager* cm = &m->base;
    cm->eventSource.eventSourceType = UA_EVENTSOURCETYPE_CONNECTIONMANAGER;
    UA_String_copy(&eventSourceName, &cm->eventSource.name);
    cm->eventSource.start = es_start;
    cm->eventSource.stop  = es_stop;
    cm->eventSource.free  = es_free;
    cm->eventSource.state = UA_EVENTSOURCESTATE_FRESH;

    cm->protocol          = UA_STRING_STATIC("tcp");
    cm->openConnection    = cm_open;
    cm->sendWithConnection = cm_send;
    cm->closeConnection   = cm_close;
    cm->allocNetworkBuffer = cm_allocbuf;
    cm->freeNetworkBuffer  = cm_freebuf;

    g_cm = m;
    return cm;
}

extern "C" UA_StatusCode UA_Arduino_acceptClient(Client* client)
{
    if (client == nullptr || g_cm == nullptr)
        return UA_STATUSCODE_BADINVALIDARGUMENT;
    ArduinoTcpCM* m = g_cm;
    if (!m->listening)
        return UA_STATUSCODE_BADCONNECTIONREJECTED;

    for (uint8_t i = 0; i < UA_ARDUINO_MAX_CONNECTIONS; i++)
    {
        if (m->conns[i].client == client)
            return UA_STATUSCODE_BADINVALIDARGUMENT;   // already held
    }
    for (uint8_t i = 0; i < UA_ARDUINO_MAX_CONNECTIONS; i++)
    {
        Conn& c = m->conns[i];
        if (c.client != nullptr)
            continue;
        c.client      = client;
        // Inherit the listener's context: that is how the server recognises a
        // new client on its own server socket and attaches a SecureChannel on
        // the first callback.
        c.context     = m->listener_context;
        c.cb          = m->cb;
        c.application = m->application;
        c.announced   = false;
        return UA_STATUSCODE_GOOD;
    }
    return UA_STATUSCODE_BADCONNECTIONREJECTED;
}

extern "C" void UA_Arduino_releaseClient(Client* client)
{
    if (client == nullptr || g_cm == nullptr)
        return;
    for (uint8_t i = 0; i < UA_ARDUINO_MAX_CONNECTIONS; i++)
    {
        if (g_cm->conns[i].client != client)
            continue;
        // Suppress the closed callback: the caller asked for this, so telling
        // them about it would be an echo they have to filter out.
        UA_Arduino_ClosedFn saved = g_closed;
        g_closed = nullptr;
        drop(g_cm, i);
        g_closed = saved;
        return;
    }
}

extern "C" size_t UA_Arduino_connectionCount(void)
{
    if (g_cm == nullptr)
        return 0;
    size_t n = 0;
    for (uint8_t i = 0; i < UA_ARDUINO_MAX_CONNECTIONS; i++)
        if (g_cm->conns[i].client != nullptr)
            n++;
    return n;
}

extern "C" void UA_Arduino_setCanSendCallback(UA_Arduino_CanSendFn fn, void* context)
{
    g_can_send     = fn;
    g_can_send_ctx = context;
}

extern "C" void UA_Arduino_setClosedCallback(UA_Arduino_ClosedFn fn, void* context)
{
    g_closed     = fn;
    g_closed_ctx = context;
}

extern "C" void UA_Arduino_setDiscoveryAddress(const char* host)
{
    if (host == nullptr)
        return;
    strncpy(g_discovery_address, host, sizeof(g_discovery_address) - 1);
    g_discovery_address[sizeof(g_discovery_address) - 1] = '\0';
}
