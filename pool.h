
#ifndef POOL_H
#define POOL_H

#include "../cthread/cthread.h"
#include "callocator.inl"
#include "os.h"

static const int32_t pool_sizes[] = {
    // if pool block size is a power of two, it will align to that size.
    8,       16,      24,      32,      40,      48,      56,      64,          // 64k  8b
    72,      80,      88,      96,      104,     112,     120,     128,         // 128  8b
    144,     160,     176,     192,     208,     224,     240,     256,         // 256  16
    288,     320,     352,     384,     416,     448,     480,     512,         // 512  32
    576,     640,     704,     768,     832,     896,     960,     1024,        // 1m   64
    1152,    1280,    1408,    1536,    1664,    1792,    1920,    2048,        // 2m   128

    // 256m arena
    2304,    2560,    2816,    3072,    3328,    3584,    3840,    4096,        // 256   512
    4608,    5120,    5632,    6144,    6656,    7168,    7680,    8192,        // 512   256
    9216,    10240,   11264,   12288,   13312,   14336,   15360,   16384,       // 1024  128
    18432,   20480,   22528,   24576,   26624,   28672,   30720,   32768       // 2k    64
    };    // 256m     // rounds to 4m

static inline uint8_t size_to_pool(const size_t as)
{
    static const int bmask = ~0x7f;
    if ((bmask & as) == 0) {
        // the first 2 rows
        return (as >> 3) - 1;
    } else {
        int32_t lz = 63 - __builtin_clzll(as);
        const size_t base = 1ULL << lz;
        const size_t row = lz - 5;
        const uint64_t incr_exp = (2 + row);
        return row * 8 + (uint8_t)((size_t)((as - 1) - base) >> incr_exp);
    }
}
static inline bool pool_is_connected(Pool *p) { return p->prev != NULL || p->next != NULL; }
static inline bool pool_is_full(const Pool *p) { return p->num_used == p->thread_free_counter; }
static inline bool pool_is_fully_commited(const Pool *p) { return p->num_committed >= p->num_available; }
static inline uint8_t* pool_base_address(Pool *p)
{
    return (uint8_t*)(ALIGN_UP_2((uintptr_t)p + sizeof(Pool), p->alignment));
}
static inline void pool_post_free(Pool *p, Allocator* a)
{
    uint8_t pid = partition_id_from_addr((uintptr_t)p);
    size_t asize = area_size_from_partition_id(pid);
    Arena *arena = (Arena *)((uintptr_t)p & ~(asize - 1));
    int32_t pidx = p->idx >> 1;
    uint32_t range = get_range((uint32_t)pidx, arena->ranges);
    uint64_t new_mask = ((1ULL << range) - 1UL) << pidx;
    arena->ranges = arena->ranges & ~new_mask;
    arena->allocations = arena->allocations & ~new_mask;
    if(arena->allocations <= 1)
    {
        // add arena to free arenas.
        Partition* partition = &a->part_alloc->area[pid];
        int32_t aidx = partition_allocator_get_arena_idx_from_queue(a->part_alloc, arena, partition);
        partition->full_mask &= ~(1ULL << aidx);
    }
}

static inline void pool_post_reserved(Pool *p, Allocator* a)
{
    uint8_t pid = partition_id_from_addr((uintptr_t)p);
    size_t asize = area_size_from_partition_id(pid);
    Arena *arena = (Arena *)((uintptr_t)p & ~(asize - 1));
    uint32_t pidx = p->idx >> 1;
    uint32_t range = get_range((uint32_t)pidx, arena->ranges);
    uint64_t new_mask = ((1ULL << range) - 1UL) << pidx;
    arena->ranges |= apply_range(range, pidx);
    arena->allocations = arena->allocations | new_mask;
    arena->zero = arena->zero | new_mask;
    if(arena->allocations == UINT64_MAX)
    {
        Partition* partition = &a->part_alloc->area[pid];
        int32_t aidx = partition_allocator_get_arena_idx_from_queue(a->part_alloc, arena, partition);
        partition->full_mask |= (1ULL << aidx%64);
    }
    
}
static inline void pool_set_full(Pool *p, Allocator* a)
{
    pool_post_free(p, a);
    // the last piece was returned so make the first item the start of the free
    p->free = NULL;//(Block *)base_addr;
    p->num_committed = 0;
    init_heap((Heap *)p);
}
static inline void pool_move_deferred(Pool *p)
{
    // for every item in the deferred list.
    if (p->free != NULL) {
        return;
    }
    
    if(pool_is_full(p))
    {
        p->num_committed = 0;
        p->free = NULL;
    }
    else
    {
        p->free = p->deferred_free;
    }
    p->deferred_free = NULL;
}

static inline void *pool_extend(Pool *p)
{
    p->num_used++;
    return (pool_base_address(p) + (p->num_committed++ * p->block_size));
}

static inline bool pool_is_empty(Pool *p)
{
    if (p->num_used < p->num_available) {
        return false;
    }
    
    if (p->deferred_free != NULL) {
        p->free = p->deferred_free;
        p->deferred_free = NULL;
        return false;
    }
    
    return true;
}

static inline void pool_free_block(Pool *p, void *block, Allocator* a)
{
    --p->num_used;
    if (pool_is_full(p)) {
        pool_set_full(p, a);
        return;
    }

    ((Block *)block)->next = p->free;
    p->free = (Block *)block;
}

static inline void *pool_get_free_block(Pool *p, Allocator* a)
{
    uint8_t *base_addr = pool_base_address(p);
    if (p->num_used++ == 0) {
        pool_post_reserved(p, a);
        p->free = NULL;
        return base_addr;
    }
    
    uint32_t curr_idx = (uint32_t)(size_t)((uint8_t *)p->free - base_addr)/p->block_size;
    if (curr_idx >= p->num_available) {
        p->free = NULL;
        return NULL;
    }
    if(p->free == NULL)
    {
        return NULL;
    }
    p->free = p->free->next;
    return (void *)(base_addr + (curr_idx * p->block_size));
}

static inline void *pool_aquire_block(Pool *p, Allocator* a)
{
    if (p->free != NULL) {
        return pool_get_free_block(p, a);
    }
    if (!pool_is_fully_commited(p)) {
        return pool_extend(p);
    } else {
        if (p->deferred_free != NULL) {
            pool_move_deferred(p);
            return pool_get_free_block(p, a);
        }
        return NULL;
    }
}

void pool_init(Pool *p, const uint8_t pidx, const uint32_t block_idx, const int32_t psize);

#endif
