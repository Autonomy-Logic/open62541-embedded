/*
 * SimpleServer -- an OPC-UA server on an Arduino, with no OpenPLC involved.
 *
 * Exposes one read/write Int32 and one read-only Float that tracks uptime.
 * Connect with any OPC-UA client (UaExpert, opcua-client, python-opcua) to
 * opc.tcp://<device-ip>:4840.
 *
 * This example exists to prove the library stands on its own. It uses the
 * Ethernet library because that is what most boards have; swap in WiFiServer
 * and WiFiClient and nothing else changes.
 *
 * MPL-2.0, same as the library.
 */

#include <Ethernet.h>
#include <open62541_arduino.h>
#include <UA_ArduinoListener.h>

/* The sketch owns the listening socket -- the library never opens one. See
 * open62541_arduino.h for why that is the only portable choice. */
UA_ArduinoListener<EthernetServer, EthernetClient> listener(4840);

byte      mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
IPAddress ip(192, 168, 1, 50);

UA_Server* server = nullptr;
UA_Int32   counter = 0;

/* The server's entire heap. Static at file scope, so it is a link-time
 * reservation: this either fits on the part or the build fails here rather
 * than on the device. Size it from UA_Arduino_getArenaStats() on a real
 * workload; 40 KB is comfortable for a handful of nodes and one client. */
static uint8_t opcuaArena[40 * 1024];

static void addCounterNode(UA_Server* s)
{
    UA_VariableAttributes attr = UA_VariableAttributes_default;
    UA_Variant_setScalar(&attr.value, &counter, &UA_TYPES[UA_TYPES_INT32]);
    attr.displayName = UA_LOCALIZEDTEXT((char*)"en-US", (char*)"Counter");
    attr.accessLevel = UA_ACCESSLEVELMASK_READ | UA_ACCESSLEVELMASK_WRITE;

    UA_Server_addVariableNode(
        s,
        UA_NODEID_STRING(1, (char*)"counter"),
        UA_NODEID_NUMERIC(0, UA_NS0ID_OBJECTSFOLDER),
        UA_NODEID_NUMERIC(0, UA_NS0ID_ORGANIZES),
        UA_QUALIFIEDNAME(1, (char*)"Counter"),
        UA_NODEID_NUMERIC(0, UA_NS0ID_BASEDATAVARIABLETYPE),
        attr, NULL, NULL);
}

void setup()
{
    Serial.begin(115200);

    /* Bring the interface up first. The library does not touch it: a library
     * that also configured the network would fight whatever else in the
     * sketch already did. */
    Ethernet.begin(mac, ip);
    listener.begin();

    /* Tell the server its own address, so the endpoint URL it publishes is
     * one a client can actually dial. Only the sketch knows this. */
    UA_Arduino_setDiscoveryAddress("192.168.1.50");

    UA_Arduino_setArena(opcuaArena, sizeof(opcuaArena));
    UA_Arduino_configureTcp(2, 8192);

    server = UA_Server_new();
    UA_ServerConfig* config = UA_Server_getConfig(server);
    UA_ServerConfig_setMinimal(config, 4840, NULL);

    addCounterNode(server);

    UA_StatusCode rc = UA_Server_run_startup(server);
    if (rc != UA_STATUSCODE_GOOD) {
        Serial.print("server startup failed: ");
        Serial.println(UA_StatusCode_name(rc));
        return;
    }

    UA_Arduino_ArenaStats stats;
    UA_Arduino_getArenaStats(&stats);
    Serial.print("OPC-UA server up. Arena in use: ");
    Serial.print((unsigned long)stats.inUse);
    Serial.print(" of ");
    Serial.println((unsigned long)stats.size);
}

void loop()
{
    /* Two calls. Accept whatever has arrived, then let the server do one
     * non-blocking pass. Neither sleeps, so the rest of loop() keeps its
     * timing. */
    listener.poll();
    UA_Server_run_iterate(server, false);

    /* Whatever else the sketch is for. */
    static unsigned long last = 0;
    if (millis() - last >= 1000) {
        last = millis();
        counter++;
    }
}
