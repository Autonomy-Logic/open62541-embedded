/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 *    Copyright 2026 (c) Autonomy Logic
 */

/*
 * UA_ArduinoListener -- optional convenience over open62541_arduino.h.
 *
 * It owns a listening server, a pool of client slots, and wires up the two
 * callbacks, so the common case is one declaration and two calls:
 *
 *     #include <open62541_arduino.h>
 *     #include <UA_ArduinoListener.h>
 *     #include <Ethernet.h>
 *
 *     UA_ArduinoListener<EthernetServer, EthernetClient> listener(4840);
 *
 *     void setup() {
 *         Ethernet.begin(mac, ip);
 *         listener.begin();
 *         ...
 *     }
 *     void loop() {
 *         listener.poll();
 *         UA_Server_run_iterate(server, false);
 *     }
 *
 * This sits ON TOP of the Client& API, never underneath it. That ordering is
 * the whole point: a board this template does not suit still works, because
 * the sketch can call UA_Arduino_acceptClient() with clients from anywhere.
 * Libraries that put the board abstraction underneath instead end up with a
 * list of supported boards and a hard error for everything else.
 *
 * WHAT IT SOLVES
 * --------------
 * Client lifetime. `EthernetClient c = server.available();` is a temporary,
 * and a Client* into it dangles as soon as loop() returns. The pool below
 * holds N concrete clients by value, so the pointers handed to the library
 * stay valid until the library says it is done with them.
 *
 * And write capacity. availableForWrite() exists on the concrete client but
 * not on `Client`, so the predicate is installed here where the concrete type
 * is still known.
 */

#ifndef UA_ARDUINO_LISTENER_H
#define UA_ARDUINO_LISTENER_H

#include "open62541_arduino.h"

/** @tparam ServerT  concrete server, e.g. EthernetServer, WiFiServer
 *  @tparam ClientT  the client it hands out, e.g. EthernetClient
 *  @tparam N        client slots. Must not exceed what you passed to
 *                   UA_Arduino_configureTcp(), or accepted clients will be
 *                   refused by the server and immediately closed. */
template <class ServerT, class ClientT, size_t N = UA_ARDUINO_DEFAULT_MAX_CONNECTIONS>
class UA_ArduinoListener
{
public:
    explicit UA_ArduinoListener(uint16_t port)
        : server_(port), started_(false)
    {
        for (size_t i = 0; i < N; i++)
            used_[i] = false;
    }

    /** Open the listening socket and install the hooks.
     *
     *  Does NOT bring the interface up: call Ethernet.begin() / WiFi.begin()
     *  first. A library that also configured the interface would fight
     *  whatever else in the sketch already did. */
    void begin()
    {
        if (started_)
            return;
        server_.begin();
        started_ = true;
        UA_Arduino_setCanSendCallback(&canSendThunk, this);
        UA_Arduino_setClosedCallback(&closedThunk, this);
    }

    /** Accept at most one new connection and hand it to the server. Call once
     *  per loop(), before UA_Server_run_iterate(). */
    void poll()
    {
        if (!started_)
            return;

        ClientT incoming = accept_one();
        if (!incoming)
            return;

        const int slot = freeSlot();
        if (slot < 0)
        {
            // Every slot busy. Closing immediately is the honest answer: the
            // peer retries, whereas a half-accepted connection just sits
            // there consuming a socket in the network stack.
            incoming.stop();
            return;
        }

        clients_[slot] = incoming;   // copy into storage that outlives loop()
        used_[slot]    = true;
        if (UA_Arduino_acceptClient(&clients_[slot]) != UA_STATUSCODE_GOOD)
        {
            clients_[slot].stop();
            used_[slot] = false;
        }
    }

    /** Slots currently in use. */
    size_t inUse() const
    {
        size_t n = 0;
        for (size_t i = 0; i < N; i++)
            if (used_[i]) n++;
        return n;
    }

    /** The underlying server, for anything this wrapper does not cover. */
    ServerT& server() { return server_; }

private:
    /** `accept()` where the core has it, `available()` where it does not.
     *
     *  Cores disagree: ESP32 renamed available() to accept() and deprecated
     *  the old name, the classic Ethernet library has both with different
     *  meanings (available() returns a client with pending data, accept()
     *  returns a newly connected one), and older cores have only available().
     *  Preferring accept() when it compiles gets the right semantics on the
     *  cores that distinguish them, and the only available one elsewhere. */
    template <class S>
    static auto try_accept(S& s, int) -> decltype(s.accept())
    {
        return s.accept();
    }
    template <class S>
    static auto try_accept(S& s, long) -> decltype(s.available())
    {
        return s.available();
    }

    ClientT accept_one() { return try_accept(server_, 0); }

    int freeSlot() const
    {
        for (size_t i = 0; i < N; i++)
            if (!used_[i]) return (int)i;
        return -1;
    }

    static bool canSendThunk(Client* c, size_t n, void* ctx)
    {
        (void)ctx;
        // The concrete type is known here, which is the entire reason this
        // template exists. availableForWrite() returns what can be queued
        // right now without blocking.
        return static_cast<ClientT*>(c)->availableForWrite() >= (int)n;
    }

    static void closedThunk(Client* c, void* ctx)
    {
        UA_ArduinoListener* self = static_cast<UA_ArduinoListener*>(ctx);
        for (size_t i = 0; i < N; i++)
        {
            if (self->used_[i] && &self->clients_[i] == c)
            {
                self->used_[i] = false;
                return;
            }
        }
    }

    ServerT server_;
    ClientT clients_[N];
    bool    used_[N];
    bool    started_;
};

#endif /* UA_ARDUINO_LISTENER_H */
