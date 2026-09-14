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
#include "ua_ns0_flash.h"

namespace {

#ifdef UA_ARDUINO_NS0_FLASH

// ---------------------------------------------------------------------------
// Namespace zero, served from flash
//
// The 48 ns0 nodes are const (see ua_ns0_flash.c) and cost no RAM. Two of them
// are written during run_startup -- ServerArray and NamespaceArray, whose
// values are device-specific strings -- so those get a writable copy here and
// everything else is handed out as a pointer into flash.
// ---------------------------------------------------------------------------

struct Ns0Overlay
{
    UA_Node   node;
    UA_UInt32 id;
    bool      in_use;
};
Ns0Overlay g_ns0_overlay[UA_ARDUINO_NS0_OVERLAY_SLOTS];
uint32_t   g_ns0_overlay_exhausted = 0;

bool is_ns0(const UA_NodeId* id)
{
    return id != nullptr && id->namespaceIndex == 0 &&
           id->identifierType == UA_NODEIDTYPE_NUMERIC;
}

/** Binary search: the generated table is sorted by numeric id. */
const UA_Node* ns0_flash_find(UA_UInt32 numeric)
{
    size_t lo = 0, hi = ua_ns0_nodes_count;
    while (lo < hi)
    {
        const size_t mid = lo + (hi - lo) / 2;
        const UA_UInt32 v = ua_ns0_nodes[mid].head.nodeId.identifier.numeric;
        if (v == numeric) return &ua_ns0_nodes[mid];
        if (v < numeric)  lo = mid + 1;
        else              hi = mid;
    }
    return nullptr;
}

Ns0Overlay* ns0_overlay_find(UA_UInt32 numeric)
{
    for (auto& o : g_ns0_overlay)
        if (o.in_use && o.id == numeric) return &o;
    return nullptr;
}

/** The node as it stands: the overlay copy if one exists, else flash. */
const UA_Node* ns0_current(UA_UInt32 numeric)
{
    const Ns0Overlay* o = ns0_overlay_find(numeric);
    return o ? &o->node : ns0_flash_find(numeric);
}

/** A writable copy, made on first edit.
 *
 *  The copy is SHALLOW on purpose: references, browse name and display name
 *  keep pointing into flash, because nothing that edits an ns0 node touches
 *  them -- what startup writes is a value and a value-source callback. A deep
 *  copy would put the 1.2 KB this design exists to keep out of RAM straight
 *  back into it. */
UA_Node* ns0_overlay_get(UA_UInt32 numeric, const UA_Logger* logger)
{
    if (Ns0Overlay* o = ns0_overlay_find(numeric))
        return &o->node;
    const UA_Node* flash = ns0_flash_find(numeric);
    if (flash == nullptr)
        return nullptr;
    for (auto& o : g_ns0_overlay)
    {
        if (o.in_use) continue;
        o.node   = *flash;      // shallow: flash still owns names and references
        o.id     = numeric;
        o.in_use = true;
        return &o.node;
    }
    g_ns0_overlay_exhausted++;
    if (logger != nullptr)
    {
        UA_LOG_ERROR(logger, UA_LOGCATEGORY_SERVER,
                     "Flash ns0: overlay full (%u slots), node %u stays read-only. "
                     "Raise UA_ARDUINO_NS0_OVERLAY_SLOTS.",
                     (unsigned)UA_ARDUINO_NS0_OVERLAY_SLOTS, (unsigned)numeric);
    }
    return nullptr;
}

#else   /* MINIMAL: namespace zero lives in the inner store, as upstream builds it */
inline bool is_ns0(const UA_NodeId*)                    { return false; }
inline const UA_Node* ns0_current(UA_UInt32)            { return nullptr; }
inline UA_Node* ns0_overlay_get(UA_UInt32, const UA_Logger*) { return nullptr; }
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
    bool          ns0_flash;   // serve namespace zero from the const table
    UA_UInt16     ns;
    PoolSlot*     pool;          // follows the struct in one allocation
    uint16_t      poolSlots;
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
    const uintptr_t hi = (uintptr_t)&m->pool[m->poolSlots];
    return p >= lo && p < hi;
}

