#!/usr/bin/env bash
#
# Generate the Arduino library form of this fork.
#
# open62541 is a CMake project with generated sources; Arduino's build system
# cannot run CMake. `UA_ENABLE_AMALGAMATION` exists for exactly this case and
# emits a single open62541.c + open62541.h, which IS an Arduino-compatible
# source library once the option set is baked in.
#
# The result is assembled under $OUT (default: build-arduino/library) as:
#
#     library.properties
#     LICENSE
#     README.md
#     src/open62541.c
#     src/open62541.h
#
# CI publishes that tree to the `arduino-library` branch, which is this
# repository's default branch — so `arduino-cli lib install --git-url <repo>`
# installs a working library with no further steps.
#
# Usage: tools/arduino/generate-arduino-library.sh [output-dir]
#
# NOT named build-*.sh, and that is deliberate: upstream's .gitignore carries
# `**/build-*`, which silently swallowed this file the first time it was added.
# `git add` skipped it without a word, the commit claimed to contain it, and CI
# then failed on a script that was never in the repository. A name outside that
# pattern cannot be lost that way.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${1:-$ROOT/build-arduino/library}"
# The build directory is per-variant. Sharing one meant a second run reused the
# first's CMake cache and silently produced the wrong namespace-zero
# configuration -- a NONE build that was actually MINIMAL, which compiles and
# links and only misbehaves on the device.
BUILD="${BUILD:-$ROOT/build-arduino/cmake-${UA_NS0:-MINIMAL}}"

VERSION="$(sed -n 's/^set(OPEN62541_VER_MAJOR \([0-9]*\).*/\1/p' "$ROOT/CMakeLists.txt" | head -1)"
MINOR="$(sed -n 's/^set(OPEN62541_VER_MINOR \([0-9]*\).*/\1/p' "$ROOT/CMakeLists.txt" | head -1)"
PATCH="$(sed -n 's/^set(OPEN62541_VER_PATCH \([0-9]*\).*/\1/p' "$ROOT/CMakeLists.txt" | head -1)"
UA_VERSION="${VERSION:-1}.${MINOR:-5}.${PATCH:-8}"

echo "==> open62541 $UA_VERSION -> Arduino library"

mkdir -p "$(dirname "$BUILD")"

# The option set. Every OFF here is a deliberate size decision for a
# microcontroller; the two ON lines are not optional.
#
#   UA_ARCHITECTURE=none      the application supplies clock/EventLoop/
#                             ConnectionManager. On Arduino that is the
#                             sketch's network stack, which differs per board.
#   UA_ENABLE_MALLOC_SINGLETON  routes every allocation through swappable
#                             function pointers. Without it UA_malloc binds to
#                             the standard allocator at COMPILE time and an
#                             application's static arena is reserved, counted,
#                             and never used.
#   UA_ENABLE_DATATYPES_ALL / TYPEDESCRIPTION  forced ON: the upstream build
#                             does not compile with them OFF.
# UA_NS0=NONE builds the variant whose namespace zero comes from the const
# table in arch/arduino/ua_ns0_flash.c instead of being constructed in RAM at
# startup -- 18,992 bytes of heap traded for 13,641 of flash. MINIMAL stays the
# default until the NONE path has more hardware behind it.
cmake -S "$ROOT" -B "$BUILD" \
  -DUA_ARCHITECTURE=none \
  -DUA_ENABLE_AMALGAMATION=ON \
  -DUA_MULTITHREADING=0 \
  -DUA_LOGLEVEL=600 \
  -DUA_BUILD_EXAMPLES=OFF \
  -DUA_BUILD_UNIT_TESTS=OFF \
  -DUA_BUILD_TOOLS=OFF \
  -DUA_ENABLE_ENCRYPTION=OFF \
  -DUA_ENABLE_SUBSCRIPTIONS=OFF \
  -DUA_ENABLE_SUBSCRIPTIONS_EVENTS=OFF \
  -DUA_ENABLE_METHODCALLS=OFF \
  -DUA_ENABLE_HISTORIZING=OFF \
  -DUA_ENABLE_DISCOVERY=OFF \
  -DUA_ENABLE_DIAGNOSTICS=OFF \
  -DUA_ENABLE_NODEMANAGEMENT=OFF \
  -DUA_ENABLE_JSON_ENCODING=OFF \
  -DUA_ENABLE_XML_ENCODING=OFF \
  -DUA_ENABLE_PUBSUB=OFF \
  -DUA_ENABLE_DA=OFF \
  -DUA_ENABLE_DATATYPES_ALL=ON \
  -DUA_ENABLE_TYPEDESCRIPTION=ON \
  -DUA_NAMESPACE_ZERO="${UA_NS0:-MINIMAL}" \
  -DUA_ENABLE_MALLOC_SINGLETON=ON \
  -DCMAKE_BUILD_TYPE=MinSizeRel \
  > "$BUILD.configure.log" 2>&1 || { tail -30 "$BUILD.configure.log"; exit 1; }

