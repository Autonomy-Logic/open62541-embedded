# SimpleServer

An OPC-UA server on a plain Arduino sketch: one writable Int32, no OpenPLC, no
board-specific code in the library.

Set `mac`, `ip` and the address passed to `UA_Arduino_setDiscoveryAddress()` to
match your network, flash it, then connect a client to
`opc.tcp://<your-ip>:4840`.

## Using a different network stack

Change two type arguments:

```cpp
#include <WiFi.h>
UA_ArduinoListener<WiFiServer, WiFiClient> listener(4840);
```

Nothing in the library changes, because the library never names either type.

## Using no adapter at all

`UA_ArduinoListener` is a convenience. If your core's server class does not fit
it, accept connections yourself and hand them over:

```cpp
EthernetClient pool[4];          // storage that outlives loop()

void loop() {
    EthernetClient c = server.accept();
    if (c) {
        for (int i = 0; i < 4; i++) {
            if (!pool[i]) { pool[i] = c; UA_Arduino_acceptClient(&pool[i]); break; }
        }
    }
    UA_Server_run_iterate(server, false);
}
```

The client object must stay at that address until the library says it is done
with it — register `UA_Arduino_setClosedCallback()` to find out when.

## Memory

`UA_ARDUINO_ARENA_SIZE` (default 64 KB) is the server's entire heap. Read
`UA_Arduino_getArenaStats()` to see what a real workload uses and size it down;
`failures` counts allocations refused, and should stay zero.
