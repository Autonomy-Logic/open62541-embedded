# DOPE-636 — Separating the OPC-UA library from the runtime, and cutting its RAM

Phase 0: investigation and measurement. Everything below was measured on real
hardware — a Siemens LOGO! 8.2 (TI Tiva TM4C1294, Cortex-M4F, 248 KB SRAM)
running the OpenPLC baremetal runtime with the amalgamated
`UA_ARCHITECTURE=none` Arduino library built from this repo at `c30dee7`.

## What this card is

The card opened as "shrink the 64 KB arena". Measuring it turned up a
structural problem underneath: **library code is living in the application.**
The connection manager, the EventLoop, the nodestore and the allocator all sit
in the editor's `resources/sources/Baremetal/`, so this repo does not actually
ship a usable Arduino library — it ships an amalgamation that works only if you
also happen to have OpenPLC's platform layer. Optimising the arena without
fixing that would mean doing the work in the wrong repo and then moving it.

So the card is three topics, in order:

1. **Make `open62541-embedded` a properly hardware-abstracted Arduino
   library** — one any sketch on any core can `#include` and use.
2. **Move every open62541-related file out of the baremetal runtime** into
   that library, leaving the runtime with only application code: mapping PLC
   variables onto nodes, and driving the server from the scan cycle.
3. **Then optimise**, with the goal that RAM consumption is decoupled from the
   number of nodes — everything static.

Topic 3 is last because doing it first means doing it twice: the largest single
allocation lives in a file topic 2 moves.

### On the goal of topic 3

Worth stating precisely, because part of it is already true. RAM **already**
does not scale with the user's node count: project nodes are served from a
`const` flash table through a fixed `OPCUA_NODE_POOL_SLOTS` pool, materialised
on read and dematerialised after, and never enter the RAM nodestore. The 48
node allocations in the profile are namespace zero's, and 48 is a constant
regardless of project size.

So topic 3 is not "stop RAM scaling with nodes" — that holds today. It is stop
paying a fixed 33 KB for a node set that never changes, and stop allocating
structures whose sizes are known at compile time.

### A note on attribution, so the numbers are not misread

It is fair to call the current split a design mistake: library code belongs in
the library, and several allocations should always have been static. But the
measurements do not support blaming the footprint on it. **42% of the peak is
upstream's namespace zero**, which no structuring choice on our side would have
avoided, and the allocation style in our `/arch` code is upstream's own house
style — `arch/posix`, `arch/lwip` and `arch/zephyr` all malloc their state too.
Restructuring is worth doing on its own merits; it is not by itself what makes
the arena 64 KB.

## Method

`opcua_arena.cpp` was temporarily instrumented with a call-site allocation
profiler: every `opcua_arena_malloc` records `__builtin_return_address(0)`
into a site table, with a pointer→site map so `free` decrements the right
site. `calloc`/`realloc` set an override PC so the site attributed is *their*
caller rather than the wrapper. Sites are dumped over the runtime's TCP line
log and resolved with `arm-none-eabi-addr2line -f -C -i` against the linked
`.elf`.

Two dumps are taken: one after `UA_Server_run_startup()` (static baseline) and
one snapshotted at the arena's high-water mark (peak). The peak snapshot is
taken inside the allocator at the moment `in_use` reaches a new maximum, which
is the only way to see the composition of the transient — by the time a
periodic dump runs, the request scratch has already been freed.

All instrumentation was reverted after measurement and the device reflashed
with a clean build (verified: 313 OPC-UA reads/s, Modbus TCP responding).

## Measured results

### Static baseline — 33,408 bytes, no client connected