cmake --build "$BUILD" --target open62541-amalgamation -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)" \
  > "$BUILD.build.log" 2>&1 || { tail -30 "$BUILD.build.log"; exit 1; }

[ -f "$BUILD/open62541.c" ] || { echo "error: amalgamation did not produce open62541.c" >&2; exit 1; }

# The amalgamation must carry a freestanding branch. If this fires, the "let the
# amalgamation honour UA_ARCHITECTURE=none" commit has been lost in a rebase onto
# a newer upstream — which would otherwise only surface as a confusing
# sys/ioctl.h error in a user's Arduino build.
#
# It checks for the branch rather than for the ABSENCE of `#include <sys/...>`,
# which is what it used to do and which was a false positive: the fix works by
# putting the POSIX include behind an `#else`, so the text is still present in
# a perfectly good amalgamation and the grep failed every build. The compile
# smoke test in CI is what actually proves nothing POSIX is reached — it builds
# for bare-metal ARM with no system headers available at all.
if ! grep -q 'UA_ARCHITECTURE_NONE' "$BUILD/open62541.c"; then
    echo "error: the amalgamation has no freestanding branch." >&2
    echo "       The UA_ARCHITECTURE=none amalgamation fix is missing." >&2
    exit 1
fi

# And the type tables must be const, or the library silently costs ~45 KB of
# RAM on every target that uses it.
if ! grep -q '^const UA_DataType UA_TYPES\[' "$BUILD/open62541.c"; then
    echo "error: UA_TYPES[] is not const — the const-type-tables commit is missing." >&2
    exit 1
fi

rm -rf "$OUT"; mkdir -p "$OUT/src/arduino"
cp "$BUILD/open62541.c" "$BUILD/open62541.h" "$OUT/src/"
cp "$ROOT/LICENSE" "$OUT/LICENSE"

