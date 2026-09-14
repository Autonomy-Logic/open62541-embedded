/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 *    Copyright 2026 (c) Autonomy Logic
 */

/*
 * open62541_arduino.h -- the Arduino platform layer for open62541.
 *
 * The library is built with UA_ARCHITECTURE=none: open62541's core is
 * OS-independent and everything platform-specific sits behind plugin structs.
 * This header supplies the Arduino implementations of those plugins, so a
 * sketch on any core can run an OPC-UA server.
 *
 *
 * WHY THE SKETCH OWNS THE LISTENING SOCKET
 * ----------------------------------------
 * This library never opens a listening socket. You accept connections and hand
 * them to us. That looks like extra work for two lines of sketch, and it is the
 * only design that runs everywhere.
 *
 * Arduino's `Client` is a real abstraction: eleven pure virtuals covering
 * connect, read, write, available, peek, flush, stop, connected and operator
 * bool. Any EthernetClient or WiFiClient is usable as a `Client&`.
 *
 * Arduino's `Server` is not an abstraction at all. On every core -- including
 * the official ArduinoCore-API -- it is, in its entirety:
 *
 *     class Server : public Print {
 *       public:
 *         virtual void begin() = 0;
 *     };
 *
 * There is no portable accept. EthernetServer::available() returns an
 * EthernetClient *by value*; WiFiServer::available() returns a WiFiClient by
 * value; neither overrides anything. A library that wanted to own the listener
 * would have to name concrete server classes behind board macros, and would
 * then work only on the boards someone remembered to add -- which is how the
 * widely-ported HTTP servers end up with forty board branches in one header.
 *
 * So we take the approach Arduino themselves take in ArduinoModbus: the library
 * includes <Client.h>, never <Server.h>, names no board, and receives connected
 * clients from the sketch. See UA_ArduinoListener.h for a template adapter that
 * reduces the common case to one declaration.
 *
 *
 * TWO THINGS `Client` CANNOT TELL US
 * ----------------------------------
 * Both are gaps in Arduino's base class, and both are why the accept call takes
 * more than a bare pointer.
 *
 * 1. LIFETIME. `EthernetClient c = server.available();` yields a temporary. A
 *    `Client*` into it dangles the moment loop() returns -- the kind of bug
 *    that survives testing and fails under load. So the caller owns the
 *    storage and guarantees it outlives the connection, and we tell you when
 *    we are done with it via the closed callback.
 *
 * 2. WRITE CAPACITY. `availableForWrite()` is declared on the concrete client
 *    classes, not on `Client`. Without it a write can block: some cores spin
 *    on delay(1) until the stack's send buffer drains, which was measured at
 *    1.27 seconds in a single call. Inside a control loop that is unacceptable,
 *    so you may supply a canSend predicate. Without one we attempt the write.
 */

#ifndef OPEN62541_ARDUINO_H
#define OPEN62541_ARDUINO_H

#include <Arduino.h>
#include <Client.h>
#include <stddef.h>
#include <stdint.h>

/* Umbrella, not a nested path: arduino-cli resolves a library by basename at
 * src/ root, so <open62541/...> discovers nothing and the precompiled archive
 * silently misses the link. */
#include <open62541.h>

/* -------------------------------------------------------------------------
 * Compile-time configuration
 *
 * Every knob has a default and every one can be overridden from the sketch,
 * a build flag, or a board's platform.txt. They are compile-time because the
 * point of this port is that a server's memory cost is known at link time.
 * ------------------------------------------------------------------------- */

/** Defaults for UA_Arduino_configureTcp(). Both are runtime settings rather
 *  than compile-time ones so a generated project can choose them; see the
 *  arena note below for why a macro could not reach this library anyway. */
#define UA_ARDUINO_DEFAULT_MAX_CONNECTIONS 4

/** 8192 is a protocol floor, not a preference: OPC-UA Part 6 6.7.1 requires a
 *  SecureChannel to accept an 8192-byte chunk, and open62541 enforces it.
 *  Going below it makes the server reject conformant clients. */
#define UA_ARDUINO_DEFAULT_RECV_BUFFER 8192