| bytes | n | call site | bucket |
|------:|--:|-----------|--------|
| 8,416 | 1 | `UA_ConnectionManager_new_Arduino_TCP` | **ours** |
| 7,376 | 48 | `newEntry` | ns0 |
| 3,416 | 129 | `UA_Array_copy` | ns0 |
| 2,640 | 1 | `UA_Nodestore_ZipTree` | ns0 |
| 2,080 | 95 | `addReferenceTarget` | ns0 |
| 1,920 | 48 | `addReferenceKind` ← `UA_Node_addReference` | ns0 |
| 1,680 | 1 | `opcua_nodestore_new` | **ours** |
| 1,560 | 48 | `UA_Node_insertOrUpdateLocale` | ns0 |
| 1,272 | 1 | `UA_EventLoop_new_Arduino` | config |
| 1,064 | 1 | `UA_Server_newWithConfig` | config |
| 624 | 1 | `UA_ServerConfig_addSecurityPolicyNone` | config |
| 600 | 16 | `UA_String_fromChars` | config |
| 328 | 1 | `UA_BinaryProtocolManager_new` | config |
| 240 | 2 | `UA_AccessControl_default` | config |
| 120 | 1 | `addEndpoint` | config |
| 32 | 1 | `addNamespace` | config |
| 24 | 1 | `UA_Log_Stdout_new` | config |
| 16 | 1 | `UA_Array_resize` | config |

Bucketed:

| bucket | bytes | share |
|--------|------:|------:|
| namespace zero (48 nodes + refs + locales + ziptree index) | 18,992 | 56.8% |
| **our own code** (TCP CM struct, flash nodestore) | 10,096 | 30.2% |
| one-shot config structures | 4,320 | 12.9% |
| **total** | **33,408** | |

### Growth — one client

| state | arena `in_use` |
|-------|---------------:|
| no client | 33,408 |
| one session established, between requests | 34,440 |
| peak, mid-request | 44,816 |

So:

- **per established session: 1,032 bytes** (`UA_Session_create` 256,
  `createServerSecureChannel` 296, `UA_Array_copy` ~480)
- **per in-flight request: 10,376 bytes**, of which
  `UA_ByteString_allocBuffer` (our `cm_alloc`) is **8,240** and
  `RefResult_init` (browse result array) is **1,928**

The request scratch is freed before the next request. The runtime is
single-threaded — `UA_Server_run_iterate` processes one request to completion
inside the scan cycle — so **there is never more than one request's scratch
live at a time, regardless of how many clients are connected.**

No leak: 12,442 reads returned `in_use` to exactly 33,408.

## Where the library ends and we begin

Worth settling before reading the findings, because "ours" and "the library's"
is the wrong axis to bucket these numbers on.

Upstream draws the line in `arch/README.md`:

- **`/src`** — the OS-independent core: protocol, services, encoding, address
  space.
- **`/arch`** — clock and EventLoop (networking, timers, interrupts). One per
  OS; upstream ships `posix`, `lwip`, `zephyr`, `freertos`.
- **`/plugins`** — nodestore, access control, logging, security policies,
  default config.
- **application** — everything above that.

`UA_ARCHITECTURE=none` means *ship no `/arch`; the integrator links their own*.
So `opcua_arch_tcp.cpp` is not application code — it fills the `/arch` slot,
and `opcua_nodestore.cpp` fills a `/plugins` slot (upstream's occupant there is
`ua_nodestore_ziptree.c`). Both are **library-layer code that we happen to have
authored**, because no upstream port fits this target.

That last part is not for want of trying by upstream. `arch/lwip` exists, and
the LOGO runs lwIP — but that port is built on `lwip/sockets.h` and
`lwip/tcpip.h` (`lwip_socket`, `lwip_accept`, `lwip_select`), which need
`LWIP_SOCKET=1` and `NO_SYS=0`: a real RTOS with a tcpip thread. The LOGO core
is `NO_SYS=1`, `LWIP_SOCKET=0`, `LWIP_NETCONN=0` — raw callback API, bare
superloop. Writing our own connection manager was forced, not a shortcut.

The right port for this repo is not lwIP anyway: it is **Arduino**. This repo
ships an Arduino library, so its `/arch` layer should target the Arduino
network API and work on any core — lwIP, WIZnet shield, ESP32 WiFi or
otherwise — rather than any one stack underneath it. See topic 1 in the plan below.

Bucketing the 44,816-byte peak by layer rather than by authorship:

| layer | bytes | share | authored by |
|-------|------:|------:|-------------|
| core `/src` — ns0 nodes, references, locales, ziptree index | 18,992 | 42% | upstream |
| core `/src` — config, session/channel, browse scratch | 6,216 | 14% | upstream |
| `/arch` — TCP connection manager, EventLoop, send buffer | 17,928 | 40% | us |
| `/plugins` — nodestore pool | 1,680 | 4% | us |
| application (OpenPLC runtime) | ~0 | ~0% | us |

**Essentially none of it is the user application.** `opcua_server.cpp` (init
plus `run_iterate` inside the scan cycle) and `opcua_nodes.cpp` (the node table
reading located PLC buffers) are the application layer, and they allocate
almost nothing: project nodes already come from a `const` flash table, and the
only application allocation is a small per-read value copy in `opcua_nodes.cpp`
that is freed immediately and never surfaced in the profile.

Nor is the 44% in our slots an implementation defect. Upstream's own
posix/lwip/zephyr connection managers malloc their state too — that is the
house style, and it is unremarkable on a host with virtual memory. It only
becomes a problem on a 248 KB part where an 8 KB receive buffer is 3% of all
SRAM.

What the split actually determines is **who we have to negotiate with**: the
56% in upstream's core needs upstream's cooperation, which we have via the
`UA_NAMESPACE_ZERO=NONE` seam; the 44% in our `/arch` and `/plugins` slots
needs nobody's. That is why the ordering in the plan below puts the part with
an external dependency first and the part we fully control on its own track.

## Findings

### 1. 30% of the static baseline is `/arch` and `/plugins` state, and it is trivially static

`UA_ConnectionManager_new_Arduino_TCP` allocates one `ArduinoTcpCM` struct
containing a `kRecvBufSize = 8192` receive buffer — 8,416 bytes, allocated
once, never freed, compile-time sized. `opcua_nodestore_new` allocates one
`FlashNodestore` containing an `OPCUA_NODE_POOL_SLOTS`-slot pool — 1,680
bytes, same story.

Both are singletons in the `/arch` and `/plugins` slots we fill
(`opcua_arch_tcp.cpp`, `opcua_nodestore.cpp`, in the editor's baremetal
sources). Making them
file-scope `static` objects instead of `UA_calloc` removes 10,096 bytes from
the arena requirement with **no open62541 change at all** and no behavioural
difference. This is the cheapest win available and it should land first.

### 2. The largest transient is also `/arch`, and is bounded by a number we set

The 8,240-byte peak allocation is `cm_alloc()` in `opcua_arch_tcp.cpp` calling
`UA_ByteString_allocBuffer(buf, bufSize)`, where `bufSize` is
`config.sendBufferSize` — which we already pin to 8192 because OPC-UA Part 6
§6.7.1 sets 8192 as the chunk floor. It is alloc'd and freed around a single
send. A static send buffer of exactly that size, with a busy flag and an arena
fallback, replaces it. (Peak shows `n=2`: one 8,192 buffer plus one ~32-byte
one, so the static path needs two slots, not one.)

Taken together, findings 1 and 2 account for **18,336 bytes — 41% of the
44,816-byte peak — in library-layer slots we fill ourselves, needing no
upstream change and no fork.** They are worth
having, but they answer none of this card's open questions, so they are
sequenced alongside the nodestore work rather than ahead of it.

### 3. `UA_NAMESPACE_ZERO=NONE` is already a first-class upstream configuration

This is the significant discovery of Phase 0. v1.5.8 already contains the seam
for a flash-resident namespace zero:

```c
#if defined(UA_GENERATED_NAMESPACE_ZERO) || defined(UA_NAMESPACE_ZERO_MINIMAL)
    res = initNS0(server);
#else
    /* NONE configuration: NS0 pre-loaded by external nodestore (e.g., ROM).
     * Only connect data sources for dynamic values like ServerTime, ... */
    res = initNS0_dataSources(server);
#endif
```

