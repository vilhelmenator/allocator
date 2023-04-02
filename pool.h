
#ifndef POOL_H
#define POOL_H

#include "../cthread/cthread.h"
#include "section.h"
//
// 4k, 8k, 16k, 32k, 64k, 128k, 256, 512, 1, 2, 4, 8, 16, 32, 64, 128, 256
//
static const int32_t pool_sizes[] = {
    // if pool block size is a power of two, it will align to that size.
    8,       16,      24,      32,      40,      48,      56,      64,   // 64k    8b range and aligned to this size
    72,      80,      88,      96,      104,     112,     120,     128,  // 128    8b   allowes for smaller subrange allocations.
    144,     160,     176,     192,     208,     224,     240,     256,  // 256    16   1 sr if pool size is not a power of two.
    288,     320,     352,     384,     416,     448,     480,     512,  // 512    32   3 sr
    576,     640,     704,     768,     832,     896,     960,     1024, // 1m     64   7 sr
    1152,    1280,    1408,    1536,    1664,    1792,    1920,    2048, // 2m    128  15 sr

    // 256m arena
    2304,    2560,    2816,    3072,    3328,    3584,    3840,    4096,    // 256   (8 - 16 slots per allocation) ( 3 -> 7 allocations per arena block)
    4608,    5120,    5632,    6144,    6656,    7168,    7680,    8192,    // 512
    9216,    10240,   11264,   12288,   13312,   14336,   15360,   16384,   // 1024
    18432,   20480,   22528,   24576,   26624,   28672,   30720,   32768,   // 2k
    
    // point to arenas
    // these sizes just get mapped to arenas:
    36864,   40960,   45056,   49152,   53248,   57344,   61440,   65536,       // 4m       // rounds to 65k
    73728,   81920,   90112,   98304,   106496,  114688,  122880,  131072,      // 8m       // rounds to 128k
    147456,  163840,  180224,  196608,  212992,  229376,  245760,  262144,      // 16m      // rounds to 256k
    294912,  327680,  360448,  393216,  425984,  458752,  491520,  524288,      // 32m      // rounds to 512k
    589824,  655360,  720896,  786432,  851968,  917504,  983040,  1048576,     // 64m      // rounds to 1m
    1179648, 1310720, 1441792, 1572864, 1703936, 1835008, 1966080, 2097152,     // 128m     // rounds to 2m
    2359296, 2621440, 2883584, 3145728, 3407872, 3670016, 3932160, 4194304};    // 256m     // rounds to 4m

static inline int32_t get_pool_size_class(size_t s)
{
    if (s <= SMALL_OBJECT_SIZE) {         // 8 - 16k
        return ST_POOL_128K;              // 128k
    } else if (s <= MEDIUM_OBJECT_SIZE) { // 16k - 128k
        return ST_POOL_512K;              // 512k
    } else {
        return ST_POOL_4M; // 4M for > 128k objects.
    }
}

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
static inline uint8_t* pool_base_address(Pool *p)
{
    return (uint8_t*)(ALIGN_UP_2((uintptr_t)p + sizeof(Pool), p->alignment));
}
static inline void pool_post_free(Pool *p, Allocator* a)
{
#if defined(ARENA_PATH)
    uint8_t pid = partition_id_from_addr((uintptr_t)p);
    size_t asize = area_size_from_partition_id(pid);
    Arena *arena = (Arena *)((uintptr_t)p & ~(asize - 1));
    int32_t pidx = p->idx >> 1;
    uint32_t range = get_range((uint32_t)pidx, arena->ranges);
    uint64_t new_mask = ((1ULL << range) - 1UL) << pidx;
    arena->ranges = arena->ranges & ~new_mask;
    arena->allocations = arena->allocations & ~new_mask;
    if(arena->allocations == 1)
    {
        // add arena to free arenas.
        Partition* partition = &a->part_alloc->area[pid];
        int32_t aidx = partition_allocator_get_arena_idx_from_queue(a->part_alloc, arena, partition);
        partition->full_mask &= ~(1ULL << aidx);
    }
#else
    Section *section = (Section *)((uintptr_t)p & ~(SECTION_SIZE - 1));
    section_free_idx(section, p->idx >> 1);
#endif
}

