/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 *    Copyright 2026 (c) Autonomy Logic
 */

/*
 * A read-only nodestore that keeps nodes in flash and materialises them on
 * demand into a small fixed pool.
 *
 * An embedded server's address space is decided when the sketch is compiled
 * and never changes at runtime. Keeping it in RAM therefore pays, per node,
 * for something that is already `const` in flash. Measured on a Cortex-M4
 * against open62541's default zip-tree nodestore:
 *
 *      2 nodes    30,840 -> 32,248 bytes of heap
 *     40 nodes    30,840 -> 49,872 bytes          =  476 B/node
 *
 * and at 40 nodes a Browse then failed outright with BadOutOfMemory: 49,872 of
 * a 65,536-byte arena was gone, the largest free block was 13,440 bytes, and
 * there was nowhere left to build the response. So this is not a tuning
 * exercise. Without it a constrained server cannot serve a realistic address
 * space at all.
 *
 * What remains in RAM is the parent folder's forward references, 8 bytes per
 * node, instead of 476.
 *
 * It WRAPS rather than replaces the default nodestore. Namespace zero is
 * large, mutable during startup and not yours, so anything outside your
 * namespace is delegated untouched. (Making namespace zero itself flash-
 * resident is a separate and larger piece of work.)
 *
 * You supply the two callbacks in UA_Arduino_FlashNodeSource: one to fill a
 * node from whatever `const` table your build generated, one to release
 * whatever that allocated. Everything else here is generic.
 */



#include <string.h>


#include "arduino_internal.h"

