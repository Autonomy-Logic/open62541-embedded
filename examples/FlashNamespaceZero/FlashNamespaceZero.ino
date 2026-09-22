/*
 * FlashNamespaceZero -- an OPC-UA server whose namespace zero lives in flash.
 *
 * Requires the library built with UA_NS0=NONE. In that configuration upstream
 * expects an external nodestore to have namespace zero already loaded, and
 * UA_Nodestore_newFlash() serves it from the const table in ua_ns0_flash.c:
 * 48 nodes for 13,641 bytes of flash and no RAM, instead of 18,992 bytes of
 * heap rebuilt on every boot.
 *
 * The nodestore has to be installed BEFORE the server exists, because
 * UA_Server_newWithConfig() is what runs namespace-zero initialisation -- by
 * the time UA_Server_new() has returned it is far too late.
 *
 * MPL-2.0, same as the library.
 */

#include <Ethernet.h>
#include <open62541_arduino.h>
#include <UA_ArduinoListener.h>

UA_ArduinoListener<EthernetServer, EthernetClient> listener(4840);

byte      mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
IPAddress ip(192, 168, 2, 60);

/* Measured on hardware: 7,800 bytes at rest, 19,296 at peak with one client
 * connected. 24 KB leaves room for fragmentation; below ~20 KB the handshake
 * fails with BadOutOfMemory, because a session needs the 8 KB response chunk
 * on top of everything resident. */
static uint8_t opcuaArena[24 * 1024];
UA_Server *server = nullptr;

/* This sketch exposes no project nodes of its own, so the source answers "no
 * such node" for everything; namespace zero is the whole address space. */
static bool no_nodes(UA_UInt16, UA_UInt32, UA_VariableNode *, void *) { return false; }
static void no_release(UA_VariableNode *, void *) {}

void setup() {
    Serial.begin(115200);
    Ethernet.begin(mac, ip);
    listener.begin();
    UA_Arduino_setDiscoveryAddress("192.168.2.60");

    UA_Arduino_setArena(opcuaArena, sizeof(opcuaArena));
    UA_Arduino_configureTcp(1, 1024);

    UA_ServerConfig config;
    memset(&config, 0, sizeof(config));
    if (UA_ServerConfig_setMinimal(&config, 4840, NULL) != UA_STATUSCODE_GOOD) {
        Serial.println("config failed");
        return;
    }

    /* CONSTRAIN THE CONFIG. This is not optional on a microcontroller.
     *
     * "Minimal" names the namespace-zero size, not the resource limits: the
     * defaults are 100 sessions, a 512 MB maximum message and an unbounded chunk
     * count, sized for a server with an MMU. Leave them and the first client's
     * Hello asks for more than the whole arena.
     *
     * tcpBufSize is the chunk length in BOTH directions. 8192 is the Part 6
     * 6.7.1 floor and open62541 enforces it internally, so smaller does not work
     * and larger only costs memory. */
    config.tcpBufSize      = 8192;
    config.tcpMaxMsgSize   = 8192;   /* refuse to assemble more than one chunk */
    config.tcpMaxChunks    = 1;      /* both default to 0, meaning UNBOUNDED  */
    config.maxSessions     = 1;
    config.maxSecureChannels = 2;
    config.maxNodesPerRead = 20;

    UA_Arduino_FlashNodeSource src;
    memset(&src, 0, sizeof(src));
    src.materialise    = no_nodes;
    src.dematerialise  = no_release;
    src.namespaceIndex = 1;     /* nothing lives here in this example */
    src.context        = nullptr;

    UA_Nodestore *flash = UA_Nodestore_newFlash(&src, config.nodestore,
                                                config.logging, 8, true);
    if (flash == nullptr) { Serial.println("nodestore failed"); return; }
    config.nodestore = flash;

    server = UA_Server_newWithConfig(&config);
    if (server == nullptr) { Serial.println("server failed"); return; }

    UA_StatusCode rc = UA_Server_run_startup(server);
    Serial.print("run_startup: "); Serial.println(UA_StatusCode_name(rc));

    UA_Arduino_ArenaStats st;
    UA_Arduino_getArenaStats(&st);
    Serial.print("IP: "); Serial.println(Ethernet.localIP());
    Serial.print("arena in use after startup: ");
    Serial.print((unsigned long)st.inUse);
    Serial.print(" of "); Serial.println((unsigned long)st.size);
}

void loop() {
    listener.poll();
    if (server) UA_Server_run_iterate(server, false);
}