const UA_Node* materialise(FlashNodestoreImpl* m, UA_UInt32 numeric_id)
{
    for (uint16_t i = 0; i < m->poolSlots; i++)
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
                     "Pass a larger poolSlots to UA_Nodestore_newFlash().",
                     (unsigned)m->poolSlots);
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
    if (m->ns0_flash && is_ns0(nodeId))
        return ns0_current(nodeId->identifier.numeric);
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
    {
        const UA_NodeId id = UA_NodePointer_toNodeId(ptr);
        if (m->ns0_flash && is_ns0(&id))
            return ns0_current(id.identifier.numeric);
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
    {
        // Our own nodes are flash too: read-only by construction, and the
        // writeMask set by the source's materialise() is what tells a client
        // so rather than letting a write appear to succeed.
        return nullptr;
    }
    if (m->ns0_flash && is_ns0(nodeId))
        return ns0_overlay_get(nodeId->identifier.numeric, m->logger);
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
    if (m->ns0_flash && node != nullptr)
    {
        // A pointer into the flash table or the overlay belongs to neither the
        // pool nor the inner store; releasing it anywhere would be wrong.
        const uintptr_t p = (uintptr_t)node;
        const uintptr_t f0 = (uintptr_t)&ua_ns0_nodes[0];
        const uintptr_t f1 = (uintptr_t)&ua_ns0_nodes[ua_ns0_nodes_count];
        if (p >= f0 && p < f1)
            return;
        for (const auto& o : g_ns0_overlay)
            if (node == &o.node) return;
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
    if (m->ns0_flash)
    {
        // Browse asks by compact index; with ns0 in flash there is no tree to
        // consult, so the generated map answers.
        if ((size_t)refTypeIndex < ua_ns0_reftype_count)
            return &ua_ns0_reftype_ids[refTypeIndex];
        return nullptr;
    }
    return m->inner->getReferenceTypeId(m->inner, refTypeIndex);
}

void ns_iterate(UA_Nodestore* ns, UA_NodestoreVisitor visitor, void* visitorCtx)
{
    FlashNodestoreImpl* m = self(ns);
    if (m->ns0_flash)
    {
        for (size_t i = 0; i < ua_ns0_nodes_count; i++)
        {
            const UA_UInt32 id = ua_ns0_nodes[i].head.nodeId.identifier.numeric;
            visitor(visitorCtx, ns0_current(id));
        }
    }
    else
    {
        m->inner->iterate(m->inner, visitor, visitorCtx);
    }
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
                                    const UA_Logger* logger,
                                    uint16_t poolSlots,
                                    bool serveNamespaceZeroFromFlash)
{
    if (inner == nullptr || source == nullptr || source->materialise == nullptr ||
        source->dematerialise == nullptr)
        return nullptr;
    if (poolSlots == 0)
        poolSlots = UA_ARDUINO_DEFAULT_NODE_POOL_SLOTS;
    // Struct and pool in one allocation: the slot count is a runtime value, and
    // one block is one thing to account for in the arena.
    FlashNodestoreImpl* m = (FlashNodestoreImpl*)UA_calloc(
        1, sizeof(FlashNodestoreImpl) + (size_t)poolSlots * sizeof(PoolSlot));
    if (m == nullptr)
        return nullptr;
    m->pool      = reinterpret_cast<PoolSlot*>(reinterpret_cast<uint8_t*>(m) +
                                               sizeof(FlashNodestoreImpl));
    m->poolSlots = poolSlots;

    m->inner  = inner;
    m->ns     = source->namespaceIndex;
    m->source = *source;
    m->logger    = logger;
    m->ns0_flash = serveNamespaceZeroFromFlash;

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