/** Repeated callbacks the EventLoop can hold.
 *
 *  open62541 registers several of its own -- SecureChannel housekeeping,
 *  session timeouts -- and more arrive as features are enabled. Too small is
 *  not a soft limit: addTimer returns BADOUTOFMEMORY, the server carries on
 *  believing the callback is scheduled, and the work silently never happens.
 *  SecureChannel housekeeping is the one that must not be dropped: without it
 *  closed channels are never reaped and the next client is refused. */
#ifndef UA_ARDUINO_MAX_TIMERS
#define UA_ARDUINO_MAX_TIMERS 24
#endif

/** Wall-clock epoch, as a Unix timestamp, for boards with no RTC.
 *
 *  OPC-UA stamps every value with a DateTime. With no clock the best available
 *  answer is a build-time epoch plus uptime, which makes timestamps wrong by
 *  the device's accumulated downtime rather than wrong by 371 years -- the
 *  difference between a client showing a stale date and a client rejecting the
 *  response outright. Override it, or call UA_Arduino_setTime() once you have
 *  a real time from NTP or an RTC. */
#ifndef UA_ARDUINO_EPOCH_UNIX
#define UA_ARDUINO_EPOCH_UNIX 1767225600L  /* 2026-01-01T00:00:00Z */
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Platform plugin factories
 *
 * Pass these to your UA_ServerConfig. UA_ServerConfig_setDefault() installs
 * them for you; see UA_ServerConfig_setDefault_Arduino() below.
 * ------------------------------------------------------------------------- */

/** EventLoop driven from the sketch's loop(). Its run() is non-blocking and
 *  ignores the timeout it is given: sleeping would stop the sketch. Pending
 *  work is picked up on the next iteration, which request/response over TCP
 *  tolerates and a control loop's timing does not. */
UA_EventLoop* UA_EventLoop_new_Arduino(const UA_Logger* logger);

/** TCP ConnectionManager over the clients you hand us. Opens nothing.
 *
 *  Both buffers come from the arena, so this must be called after
 *  UA_Arduino_setArena(). UA_ServerConfig_setMinimal() calls it for you. */
UA_ConnectionManager* UA_ConnectionManager_new_Arduino_TCP(const UA_String eventSourceName);

/** Size the connection manager. Call before UA_Server_new(), which is when the
 *  server builds it; after that it has no effect.
 *
 *  `maxConnections` is concurrent clients. `recvBufferSize` must be at least
 *  UA_ARDUINO_DEFAULT_RECV_BUFFER or conformant clients will be rejected --
 *  a smaller value is clamped up rather than silently honoured. Zero for
 *  either keeps the current setting. */
void UA_Arduino_configureTcp(uint8_t maxConnections, size_t recvBufferSize);

/* -------------------------------------------------------------------------
 * Handing connections to the server
 * ------------------------------------------------------------------------- */

/** Predicate: can `n` bytes be queued on `client` right now without blocking?
 *
 *  Return false and we drop the connection rather than stall; the client will
 *  reconnect. Supply one built from your concrete client's
 *  availableForWrite() -- UA_ArduinoListener.h does this for you. */
typedef bool (*UA_Arduino_CanSendFn)(Client* client, size_t n, void* context);

/** Called when the library has finished with a client and closed it. Reclaim
 *  your storage here. Never called from inside your own accept call. */
typedef void (*UA_Arduino_ClosedFn)(Client* client, void* context);

/** Install the optional hooks. Both may be NULL, which is the default:
 *  writes are attempted unconditionally, and closed clients are simply
 *  stopped and forgotten. */
void UA_Arduino_setCanSendCallback(UA_Arduino_CanSendFn fn, void* context);
void UA_Arduino_setClosedCallback(UA_Arduino_ClosedFn fn, void* context);

/** Hand a freshly connected client to the server.
 *
 *  `client` must remain valid, at that address, until the closed callback
 *  fires or UA_Arduino_releaseClient() is called. Do not pass the address of
 *  a temporary -- see LIFETIME above.
 *
 *  Returns GOOD when adopted, BADCONNECTIONREJECTED when
 *  the configured maximum are already held (the caller should stop() the
 *  client), or BADINVALIDARGUMENT for a null or already-held client. */
