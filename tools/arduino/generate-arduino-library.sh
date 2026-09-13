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
BUILD="$ROOT/build-arduino/cmake"

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
  -DUA_NAMESPACE_ZERO=MINIMAL \
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

rm -rf "$OUT"; mkdir -p "$OUT/src"
cp "$BUILD/open62541.c" "$BUILD/open62541.h" "$OUT/src/"
cp "$ROOT/LICENSE" "$OUT/LICENSE"

cat > "$OUT/library.properties" <<PROPS
name=open62541
version=$UA_VERSION
author=open62541 authors
maintainer=Autonomy Logic <noreply@autonomylogic.com>
sentence=OPC UA stack (open62541) packaged as a source library for Arduino.
paragraph=Amalgamated build of open62541 with UA_ARCHITECTURE=none: the sketch supplies the clock, EventLoop and ConnectionManager, so the library is independent of any one board's network stack. Encryption, subscriptions, method calls, historizing, discovery, node management and PubSub are compiled out for size. Needs roughly 190 KB of flash and a few KB of RAM plus whatever address space you build, so it does not fit 8-bit parts. MPL-2.0.
category=Communication
url=https://github.com/Autonomy-Logic/open62541-embedded
architectures=*
PROPS

cat > "$OUT/README.md" <<'README'
# open62541 for Arduino

The [open62541](https://github.com/open62541/open62541) OPC UA stack, packaged
as a **source** Arduino library so it compiles for any architecture
`arduino-cli` supports rather than only the ones someone remembered to
cross-compile for.

This branch is **generated**. Do not edit it — changes belong on
`arduino-embedded`, and CI regenerates this from there.

## What it is

A single `open62541.c` / `open62541.h` pair, produced by upstream's
`UA_ENABLE_AMALGAMATION` with an option set chosen for microcontrollers:
`UA_ARCHITECTURE=none`, no encryption, no subscriptions, no method calls, no
historizing, no discovery, no node management, no PubSub, namespace zero
`MINIMAL`, and allocations routed through `UA_ENABLE_MALLOC_SINGLETON`.

`UA_ARCHITECTURE=none` means **the application supplies the platform layer** —
the clock, the EventLoop and the ConnectionManager. That is deliberate: it is
what keeps the library independent of any particular board's network stack.
You will need to provide those six symbols. The OpenPLC baremetal runtime is a
worked example.

## Size

Roughly **190 KB of flash** and a few KB of static RAM, plus whatever your
address space costs. It will not fit an 8-bit AVR. `architectures=*` is a
statement about portability, not about every board having room.

## Install

    arduino-cli lib install --git-url https://github.com/Autonomy-Logic/open62541-embedded

(`--git-url` requires `library.enable_unsafe_install: true` in your
arduino-cli configuration.)

## Changes from upstream

Two, both on the `arduino-embedded` branch and both offered upstream:

- **The amalgamation honours `UA_ARCHITECTURE=none`.** Upstream force-adds the
  POSIX clock and eventloop whenever amalgamation is on, so the freestanding
  amalgamation the option exists to serve would not compile.
- **The generated type tables are `const`.** Measured on Cortex-M4:
  **−45,504 bytes of RAM for +8 bytes of flash.**

## Licence

MPL-2.0, unchanged from upstream. See `LICENSE`.
README

echo "==> library assembled at $OUT"
( cd "$OUT" && ls -la src/ )
