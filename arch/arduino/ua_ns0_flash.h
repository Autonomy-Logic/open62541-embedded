/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 *    Copyright 2026 (c) Autonomy Logic
 */

/* Flash-resident namespace zero: the table, and the one macro that makes it
 * portable. */

#ifndef UA_NS0_FLASH_H
#define UA_NS0_FLASH_H

#include <open62541.h>

/* Namespace zero comes from this table exactly when the library was NOT built
 * with UA_NAMESPACE_ZERO=MINIMAL -- i.e. the NONE configuration, where upstream
 * expects an external nodestore to have ns0 pre-loaded. Deriving it from the
 * library's own configuration rather than a separate switch means the two
 * cannot disagree. */
#ifndef UA_NAMESPACE_ZERO_MINIMAL
#  define UA_ARDUINO_NS0_FLASH 1
#endif

/* A UA_NodePointer holding a small numeric NodeId inline.
 *
 * The packing differs between 32- and 64-bit builds, so this cannot be a value
 * the generator computes on the host: it has to be an expression the TARGET
 * compiler evaluates. Mirrors UA_NodePointer_fromNodeId() in ua_nodes.c. The
 * generator refuses any target that is not immediate-encodable (namespace >=
 * 64 or identifier >= 2^24 on 32-bit), so the fallback branch there has no
 * equivalent here. */
#if SIZE_MAX > UA_UINT32_MAX
#  define UA_NS0_NP(ns, num) {.immediate = (((uintptr_t)(num)) << 32) | (((uintptr_t)(ns)) << 8)}
#else
#  define UA_NS0_NP(ns, num) {.immediate = (((uintptr_t)(num)) << 8)  | (((uintptr_t)(ns)) << 2)}
#endif

/** The namespace-zero nodes, sorted by (namespaceIndex, numeric id) so a
 *  lookup is a binary search rather than a walk. */
extern const UA_Node ua_ns0_nodes[];
extern const size_t  ua_ns0_nodes_count;

/** referenceTypeIndex -> NodeId. Browse resolves reference types by a compact
 *  byte index and the default nodestore answers from its own tree; with
 *  namespace zero in flash there is no tree to ask. */
extern const UA_NodeId ua_ns0_reftype_ids[];
extern const size_t    ua_ns0_reftype_count;

/** Writable copies for the namespace-zero nodes the server edits at startup.
 *
 *  Measured, not guessed: exactly two, ServerArray (2254) and NamespaceArray
 *  (2257), are written during run_startup -- their values are device-specific
 *  strings, so they were always going to need RAM. Everything else is served
 *  straight out of flash. The slot count is a ceiling with headroom, and
 *  exhaustion is reported rather than silently dropping a write. */
#ifndef UA_ARDUINO_NS0_OVERLAY_SLOTS
#define UA_ARDUINO_NS0_OVERLAY_SLOTS 4
#endif

#endif /* UA_NS0_FLASH_H */