UA_StatusCode UA_Arduino_acceptClient(Client* client);

/** Drop a client the library is holding, closing it. Safe to call for a
 *  client we do not hold. Does not invoke the closed callback: you already
 *  know. */
void UA_Arduino_releaseClient(Client* client);

/** How many connections are currently held. */
size_t UA_Arduino_connectionCount(void);

/** Address published in the server's DiscoveryUrl.
 *
 *  The library cannot know it: the sketch is what called Ethernet.begin() or
 *  WiFi.begin(). Set it to the device's address after the interface is up, or
 *  leave it at the default "0.0.0.0", which clients read as "the host I
 *  connected to". Must be set before UA_Server_run_startup(), which is when
 *  the endpoint URL is published. */
void UA_Arduino_setDiscoveryAddress(const char* host);

/* -------------------------------------------------------------------------
 * Clock
 * ------------------------------------------------------------------------- */

/** Set wall-clock time from NTP, an RTC, or anything else, as a Unix
 *  timestamp in seconds. Monotonic time is unaffected. Without this the clock
 *  reads UA_ARDUINO_EPOCH_UNIX plus uptime. */
void UA_Arduino_setTime(int64_t unixSeconds);

/* -------------------------------------------------------------------------
 * Flash-resident nodestore
 *
 * An embedded server's address space is fixed when the sketch is compiled, so
 * keeping it in RAM pays per node for something already `const` in flash.
 * Measured on a Cortex-M4 against open62541's default zip-tree nodestore:
 * 476 bytes of heap per node, and a Browse failing with BadOutOfMemory at 40
 * nodes. With this, what stays in RAM is 8 bytes of parent reference per node.
 *
 * Supply the callbacks below over whatever `const` table your build generates.
 * The nodestore wraps the default one, so anything outside your namespace --
 * namespace zero especially -- is delegated untouched.
 * ------------------------------------------------------------------------- */

/** Default simultaneously-materialised nodes; UA_Nodestore_newFlash() takes
 *  the real value.
 *
 *  Bounded by the OperationLimits: a Read walks its nodes one at a time, a
 *  Browse holds the browsed node plus what it is looking at. Deliberately
 *  small, and exhaustion is counted rather than tolerated, so a pool that is
 *  too small shows up in test instead of in the field. */
#define UA_ARDUINO_DEFAULT_NODE_POOL_SLOTS 8

typedef struct {
    /** Fill `out` with the node having this numeric id in namespace `ns`.
     *  Return false if there is no such node -- that is a normal answer, not
     *  an error. Anything allocated here is released by dematerialise. */
    bool (*materialise)(UA_UInt16 ns, UA_UInt32 numericId,
                        UA_VariableNode* out, void* context);

    /** Release whatever materialise() allocated (typically reference arrays).
     *  The node storage itself belongs to the pool and is not yours. */
    void (*dematerialise)(UA_VariableNode* node, void* context);

    /** Optional: how many nodes you have, and the id of the i-th.
     *
     *  Used only to let the server walk the whole address space. Leave both
     *  NULL if enumeration is inconvenient; clients are unaffected, since they
     *  reach nodes by id and through references. */
    UA_UInt16 (*count)(void* context);
    UA_UInt32 (*idAt)(UA_UInt16 index, void* context);

    /** The namespace these nodes live in. Everything else is delegated. */
    UA_UInt16 namespaceIndex;

    void* context;
} UA_Arduino_FlashNodeSource;

/** Wrap `inner` so that nodes in `source->namespaceIndex` come from flash.
 *
 *  Takes ownership of `inner`: freeing the returned nodestore frees it too,
 *  which is what open62541 does on shutdown. Returns NULL if the source is
 *  incomplete or the pool cannot be allocated, in which case keep using
 *  `inner` alone rather than run with no address space. `logger` may be NULL.
 *
 *  Install it before UA_Server_new():
 *
 *      UA_ServerConfig *cfg = ...;
 *      cfg->nodestore = *UA_Nodestore_newFlash(&source, cfg->nodestore, cfg->logging);
 */
