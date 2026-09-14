# DOPE-636 — Reducing open62541 RAM consumption on baremetal targets

Phase 0: investigation and measurement. Everything below was measured on real
hardware — a Siemens LOGO! 8.2 (TI Tiva TM4C1294, Cortex-M4F, 248 KB SRAM)
running the OpenPLC baremetal runtime with the amalgamated
`UA_ARCHITECTURE=none` Arduino library built from this repo at `c30dee7`.

## Why this card exists

The baremetal OPC-UA server reserves a **fixed 64 KB arena** (`opcua_arena.cpp`
in the editor's `resources/sources/Baremetal/`). On a part with 248 KB of SRAM,
of which lwIP already takes ~100 KB, a fixed 64 KB block is the single largest
line item in the budget, and it is reserved whether or not a client ever
connects. The arena exists because open62541 calls `UA_malloc` on paths we
cannot avoid, and an unbounded newlib heap shared with the PLC application is
not something we are willing to ship. The question this card answers is how
much of that 64 KB is actually necessary.

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
   gone, which is why it is last.
6. **Should we pivot to another OPC-UA stack or an existing fork?** **No.** The
   fork we would be looking for is upstream v1.5.8 itself: the ROM-nodestore
   seam is already there. Pivoting would discard a working, hardware-validated,
   MPL-2.0 port to gain a seam we already have.

## Implementation plan

The library question comes first. The flash-resident nodestore is the only part
of this card with genuine unknowns, it is the largest single line item, and it
is what determines what the arena can eventually become — so it leads, and
everything else is sized around what it turns out to allow.

An earlier draft of this plan opened with the cheap platform-layer statics
instead. That was ordering by cost rather than by uncertainty: those statics
de-risk nothing for the nodestore, and one of them (the `FlashNodestore` pool)
lives in the very file the nodestore work rewrites, so doing it first would
have been work done twice.

### Phase 1 — flash-resident namespace zero (this repo)

1. Add `UA_NAMESPACE_ZERO=NONE` as a generator option in
   `tools/arduino/generate-arduino-library.sh`, producing a second library
   variant. Keep MINIMAL as the default until this phase is proven.
2. Write a host-side generator that builds a MINIMAL server, walks the
   nodestore after `run_startup`, and emits a `const` ns0 table (nodes,
   references, locales, browse names) plus an index. The host build being the
   source of truth is what keeps the table honest across upstream bumps.
3. Rewrite the flash nodestore to serve ns0 from that table and drop the inner
   RAM store. It is allocated statically from the start — the 1,680-byte pool
   is part of this phase, not a separate one.
4. Confirm `initNS0_dataSources()` binds every dynamic callback it expects.

- Expected: −18,992 static (ns0) −1,680 (pool) = **−20,672**.
- Risk: medium, and concentrated here. Everything unknown about this card is
  in step 2 and step 3.
- Verify: browse the full address space from a reference client and diff
  against the MINIMAL build's — they must be identical.

### Phase 2 — replace the fixed arena with a computed one

What Phase 1 leaves behind decides this, which is why it cannot be planned in
detail yet. Derive the arena size from declared project settings, have the VPP
declare a *maximum* rather than a fixed size, and keep the exhaustion counter
so overruns stay observable rather than silent.

### Orthogonal — `/arch` statics

Independent of both phases above: needs no upstream change, informs no
nodestore decision, and can land on its own schedule. It is library-layer work
in the sense that matters (it fills upstream's `/arch` slot), but it is work we
can do unilaterally.

Make `ArduinoTcpCM` (8,416) and the `UA_EventLoop_new_Arduino` allocation
(1,272) file-scope statics, and give `cm_alloc` a two-slot static send buffer
(8,192 + 256, matching the measured `n=2`) with an arena fallback (8,240 peak).

- Expected: −9,688 static, −8,240 peak.
- Risk: very low. No open62541 change, no protocol change.
- These files currently live in the editor's `resources/sources/Baremetal/`,
  which is a poor home for something that is architecturally a `/arch` port —
  it is not editor UI, and it is not OpenPLC application logic. Where it
  belongs is an open question, but the answer is not this repo either: the
  fork's delta over upstream is deliberately just `tools/arduino/`, and a
  board-coupled connection manager in it would tax every future rebase. The
  candidate homes are the Arduino core and a shared baremetal runtime.

### Remaining one-shot config structures

`UA_Server_newWithConfig` (1,064), `UA_ServerConfig_addSecurityPolicyNone`
(624) and a tail of small strings — ~3,048 bytes. These would need upstream
changes for diminishing returns. Evaluate after Phase 1, do not assume.

### Projected outcome

| | static | peak |
|---|---:|---:|
| today | 33,408 | 44,816 |
| after Phase 1 (flash ns0) | 12,736 | 23,112 |
| + platform-layer statics | 3,048 | 6,216 |
| + config structures (optimistic) | ~1,360 | ~4,528 |

An arena of **8 KB instead of 64 KB**, returning ~56 KB of a 248 KB part —
before counting what a smaller `sendBufferSize` would buy on projects that do
not need the full 8,192-byte chunk.

Phase 1 alone takes the peak below 24 KB, which is already enough to justify
cutting the arena to 32 KB before anything else lands.

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