`initNS0_dataSources()` is a public function documented as *"for external
nodestores (e.g., ROM nodestore) that have NS0 pre-loaded"*. It attaches the
dynamic callbacks (`readCurrentTime`, `readStatus`, `readNamespaces`, …) to
nodes it expects to already exist, and `configureNS0()` deliberately does not
fail on missing nodes — *"this may be normal if some nodes don't exist in the
ROM"*.

Verified by building it: a `-DUA_NAMESPACE_ZERO=NONE` amalgamation configures
and builds cleanly with our exact option set. The only difference in the
generated `open62541.h` is `UA_NAMESPACE_ZERO_MINIMAL` going from defined to
undefined. **No fork, no patch — one CMake value.**

What upstream does *not* ship is a ROM nodestore implementation or a generator
for one. The only nodestore plugin in the tree is `ua_nodestore_ziptree.c`.
That implementation is ours to write, and it is the whole of the remaining
work — but it is additive plugin work against a supported seam, not surgery on
the core.

We are well positioned for it: `opcua_nodestore.cpp` already implements the
`UA_Nodestore` vtable and already serves our own project nodes from a `const`
flash table, delegating everything else to an inner RAM store. The change is to
make that flash table cover ns0 too and drop the inner store.

### 4. The ns0 node set is small and completely static

48 nodes, 95 references, 48 locale entries — created identically on every boot
by `minimalServerObject()` + `createNS0_base()`, and never modified afterwards
(we build with `UA_ENABLE_NODEMANAGEMENT=OFF`, so clients cannot add or delete
nodes). 18,992 bytes of RAM holding data that is bit-for-bit the same on every
device, every boot.

The generator should not hand-transcribe these. The safe design is to build the
MINIMAL server on the host, walk its nodestore after `run_startup`, and emit a
`const` C table — so the flash table is by construction exactly what MINIMAL
produces, and regenerating after an upstream bump is one command.

### 5. The 64 KB arena is sized for a transient, not for a working set

The arena holds 33,408 bytes at rest and peaks at 44,816. The 64 KB figure is
the peak rounded up with headroom. But the peak is the static baseline plus one
request's scratch, and that scratch is dominated by a buffer whose size we
choose. Once the flash nodestore and the platform-layer statics land, the arena is no longer sized by "how much does
open62541 need" but by a formula over the project's own settings.

## Answers to the Phase 0 questions

1. **Where do the 30,848 bytes of `UA_Server_new` go?** Measured in full above:
   57% ns0 nodes, 30% our own singletons, 13% one-shot config.
2. **Is open62541 really mallocing in our path?** Yes, but overwhelmingly at
   *startup*, not per request. Per-request allocation is 10,376 bytes and is
   dominated by a buffer our own `cm_alloc` requests.
3. **Can ns0 be flash-resident?** **Yes — go.** Upstream supports it via
   `UA_NAMESPACE_ZERO=NONE`, verified to build. Implementation is a nodestore
   plugin plus a generator, both additive.
4. **Can the arena be dropped entirely?** Not entirely, but it can shrink a lot.
   After the flash nodestore and the platform-layer statics, the remaining arena users are per-session structures
   (~1 KB each) and small request scratch. A ~8 KB arena looks reachable.
5. **Can the arena be computed rather than fixed?** Yes. Once the large fixed
   consumers are static, what remains scales with declared settings:
   `arena = base + sessions x 1,032 + request_scratch`, where the VPP declares
   a maximum. This becomes tractable only once the large fixed consumers are
   gone, which is why it is topic 3d.
6. **Should we pivot to another OPC-UA stack or an existing fork?** **No.** The
   fork we would be looking for is upstream v1.5.8 itself: the ROM-nodestore
   seam is already there. Pivoting would discard a working, hardware-validated,
   MPL-2.0 port to gain a seam we already have.

## Implementation plan

### Topic 1 — make this repo a properly hardware-abstracted Arduino library

The goal is that a sketch on any core can `#include` this library and use it,
with the OpenPLC runtime as one sketch among others rather than a special case.

