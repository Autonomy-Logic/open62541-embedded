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
management and PubSub. Namespace zero is served from a const flash table
(`arch/arduino/ua_ns0_flash.c`) rather than reconstructed in SRAM.

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