static inline void pool_post_reserved(Pool *p, Allocator* a)
{
#if defined(ARENA_PATH)
    uint8_t pid = partition_id_from_addr((uintptr_t)p);
    size_t asize = area_size_from_partition_id(pid);
    Arena *arena = (Arena *)((uintptr_t)p & ~(asize - 1));
    uint32_t pidx = p->idx >> 1;
    uint32_t range = get_range((uint32_t)pidx, arena->ranges);
    uint64_t new_mask = ((1ULL << range%64) - 1UL) << pidx%64;
    arena->ranges |= apply_range(range, pidx);
    arena->allocations = arena->allocations | new_mask;
    arena->zero = arena->zero | new_mask;
    if(arena->allocations == UINT64_MAX)
    {
        Partition* partition = &a->part_alloc->area[pid];
        int32_t aidx = partition_allocator_get_arena_idx_from_queue(a->part_alloc, arena, partition);
        partition->full_mask |= (1ULL << aidx%64);
    }
#else
    Section *section = (Section *)((uintptr_t)p & ~(SECTION_SIZE - 1));
    section_reserve_idx(section, p->idx >> 1);
#endif
    
}
static inline void pool_set_empty(Pool *p, Allocator* a)
{
    pool_post_free(p, a);
    // the last piece was returned so make the first item the start of the free
    p->free = NULL;//(Block *)base_addr;
    //p->free->next = NULL;
    p->num_committed = 0;
    init_heap((Heap *)p);
}
static inline void pool_move_deferred(Pool *p)
{
    // for every item in the deferred list.
    if (p->free != NULL) {
        return;
    }
    p->free = p->deferred_free;
    p->deferred_free = NULL;
}

static inline void *pool_extend(Pool *p)
{
    p->num_used++;
    return (pool_base_address(p) + (p->num_committed++ * p->block_size));
}

static inline bool pool_is_maybe_empty(const Pool *p) { return p->free == NULL; }
static inline bool pool_is_empty(const Pool *p)
{
    if (p->num_used < p->num_available) {
        return false;
    }
    if (p->deferred_free != NULL) {
        return false;
    }
    const AtomicQueue *q = &p->thread_free;
    if (q->head != q->tail) {
        return false;
    }
    return true;
}
static inline bool pool_is_full(const Pool *p) { return p->num_used == 0; }
static inline bool pool_is_fully_commited(const Pool *p) { return p->num_committed >= p->num_available; }

static inline void pool_free_block(Pool *p, void *block, Allocator* a)
{
    if (--p->num_used == 0) {
        pool_set_empty(p, a);
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
    
    while (((uintptr_t)p->free & (p->alignment - 1)) != 0) {
        // for each unaligned part
        // we treat it as a counter alloc created within the pool.
        uintptr_t header = ALIGN_DOWN_2(p->free, p->alignment);
        int32_t *counter = (int32_t*)header;
        if(*counter-- > 0)
        {
            // an internal counter allocator would have been created for this pool
            // we just truncate its sub-part and remove it from the list.
            p->free = p->free->next;
        }
        else
        {
            // else, our decrement has removed all references to the counter allocator
            // and we are safe to return its header
            ((Block*)header)->next = p->free->next;
            p->free = (Block*)header;
            break;
        }
    }
    uint32_t curr_idx = (uint32_t)(size_t)((uint8_t *)p->free - base_addr)/p->block_size;
    if (curr_idx >= p->num_available) {
        p->free = NULL;
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
        const AtomicQueue *q = &p->thread_free;
        if (q->head != q->tail) {
            deferred_move_thread_free((Heap *)p);
        }
        if (p->deferred_free != NULL) {
            pool_move_deferred(p);
            return pool_get_free_block(p, a);
        }
        return NULL;
    }
}

static void pool_init(Pool *p, const uint8_t pidx, const uint32_t block_idx, const int32_t psize)
{
    init_heap((Heap *)p);
    p->idx = pidx << 1;
    p->block_idx = block_idx;
    p->block_size = pool_sizes[block_idx];
    p->num_committed = 0;
    p->alignment = 1 << __builtin_ctz(p->block_size);
    
    p->num_used = 0;
    p->next = NULL;
    p->prev = NULL;
    p->free = NULL;
    
    void *blocks = pool_base_address(p);
    const uintptr_t section_end = ALIGN_UP_2((uintptr_t)blocks, SECTION_SIZE);
    
    const size_t block_memory = psize - ALIGN_UP_2(sizeof(Pool), p->alignment);
    const size_t remaining_size = section_end - (uintptr_t)blocks;
    p->num_available = (uint32_t)(MIN(remaining_size, block_memory)/p->block_size);
}

#endif