**The obstacle is in Arduino, not in open62541.** `Client` is a real
abstraction — eleven pure virtuals. `Server` is not one at all; verified
identical across seven cores including the official ArduinoCore-API:

```cpp
class Server : public Print {
  public:
    virtual void begin() = 0;
};
```

There is no portable accept. `EthernetServer::available()` returns an
`EthernetClient` **by value**; `WiFiServer::available()` returns a `WiFiClient`
by value; neither overrides anything. So client-side protocols port for free
and server-side ones cannot.

**How other libraries solve it.** Surveyed eight, five distinct strategies:

| strategy | libraries | portability |
|----------|-----------|-------------|
| **A. Refuse to own the listener** — take `Client&`, sketch accepts | **ArduinoModbus** (Arduino SA's own), **aWOT**, **our Settimino fork** | every core, zero board macros |
| B. Template on `ServerType` | ESP8266WebServer, rp2040 WebServer | needs `ServerType::ClientType`; esp8266 had to add it to their own `WiFiServer` |
| C. Replace Arduino's abstraction in the core | ESP32 `WebServer` via `NetworkServer`/`NetworkClient` | within that core only |
| D. Enumerate every board | khoih-prog EthernetWebServer — 43 board macros in one header | many boards, by brute force |
| E. Bypass Arduino entirely | ESPAsyncWebServer (AsyncTCP over raw lwIP) | ESP32/ESP8266 only |
| F. Two-way `#ifdef` | Espalexa | 2 platforms |

**We take strategy A**, which is what Arduino themselves chose for their own
server library. `ModbusTCPServer` in full:

```cpp
#include <Client.h>                     // note: NOT <Server.h>
class ModbusTCPServer : public ModbusServer {
  void accept(Client& client);
  int  poll();
private:
  Client* _client;
};
```

The sketch owns the listener and does the accepting. aWOT is identical
(`Application::process(Client*)`, zero board macros, never includes
`<Server.h>`), and our own Settimino fork already does the strongest form of
it — `S7Server.h` includes only `<stdint.h>` and `<stddef.h>` and takes
complete frames, so it runs against any transport or none.

Strategies D and E are the warnings. D is the shape of today's
`baremetal_net.h` and of the `modbus_tcp.cpp` fall-through that already
produced three live defects. E buys a nicer API by giving up every core but
one.

So the library's entire public networking surface becomes:

```cpp
UA_StatusCode opcua_accept(Client& client);
```

This is feasible without fighting open62541: `cm_open` already treats the
listener as a fiction — `kListenerId = BM_NET_OPCUA_SLOTS + 1` exists purely to
satisfy the EventLoop's model while the real accept happens in
`bm_net::Listener`. Dropping `Listener::begin()` in favour of injected clients
is a change confined to our own connection manager.

An optional convenience adapter (a `template<class ServerT, class ClientT>`
listener plus `__has_include` typedefs for the common cores) can ship *on top*
of that API for users who want one-line setup. It must never be the interface
itself — that distinction is exactly what separates aWOT from khoih-prog.

Deliverables:

1. `arch/arduino/` — clock and EventLoop, following upstream's `arch/<name>/`
   convention so the delta over upstream stays additive. (Upstream's porting
   guide also directs ports to edit `plugins/ua_config_default.c`; we avoid
   touching it by shipping our own `UA_ServerConfig_setDefault_Arduino()`.)
2. The `Client&` accept API, with no `<Server.h>` include and no board macro
   anywhere in the library.
3. The optional listener adapter and typedefs.
4. An example sketch that is not OpenPLC, to prove the library stands alone.

### Topic 2 — move open62541 code out of the baremetal runtime

Everything in the editor's `resources/sources/Baremetal/` that is not
application code moves into this library:

| file | lines | destination |
|------|------:|-------------|
| `opcua_arch_tcp.cpp` | 436 | library `/arch` — already 0 board macros |
| `opcua_arch.cpp` | 449 | library `/arch` (EventLoop, clock) |
| `opcua_arch.h` | 36 | library `/arch` |
| `opcua_nodestore.cpp` | — | library `/plugins` |
| `opcua_arena.cpp/.h` | — | library (the `UA_malloc` singleton backing) |
| `baremetal_net.*` | 504 | **stays** — becomes the sketch's accept loop |

What stays in the runtime is application code only: `opcua_nodes.cpp` (mapping
located PLC variables onto nodes) and `opcua_server.cpp` (init, and driving
`UA_Server_run_iterate` from the scan cycle). `baremetal_net.h` keeps its board
table, which is correct — picking `EthernetServer` vs `WiFiServer` for *this
product* is a product decision, and under strategy A it is the sketch's job.

This also resolves the OPC-UA/S7Comm shared slot pool cleanly: the sketch owns
the accept loop for both, so "one pool, two protocols" stops being something
two libraries have to cooperate on.

Acceptance: the library builds and runs from a non-OpenPLC sketch; the runtime
contains no `UA_*` platform implementation; both repos' CI green.

### Topic 3 — optimise, so RAM is decoupled from node count

Only once topics 1 and 2 have settled where the code lives.

**3a. Flash-resident namespace zero.** `UA_NAMESPACE_ZERO=NONE` is already a
first-class upstream configuration (finding 3) — `initNS0_dataSources()` is
public and documented for external ROM nodestores, and a NONE amalgamation
builds cleanly with our option set. Add the generator variant, write a
host-side generator that builds a MINIMAL server and emits its nodestore as a
`const` table, and rewrite the nodestore to serve ns0 from it with no inner RAM
store. Statically allocated from the start.
**−18,992 (ns0) −1,680 (pool) = −20,672.** This is where the unknowns are.

**3b. Static `/arch` structures.** Having moved into the library, size the
receive buffer, the EventLoop and the send buffer from compile-time constants
rather than allocating them. **−9,688 static, −8,240 peak.**

**3c. Remaining one-shot config.** `UA_Server_newWithConfig` (1,064),
`UA_ServerConfig_addSecurityPolicyNone` (624) and a tail of strings, ~3,048
bytes. Needs upstream changes for diminishing returns — evaluate, do not
assume.

**3d. Computed arena.** What 3a–3c leave decides this. Derive the size from
declared project settings, have the VPP declare a *maximum* rather than a fixed
size, and keep the exhaustion counter so overruns stay observable.

### Projected outcome

Topics 1 and 2 move code without changing footprint. Topic 3 is where the
bytes come off:

| | static | peak |
|---|---:|---:|
| today | 33,408 | 44,816 |
| after 3a (flash ns0) | 12,736 | 23,112 |
| after 3b (static `/arch`) | 3,048 | 6,216 |
| after 3c (optimistic) | ~1,360 | ~4,528 |

An arena of **8 KB instead of 64 KB**, returning ~56 KB of a 248 KB part —
before counting what a smaller `sendBufferSize` would buy on projects that do
not need the full 8,192-byte chunk.

3a alone takes the peak below 24 KB, which already justifies cutting the arena
to 32 KB before anything else lands.

And the end state satisfies the objective: with ns0 in flash and `/arch`
structures static, what remains in the arena is per-session state (~1,032
bytes) and per-request scratch. Neither scales with the number of nodes —
which, for project nodes, is already true today.

## Notes for whoever picks this up

- The profiler is worth rebuilding rather than reinventing; it is small and it
  is the only way to see the peak composition. It lived in `opcua_arena.cpp`
  behind the same `OPCUA_DEBUG_LOG` switch as the TCP line log.
- Dump routines that consume their own state produce empty second dumps. The
  first version of this profiler zeroed `live_bytes` as it sorted, which made
  every dump after the first look like the arena was empty.
- `g_high_water` never resets, so a peak reading is the maximum since boot, not
  since the last dump. Reflash between scenarios or the numbers blend.
- `--gc-sections` will silently discard a static buffer nothing demonstrably
  reads. The existing arena carries `__attribute__((used))` for exactly this
  reason; every static buffer added by this card needs the same treatment, and
  the check is the map file, not the compiler.
