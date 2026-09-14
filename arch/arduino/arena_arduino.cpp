/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 *    Copyright 2026 (c) Autonomy Logic
 */

/*
 * A first-fit free-list allocator over one fixed static arena, installed as
 * open62541's UA_malloc/UA_free/UA_calloc/UA_realloc via
 * UA_ENABLE_MALLOC_SINGLETON.
 *
 * Deliberately a boring allocator. It is not trying to be fast: a server
 * allocates on session setup and per-request scratch, at network timescales,
 * not in a control loop's critical path. It is trying to be three things the
 * standard allocator on a microcontroller is not:
 *
 *   BOUNDED       the footprint is a link-time constant. The binary either
 *                 fits or it does not, decided on the developer's machine
 *                 rather than in the field.
 *   ISOLATED      a network-facing protocol parser cannot fragment the heap
 *                 the rest of the sketch depends on. A remote peer churning
 *                 sessions is contained here.
 *   OBSERVABLE    exhaustion increments a counter you can read, instead of
 *                 returning NULL into code that stops answering for reasons
 *                 nobody can see from outside.
 *
 * Layout: one arena carved into blocks, each with an 8-byte header holding its
 * size and a free flag. Free neighbours are coalesced on release so a
 * long-running server tends back toward one large block rather than sawing the
 * arena into unusable slivers.
 */

#include "arduino_internal.h"

#if UA_ARDUINO_ARENA_SIZE > 0

#include <string.h>

namespace {

/** 8-byte alignment: the strictest thing open62541 stores in allocated memory
 *  is a double / UA_DateTime. Aligning everything to 8 avoids having to reason
 *  per-type about it. */
constexpr size_t kAlign = 8;

struct BlockHeader
{
    uint32_t size;  // payload bytes, excluding this header
    uint32_t free;  // 1 = available. A whole word rather than a bit so the
                    // payload stays 8-byte aligned without extra padding.
};

constexpr size_t kHeader = sizeof(BlockHeader);

inline size_t align_up(size_t n) { return (n + (kAlign - 1)) & ~(kAlign - 1); }

/** The arena.
 *
 *  __attribute__((used)) is load-bearing. Arduino cores link with
 *  --gc-sections, and an array nothing demonstrably reads gets discarded --
 *  measured, silently, the whole of it. A dropped arena lets an over-budget
 *  build link cleanly and fail only on the device, which is the exact failure
 *  this arena exists to prevent. */
__attribute__((used)) alignas(kAlign) uint8_t g_arena[UA_ARDUINO_ARENA_SIZE];

bool     g_ready      = false;
uint32_t g_in_use     = 0;
uint32_t g_high_water = 0;
uint32_t g_failures   = 0;

inline BlockHeader* first_block() { return reinterpret_cast<BlockHeader*>(g_arena); }

inline BlockHeader* next_block(BlockHeader* b)
{
    uint8_t* p = reinterpret_cast<uint8_t*>(b) + kHeader + b->size;
    return (p >= g_arena + UA_ARDUINO_ARENA_SIZE) ? nullptr
                                                  : reinterpret_cast<BlockHeader*>(p);
}

void init_if_needed()
{
    if (g_ready)
        return;
    BlockHeader* b = first_block();
    b->size  = UA_ARDUINO_ARENA_SIZE - kHeader;
    b->free  = 1;
    g_in_use = 0;
    g_ready  = true;
}

/** Merge a block with every free block that follows it. */
void coalesce_forward(BlockHeader* b)
{
    for (;;)
    {
        BlockHeader* n = next_block(b);
        if (n == nullptr || !n->free)
            return;
        b->size += kHeader + n->size;
    }
}

/** Split a block if the remainder can hold a header plus useful payload.
 *  Without the kAlign floor this would carve off zero-byte blocks that can
 *  never be allocated but still cost a header to walk past. */
void split_if_worthwhile(BlockHeader* b, size_t want)
{
    if (b->size < want + kHeader + kAlign)
        return;
    BlockHeader* rest = reinterpret_cast<BlockHeader*>(
        reinterpret_cast<uint8_t*>(b) + kHeader + want);
    rest->size = static_cast<uint32_t>(b->size - want - kHeader);
    rest->free = 1;
    b->size    = static_cast<uint32_t>(want);
}

BlockHeader* header_of(void* p)
{
    return reinterpret_cast<BlockHeader*>(static_cast<uint8_t*>(p) - kHeader);
}

bool in_arena(const void* p)
{
    const uint8_t* c = static_cast<const uint8_t*>(p);
    return c > g_arena && c < g_arena + UA_ARDUINO_ARENA_SIZE;
}

void* arena_malloc(size_t size)
{
    if (size == 0)
        return nullptr;
    init_if_needed();

    const size_t want = align_up(size);

    // First fit, not best fit. Best fit costs a full walk on every allocation
    // to buy less fragmentation than forward coalescing already provides for
    // this workload: a few long-lived session objects plus short-lived
    // request scratch.
    for (BlockHeader* b = first_block(); b != nullptr; b = next_block(b))
    {
        if (!b->free || b->size < want)
            continue;
        split_if_worthwhile(b, want);
        b->free   = 0;
        g_in_use += kHeader + b->size;
        if (g_in_use > g_high_water)
            g_high_water = g_in_use;
        return reinterpret_cast<uint8_t*>(b) + kHeader;
    }

    g_failures++;
    return nullptr;
}

void arena_free(void* p)
{
    if (p == nullptr || !in_arena(p))
        return;
    BlockHeader* b = header_of(p);
    if (b->free)
        return;                       // double free: ignore rather than corrupt
    b->free   = 1;
    g_in_use -= kHeader + b->size;
    coalesce_forward(b);
}

void* arena_calloc(size_t n, size_t size)
{
    const size_t total = n * size;
    if (n != 0 && total / n != size)  // overflow
        return nullptr;
    void* p = arena_malloc(total);
    if (p != nullptr)
        memset(p, 0, total);
    return p;
}

void* arena_realloc(void* p, size_t size)
{
    if (p == nullptr)
        return arena_malloc(size);
    if (size == 0)
    {
        arena_free(p);
        return nullptr;
    }
    BlockHeader* b = header_of(p);
    if (b->size >= align_up(size))
        return p;                     // already big enough
    void* n = arena_malloc(size);
    if (n == nullptr)
        return nullptr;               // caller still owns p
    memcpy(n, p, b->size);
    arena_free(p);
    return n;
}

} // namespace

