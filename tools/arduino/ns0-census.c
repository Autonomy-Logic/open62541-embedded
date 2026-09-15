/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 *    Copyright 2026 (c) Autonomy Logic
 */

/*
 * ns0-census -- walk a MINIMAL server's namespace zero and report what a
 * flash-resident version would have to supply.
 *
 * A host tool, and the first step of making namespace zero const: the only
 * trustworthy source is a real server after run_startup rather than a reading of
 * the nodeset XML.
 *
 * Build (from the repo root, against a host build of the library):
 *
 *   cmake -S . -B build-host -DUA_NAMESPACE_ZERO=MINIMAL ...
 *   cmake --build build-host --target open62541
 *   cc -std=c99 -Iinclude -Ibuild-host/src_generated -Iplugins/include \
 *      -Iarch -Ideps tools/arduino/ns0-census.c build-host/bin/libopen62541.a \
 *      -o ns0-census
 */
#include <open62541/server.h>
#include <open62541/server_config_default.h>
#include <open62541/plugin/nodestore.h>
#include <stdio.h>

static int n_nodes = 0, n_refs = 0, n_locales = 0;
static size_t bytes_names = 0, bytes_values = 0;

static void visit(void *ctx, const UA_Node *node) {
    (void)ctx;
    const UA_NodeHead *h = &node->head;
    n_nodes++;
    bytes_names += h->browseName.name.length;
    for (const UA_LocalizedTextListEntry *e = h->displayName; e; e = e->next) {
        n_locales++;
        bytes_names += e->localizedText.text.length + e->localizedText.locale.length;
    }
    for (size_t i = 0; i < h->referencesSize; i++)
        n_refs += (int)h->references[i].targetsSize;
    if (h->nodeClass == UA_NODECLASS_VARIABLE) {
        const UA_VariableNode *v = &node->variableNode;
        if (v->valueSourceType == UA_VALUESOURCETYPE_INTERNAL &&
            v->valueSource.internal.value.hasValue) {
            const UA_DataType *t = v->valueSource.internal.value.value.type;
            bytes_values += t ? t->memSize : 0;
        }
    }
    printf("  ns=%u id=", h->nodeId.namespaceIndex);
    if (h->nodeId.identifierType == UA_NODEIDTYPE_NUMERIC) printf("%u", h->nodeId.identifier.numeric);
    else printf("(non-numeric)");
    printf(" class=%d refs=%u name=%.*s\n", (int)h->nodeClass,
           (unsigned)h->referencesSize,
           (int)h->browseName.name.length, (char*)h->browseName.name.data);
}

int main(void) {
    UA_Server *s = UA_Server_new();
    UA_ServerConfig *c = UA_Server_getConfig(s);
    UA_ServerConfig_setMinimal(c, 4840, NULL);
    UA_StatusCode rc = UA_Server_run_startup(s);
    printf("run_startup: %s\n\n", UA_StatusCode_name(rc));
    c->nodestore->iterate(c->nodestore, visit, NULL);
    printf("\n--- namespace zero census ---\n");
    printf("nodes           : %d\n", n_nodes);
    printf("reference targets: %d\n", n_refs);
    printf("locale entries  : %d\n", n_locales);
    printf("name bytes      : %zu\n", bytes_names);
    printf("value bytes     : %zu\n", bytes_values);
    UA_Server_delete(s);
    return 0;
}