UA_Nodestore* UA_Nodestore_newFlash(const UA_Arduino_FlashNodeSource* source,
                                    UA_Nodestore* inner,
                                    const UA_Logger* logger,
                                    uint16_t poolSlots,
                                    bool serveNamespaceZeroFromFlash);

/** Tell the flash nodestore which namespace its nodes live in.
 *
 *  Needed because serving namespace zero from flash forces the nodestore to
 *  exist BEFORE the server -- UA_Server_newWithConfig() is what runs
 *  namespace-zero initialisation -- while the index for your own namespace is
 *  only known after UA_Server_addNamespace() returns it. Call this once with
 *  that index. Until you do, the store answers for namespace zero only. */
void UA_Nodestore_flashSetNamespace(UA_Nodestore* ns, UA_UInt16 namespaceIndex);

/** Peak simultaneous materialised nodes, and how many times the pool was
 *  exhausted. `exhausted` must be zero in a healthy build. */
void UA_Arduino_getNodestoreStats(uint16_t* outHighWater, uint32_t* outExhausted);

/** Namespace-zero overlay usage: slots taken, and refusals for want of one.
 *
 *  Worth reading during bring-up. The overlay's own error goes through
 *  UA_Logger, which on a board with no console writes nowhere -- so a counter
 *  is what actually tells you. `refused` must be zero. */
void UA_Arduino_getNs0OverlayStats(uint16_t* outUsed, uint32_t* outRefused);

/* -------------------------------------------------------------------------
 * Bounded allocator
 *
 * open62541 calls UA_malloc on paths a server cannot avoid: session setup,
 * per-request scratch. Sharing the sketch's heap with a network-facing
 * protocol parser means a remote peer can fragment the memory your control
 * logic depends on, so give the server a buffer of its own instead.
 *
 * The library declares no arena. You own the buffer, which is the only
 * arrangement that works: how much RAM a server may have is a decision only
 * the application can make, and a library-side compile-time size could not be
 * reached by a sketch's build anyway -- arduino-cli does not put the sketch
 * include path on library compilation, so a generated header setting it would
 * be silently ignored.
 *
 *     static uint8_t opcuaArena[24 * 1024];
 *     UA_Arduino_setArena(opcuaArena, sizeof(opcuaArena));   // before UA_Server_new()
 *
 * Declared static at file scope, that is a link-time reservation: the binary
 * either fits or it does not, decided on your machine rather than in the
 * field. Pass nothing and UA_malloc falls through to the standard allocator,
 * which works and gives up every property above.
 *
 * Sizing it: usage barely tracks node count, because nodes come from flash
 * through a fixed pool and cost about 8 bytes each in parent references. The
 * terms that matter are namespace zero (a constant), roughly 1 KB per
 * concurrent session, and per-request scratch dominated by the send buffer.
 * Measure with UA_Arduino_getArenaStats() rather than guessing.
 * ------------------------------------------------------------------------- */

typedef struct {
    size_t   size;         /* arena capacity, bytes; 0 if none was supplied */
    size_t   inUse;        /* currently allocated, including block headers */
    size_t   highWater;    /* peak inUse since the arena was set */
    size_t   largestFree;  /* biggest single allocation still possible */
    uint32_t failures;     /* allocations refused for want of room */
} UA_Arduino_ArenaStats;

/** Give the server its heap. Call before UA_Server_new().
 *
 *  The buffer must outlive the server. Passing NULL, or a size too small to
 *  hold one allocation, reverts to the standard allocator. Calling it again
 *  re-carves the arena and resets the statistics, so do not call it while a
 *  server is running. */
void UA_Arduino_setArena(void* buffer, size_t size);

/** Read the arena's counters. Exhaustion is observable by design: a server
 *  that quietly stops answering is a far worse failure than one that tells
 *  you it ran out. `failures` should be zero in a healthy build. */
void UA_Arduino_getArenaStats(UA_Arduino_ArenaStats* stats);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPEN62541_ARDUINO_H */
