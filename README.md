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