namespace {

/** How many of our nodes open62541 may hold materialised at once.
 *
 *  Bounded by the OperationLimits (§4.6): a Read walks its nodes one at a
 *  time, a Browse holds the browsed node plus what it is looking at. The pool
 *  is deliberately small, and exhaustion is counted rather than silently
 *  tolerated -- a pool that is too small must show up in test. */
#ifndef UA_ARDUINO_NODE_POOL_SLOTS
#define UA_ARDUINO_NODE_POOL_SLOTS 8
#endif

struct PoolSlot
{
    UA_VariableNode node;
    bool            in_use;
};

struct FlashNodestoreImpl
{
    UA_Nodestore  base;     // MUST be first: open62541 casts between the two
    UA_Nodestore* inner;    // owns namespace zero and anything not ours
    UA_Arduino_FlashNodeSource source;
    const UA_Logger* logger;
    UA_UInt16     ns;
    PoolSlot      pool[UA_ARDUINO_NODE_POOL_SLOTS];
    uint16_t      live;
    uint16_t      high_water;
    uint32_t      exhausted;
};

FlashNodestoreImpl* g_self = nullptr;

FlashNodestoreImpl* self(UA_Nodestore* ns) { return reinterpret_cast<FlashNodestoreImpl*>(ns); }

/** Is this one of ours? Only the namespace index decides, so a node in our
 *  namespace that was never declared resolves to "ours, and missing" rather
 *  than leaking into the inner store where it would also not be found. */
bool is_ours(FlashNodestoreImpl* m, const UA_NodeId* id)
{
    return id != nullptr && id->namespaceIndex == m->ns &&
           id->identifierType == UA_NODEIDTYPE_NUMERIC;
}

bool from_pool(FlashNodestoreImpl* m, const UA_Node* n)
{
    const uintptr_t p = (uintptr_t)n;
    const uintptr_t lo = (uintptr_t)&m->pool[0];
    const uintptr_t hi = (uintptr_t)&m->pool[UA_ARDUINO_NODE_POOL_SLOTS];
    return p >= lo && p < hi;
}

const UA_Node* materialise(FlashNodestoreImpl* m, UA_UInt32 numeric_id)
{
    for (uint16_t i = 0; i < UA_ARDUINO_NODE_POOL_SLOTS; i++)
    {
        if (m->pool[i].in_use)
            continue;
        if (!m->source.materialise(m->ns, numeric_id, &m->pool[i].node, m->source.context))
            return nullptr;   // no such node; not a pool problem
        m->pool[i].in_use = true;
        m->live++;
        if (m->live > m->high_water)
            m->high_water = m->live;
        return (const UA_Node*)&m->pool[i].node;
    }
    // Loudly, not quietly. A client that hits this gets a clean error and the
    // census shows why; a silently returned nullptr would look like a missing
    // node and send the next person hunting the address space instead.
    m->exhausted++;
    if (m->logger != nullptr)
    {
        UA_LOG_ERROR(m->logger, UA_LOGCATEGORY_SERVER,
                     "Flash nodestore: pool exhausted (%u slots). "
                     "Raise UA_ARDUINO_NODE_POOL_SLOTS.",
                     (unsigned)UA_ARDUINO_NODE_POOL_SLOTS);
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// UA_Nodestore vtable
// ---------------------------------------------------------------------------

void ns_free(UA_Nodestore* ns)
{
    FlashNodestoreImpl* m = self(ns);
    if (m->inner != nullptr && m->inner->free != nullptr)
        m->inner->free(m->inner);
    if (g_self == m)
        g_self = nullptr;
    UA_free(m);
}

UA_Node* ns_newNode(UA_Nodestore* ns, UA_NodeClass nodeClass)
{
    // Only the inner store ever creates nodes: ours come from flash and the
    // server never asks for one of those to be built.
    FlashNodestoreImpl* m = self(ns);
    return m->inner->newNode(m->inner, nodeClass);
}

void ns_deleteNode(UA_Nodestore* ns, UA_Node* node)
{
    FlashNodestoreImpl* m = self(ns);
    if (from_pool(m, node))
        return;   // flash-backed: nothing was allocated, nothing to free
    m->inner->deleteNode(m->inner, node);
}

const UA_Node* ns_getNode(UA_Nodestore* ns, const UA_NodeId* nodeId,
                          UA_UInt32 attributeMask, UA_ReferenceTypeSet references,
                          UA_BrowseDirection referenceDirections)
{
    FlashNodestoreImpl* m = self(ns);
    if (is_ours(m, nodeId))
        return materialise(m, nodeId->identifier.numeric);
    return m->inner->getNode(m->inner, nodeId, attributeMask, references,
                             referenceDirections);
}

const UA_Node* ns_getNodeFromPtr(UA_Nodestore* ns, UA_NodePointer ptr,
                                 UA_UInt32 attributeMask, UA_ReferenceTypeSet references,
                                 UA_BrowseDirection referenceDirections)
{
    FlashNodestoreImpl* m = self(ns);
    if (UA_NodePointer_isLocal(ptr))
    {
        const UA_NodeId id = UA_NodePointer_toNodeId(ptr);
        if (is_ours(m, &id))
            return materialise(m, id.identifier.numeric);
    }
    return m->inner->getNodeFromPtr(m->inner, ptr, attributeMask, references,
                                    referenceDirections);
}

/* An "edit" node is still a pool node.
 *
 * The write service takes the edit path even for a CALLBACK value source --
 * it needs the node to find the callback -- so refusing here would break
 * writes entirely. Nothing persistent is edited: the value goes to the
 * callback, and every other attribute is refused earlier by the zero
 * writeMask set by the source's materialise(). */
UA_Node* ns_getEditNode(UA_Nodestore* ns, const UA_NodeId* nodeId,
                        UA_UInt32 attributeMask, UA_ReferenceTypeSet references,
                        UA_BrowseDirection referenceDirections)
{
    FlashNodestoreImpl* m = self(ns);
    if (is_ours(m, nodeId))
        return (UA_Node*)(uintptr_t)materialise(m, nodeId->identifier.numeric);
    return m->inner->getEditNode(m->inner, nodeId, attributeMask, references,
                                 referenceDirections);
}

UA_Node* ns_getEditNodeFromPtr(UA_Nodestore* ns, UA_NodePointer ptr,
                               UA_UInt32 attributeMask, UA_ReferenceTypeSet references,
                               UA_BrowseDirection referenceDirections)
{
    FlashNodestoreImpl* m = self(ns);
    if (UA_NodePointer_isLocal(ptr))
    {
        const UA_NodeId id = UA_NodePointer_toNodeId(ptr);
        if (is_ours(m, &id))
            return (UA_Node*)(uintptr_t)materialise(m, id.identifier.numeric);
    }
    return m->inner->getEditNodeFromPtr(m->inner, ptr, attributeMask, references,
                                        referenceDirections);
}

void ns_releaseNode(UA_Nodestore* ns, const UA_Node* node)
{
    FlashNodestoreImpl* m = self(ns);
    if (node == nullptr)
        return;
    if (from_pool(m, node))
    {
        PoolSlot* slot = (PoolSlot*)(uintptr_t)node;   // node is first in PoolSlot
        if (slot->in_use)
        {
            m->source.dematerialise(&slot->node, m->source.context);
            slot->in_use = false;
            if (m->live > 0)
                m->live--;
        }
        return;
    }
    m->inner->releaseNode(m->inner, node);
}

UA_StatusCode ns_getNodeCopy(UA_Nodestore* ns, const UA_NodeId* nodeId, UA_Node** outNode)
{
    FlashNodestoreImpl* m = self(ns);
    if (is_ours(m, nodeId))
    {
        // A copy is the caller's to keep and to free, so it cannot share the
        // flash-resident names and reference arrays a pool node points at.
        // Nothing in this build asks for one -- the address space is not
        // edited at runtime -- so refusing is honest and avoids a deep copy
        // whose ownership rules would be easy to get wrong later.
        return UA_STATUSCODE_BADNOTSUPPORTED;
    }
    return m->inner->getNodeCopy(m->inner, nodeId, outNode);
}

UA_StatusCode ns_insertNode(UA_Nodestore* ns, UA_Node* node, UA_NodeId* addedNodeId)
{
    FlashNodestoreImpl* m = self(ns);
    if (node != nullptr && node->head.nodeId.namespaceIndex == m->ns)
    {
        // The project's namespace is compiled in, not built at runtime.
        m->inner->deleteNode(m->inner, node);
        return UA_STATUSCODE_BADNOTSUPPORTED;
    }
    return m->inner->insertNode(m->inner, node, addedNodeId);
}

UA_StatusCode ns_replaceNode(UA_Nodestore* ns, UA_Node* node)
{
    FlashNodestoreImpl* m = self(ns);
    if (node != nullptr && from_pool(m, node))
        return UA_STATUSCODE_GOOD;   // nothing to write back; flash is the truth
    return m->inner->replaceNode(m->inner, node);
}

UA_StatusCode ns_removeNode(UA_Nodestore* ns, const UA_NodeId* nodeId)
{
    FlashNodestoreImpl* m = self(ns);
    if (is_ours(m, nodeId))
        return UA_STATUSCODE_BADNOTSUPPORTED;
    return m->inner->removeNode(m->inner, nodeId);
}

const UA_NodeId* ns_getReferenceTypeId(UA_Nodestore* ns, UA_Byte refTypeIndex)
{
    FlashNodestoreImpl* m = self(ns);
    return m->inner->getReferenceTypeId(m->inner, refTypeIndex);
}

void ns_iterate(UA_Nodestore* ns, UA_NodestoreVisitor visitor, void* visitorCtx)
{
    FlashNodestoreImpl* m = self(ns);
    m->inner->iterate(m->inner, visitor, visitorCtx);
    // Enumerating our own nodes is optional: a source that cannot list them
    // simply is not walked, which costs a Browse of the whole address space
    // from the server's own housekeeping and nothing a client can see.
    if (m->source.count == nullptr || m->source.idAt == nullptr)
        return;
    const UA_UInt16 n = m->source.count(m->source.context);
    for (UA_UInt16 i = 0; i < n; i++)
    {
        const UA_Node* node = materialise(m, m->source.idAt(i, m->source.context));
        if (node == nullptr)
            continue;
        visitor(visitorCtx, node);
        ns_releaseNode(ns, node);
    }
}

} // namespace

UA_Nodestore* UA_Nodestore_newFlash(const UA_Arduino_FlashNodeSource* source,
                                    UA_Nodestore* inner,
                                    const UA_Logger* logger)
{
    if (inner == nullptr || source == nullptr || source->materialise == nullptr ||
        source->dematerialise == nullptr)
        return nullptr;
    FlashNodestoreImpl* m = (FlashNodestoreImpl*)UA_calloc(1, sizeof(FlashNodestoreImpl));
    if (m == nullptr)
        return nullptr;

    m->inner  = inner;
    m->ns     = source->namespaceIndex;
    m->source = *source;
    m->logger = logger;

    UA_Nodestore* ns          = &m->base;
    ns->free                  = ns_free;
    ns->newNode               = ns_newNode;
    ns->deleteNode            = ns_deleteNode;
    ns->getNode               = ns_getNode;
    ns->getNodeFromPtr        = ns_getNodeFromPtr;
    ns->getEditNode           = ns_getEditNode;
    ns->getEditNodeFromPtr    = ns_getEditNodeFromPtr;
    ns->releaseNode           = ns_releaseNode;
    ns->getNodeCopy           = ns_getNodeCopy;
    ns->insertNode            = ns_insertNode;
    ns->replaceNode           = ns_replaceNode;
    ns->removeNode            = ns_removeNode;
    ns->getReferenceTypeId    = ns_getReferenceTypeId;
    ns->iterate               = ns_iterate;

    g_self = m;
    return ns;
}

extern "C" void UA_Arduino_getNodestoreStats(uint16_t* outHighWater, uint32_t* outExhausted)
{
    if (outHighWater != nullptr)
        *outHighWater = (g_self != nullptr) ? g_self->high_water : 0;
    if (outExhausted != nullptr)
        *outExhausted = (g_self != nullptr) ? g_self->exhausted : 0;
}