extern "C" void UA_Arduino_getArenaStats(UA_Arduino_ArenaStats* stats)
{
    if (stats == nullptr)
        return;
    init_if_needed();
    size_t largest = 0;
    for (BlockHeader* b = first_block(); b != nullptr; b = next_block(b))
    {
        if (b->free && b->size > largest)
            largest = b->size;
    }
    stats->size        = UA_ARDUINO_ARENA_SIZE;
    stats->inUse       = g_in_use;
    stats->highWater   = g_high_water;
    stats->largestFree = largest;
    stats->failures    = g_failures;
}

namespace {

/** Install ourselves as open62541's allocator.
 *
 *  UA_ENABLE_MALLOC_SINGLETON makes UA_malloc and friends function pointers
 *  rather than compile-time bindings, so they can be redirected here before
 *  the server is built.
 *
 *  Called from two places on purpose. The static constructor below puts it in
 *  place before setup() runs, which covers a sketch that allocates early; but
 *  static initialisation order ACROSS translation units is unspecified, so
 *  every entry point that could be the first to allocate calls it as well.
 *  Without the second path a sketch whose own static constructor reached
 *  open62541 first would allocate from the standard heap and free into the
 *  arena, or the reverse. */
void ua_arduino_install_allocator_impl()
{
    static bool done = false;
    if (done)
        return;
    UA_mallocSingleton  = arena_malloc;
    UA_freeSingleton    = arena_free;
    UA_callocSingleton  = arena_calloc;
    UA_reallocSingleton = arena_realloc;
    done = true;
}

struct ArenaInstaller
{
    ArenaInstaller() { ua_arduino_install_allocator_impl(); }
};
ArenaInstaller g_installer;

} // namespace

void ua_arduino_install_allocator(void)
{
    ua_arduino_install_allocator_impl();
}

#else  /* UA_ARDUINO_ARENA_SIZE == 0: use the standard allocator */

#include <string.h>

void ua_arduino_install_allocator(void) { }

extern "C" void UA_Arduino_getArenaStats(UA_Arduino_ArenaStats* stats)
{
    if (stats != nullptr)
        memset(stats, 0, sizeof(*stats));
}

#endif