# The Arduino platform layer: the clock, EventLoop, ConnectionManager and
# allocator that UA_ARCHITECTURE=none leaves for the integrator. Without these
# the amalgamation does not link, and every sketch would have to write them.
#
# Public headers go to src/ root and implementation to src/arduino/, because
# arduino-cli resolves a library's includes by basename at src/ root -- a
# header in a subdirectory is invisible to `#include <...>` while its .cpp is
# still compiled. Arduino compiles src/ recursively, so the split costs
# nothing.
ARCH="$ROOT/arch/arduino"
cp "$ARCH/open62541_arduino.h" "$ARCH/UA_ArduinoListener.h" "$OUT/src/"
cp "$ARCH/arduino_internal.h" "$ARCH/ua_ns0_flash.h" \
   "$ARCH"/*.cpp "$ARCH"/*.c                                "$OUT/src/arduino/"

# Examples are how anyone finds out the library stands on its own.
if [ -d "$ROOT/arch/arduino/examples" ]; then
    cp -R "$ROOT/arch/arduino/examples" "$OUT/examples"
fi

cat > "$OUT/library.properties" <<PROPS
name=open62541
version=$UA_VERSION
author=open62541 authors
maintainer=Autonomy Logic <noreply@autonomylogic.com>
sentence=OPC UA stack (open62541) packaged as a source library for Arduino.
paragraph=Amalgamated open62541 plus an Arduino platform layer: clock, cooperative EventLoop, TCP ConnectionManager and a bounded allocator. Runs on any core, because it never opens a listening socket -- your sketch accepts connections and hands them over, the one design that does not need a list of supported boards. Encryption, subscriptions, method calls, historizing, discovery, node management and PubSub are compiled out for size. Needs roughly 190 KB of flash plus a configurable arena, so it does not fit 8-bit parts. MPL-2.0.
category=Communication
url=https://github.com/Autonomy-Logic/open62541-embedded
architectures=*
PROPS

cat > "$OUT/README.md" <<'README'
# open62541 for Arduino

An OPC-UA server for microcontrollers. Amalgamated open62541 plus an Arduino
platform layer -- clock, cooperative EventLoop, TCP ConnectionManager and a
bounded allocator -- so a sketch can run a server without writing any of that.

## Quick start

```cpp
#include <Ethernet.h>
#include <open62541_arduino.h>
#include <UA_ArduinoListener.h>

UA_ArduinoListener<EthernetServer, EthernetClient> listener(4840);
UA_Server* server;

void setup() {
    Ethernet.begin(mac, ip);
    listener.begin();
    UA_Arduino_setDiscoveryAddress("192.168.1.50");

    server = UA_Server_new();
    UA_ServerConfig_setMinimal(UA_Server_getConfig(server), 4840, NULL);
    UA_Server_run_startup(server);
}

void loop() {
    listener.poll();
    UA_Server_run_iterate(server, false);
}
```

See `examples/SimpleServer`.

## Your sketch owns the listening socket

This library never opens one. That looks like extra work for two lines, and it
is the only design that runs on every core.

Arduino's `Client` is a real abstraction -- eleven pure virtuals. Arduino's
`Server`, on every core including the official ArduinoCore-API, is this in its
entirety:

```cpp
class Server : public Print {
  public:
    virtual void begin() = 0;
};
```

There is no portable accept. `EthernetServer::available()` returns an
`EthernetClient` *by value*; `WiFiServer::available()` returns a `WiFiClient` by
value; neither overrides anything. A library that owned the listener would have
to name concrete server classes behind board macros, and would then work only
on the boards someone remembered to add.

So we do what Arduino themselves do in ArduinoModbus: include `<Client.h>`,
never `<Server.h>`, name no board, and take connections from the sketch.
`UA_ArduinoListener` is a convenience on top of that -- if it does not suit your
core, call `UA_Arduino_acceptClient()` yourself and everything still works.

## Configuration

All compile-time, all overridable from the sketch or a build flag:

| macro | default | |
|---|---|---|
Runtime, because a generated project chooses them and a compile-time macro
could not reach this library anyway — arduino-cli does not put the sketch
include path on library compilation:

| call | |
|---|---|
| `UA_Arduino_setArena(buf, size)` | the server's entire heap; **you own the buffer**. Omit it and the standard allocator is used |
| `UA_Arduino_configureTcp(maxConns, recvSize)` | concurrent clients, receive buffer (clamped up to the 8192 Part 6 6.7.1 floor) |
| `UA_Nodestore_newFlash(..., poolSlots)` | simultaneously materialised nodes |
| `UA_Arduino_setTime(unixSeconds)` | wall clock, for boards with no RTC |
| `UA_Arduino_setDiscoveryAddress(host)` | what the endpoint URL advertises |

Compile-time, because nothing per-project depends on it:

| macro | default | |
|---|---|---|
| `UA_ARDUINO_MAX_TIMERS` | 24 | repeated callbacks the EventLoop can hold |
| `UA_ARDUINO_EPOCH_UNIX` | 2026-01-01 | wall clock before `UA_Arduino_setTime()` |

`UA_Arduino_getArenaStats()` reports usage, peak and refused allocations, so you
can size the arena from a real workload rather than a guess.

## Size

Roughly **190-200 KB of flash**, plus the arena and an 8 KB receive buffer in
RAM. A server currently needs about **33 KB of arena at rest**, so parts with
32 KB of RAM will link but exhaust the arena at runtime; that figure is the
subject of ongoing work. It does not fit 8-bit AVR at all. `architectures=*` is
a statement about portability, not about every board having room.

Built and linked against: rp2040, ESP32, STM32 (Nucleo-144), SAMD (P1AM-200)
and TI Tiva TM4C. The SAMD21 in a P1AM-100 has 32 KB of RAM total, so the
example's 40 KB arena does not fit — shrink it there, though a server needs
about 33 KB at rest, so that part is not currently a realistic target.

## What is compiled out

Encryption, subscriptions, method calls, historizing, discovery, node
management and PubSub. Namespace zero is `MINIMAL`.

## Install

    arduino-cli lib install --git-url https://github.com/Autonomy-Logic/open62541-embedded

(`--git-url` requires `library.enable_unsafe_install: true` in your
arduino-cli configuration.)

## Changes from upstream

All on the `arduino-embedded` branch, all offered upstream:

- **The amalgamation honours `UA_ARCHITECTURE=none`.** Upstream force-adds the
  POSIX clock and eventloop whenever amalgamation is on, so the freestanding
  amalgamation the option exists to serve would not compile.
- **The generated type tables are `const`.** Measured on Cortex-M4:
  **-45,504 bytes of RAM for +8 bytes of flash.**
- **`UA_ARCHITECTURE=none` declares the platform factories it calls.**
  `ua_config_default.c` references `UA_EventLoop_new_POSIX` and friends on every
  architecture, but they are declared only for POSIX and WIN32 -- so a
  freestanding build failed on an implicit declaration, which newer compilers
  treat as an error.
- **`dtoa` is renamed `ua_dtoa`.** The old newlib shipped with some SAMD
  toolchains declares a different `dtoa` in `<stdlib.h>`, and the collision
  made the amalgamation uncompilable there.

## Licence

MPL-2.0, unchanged from upstream, and the Arduino platform layer in
`arch/arduino/` is MPL-2.0 too. See `LICENSE`.
README

echo "==> library assembled at $OUT"
( cd "$OUT" && ls -la src/ )
