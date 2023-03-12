//
//  arena.inl
//  MemPoolTests
//
//  Created by Vilhelm Sævarsson on 24.7.2022.
//  Copyright © 2022 Vilhelm Sævarsson. All rights reserved.
//

#ifndef ARENA_H
#define ARENA_H

#include "callocator.inl"

#define HEADER_OVERHEAD 4
#define HEADER_FOOTER_OVERHEAD 8
#define ARENA_PARTS 64
#define ARENA_AVAIL_1 3
#define ARENA_AVAIL_2 4
#define ARENA_RANGE_OFFSET 16
#define ARENA_RANGE_FIELD 48
#define ARENA_BITS_PER_RANGE 6
#define ARENA_BITS_QUAD (ARENA_BITS_PER_RANGE * 4)
#define ARENA_BITS_GROUP (ARENA_BITS_QUAD / ARENA_BITS_PER_RANGE)
#define ARENA_BASE_SIZE_EXPONENT 22
#define ARENA_ROOT_MASK_SIZE 64
#define ARENA_ROOT_FILTER_SIZE 24

/*
    block. Memory
    slab. collection of blocks.


    top - [a, s, links][a, s, links][a, s, zero] [filter1, filter2]
    mid - [a, s, links][a, s, zero][p, n, scale]
    bot - [a, s][ p, n, scale]

    find a slab.
        - if request size is the same as previously.
            - if we have a cached slab. Allocate from slab if we can.
        - how big memory are you asking for.
            - at what level. l1, l2, l3.
            - the range count.
            - cache previous size.

    if size > 64th of heap.
        - allocate from the top.
    else
        - find mid slab with free space.
            - allocate from the top. 64k.


    find a free slab [ ]
    get a block from slab [ ]
        - if fail. goto find.

    if slab is full, update parent slab.
        heap
            - slab
                - parent
    current_slab
        -next free idx
        -num free blocks.
        -scale

    compute the largest range of zero bits in 64 bits.
        - update on free.
        - count number of bits set.
        - test to see if we can allocate them all, are contiguous.
        - if they are not contiguous.
            - use binary search. numSet/2
 */

/*


 heap allocations.
 // Layout
 [L3     ] 1     [(headers 128b)(masks 192b)| data   ]
 [L2     ] 64    [(masks 128b)   | data              ]
 [L1     ] 4096  [(masks 64b)    | data              ]
 // init

 [ L1 reserved | L1 next reserved ] [ L2 reserved | L2 next reserved ] [ L3 reserved | need_zero_init ]
 [ L1 reserved | L1 next reserved ] [ L2 reserved | need_zero_init ]
 [ L1 reserved | need_zero_init ]

 //
 // allocate
 // When there is no room for a L3 allocation.
 // might need to search 64 masks, if remaning memory allows.
 // When there is no room for a L2 allocation
 // anything more than L1 will only succeed if the cached L1 mask has room.
 //

 // L3
 // reserve L3 (range)
 // tag filter of L2 and L1 at L3 root. ( range )
 //

 // L2 -
 // reserve L2 (range)
 // tag filter of L1 at L2 root ( range )
 // reserve from L3 at L3 root. ( 1 bit )
 // if mask becomes full. -> reserve at root. l1 l2.

 // L1
 // reserve L1 ( range )
 // reserve L2 at L2 root ( 1 bit )
 // reserve L3 at L3 root ( 1 bit )
 // if masks becomes full -> reserve at l2 root.
 // if l2 root becomes full. -> reserve at l3 root.

 // free -----
 // L3
 // free L3 (range)
 // zero_init_mark L3 (range) (L1 and L2 allocations will need to zero initialize)
 // tag filter of L2 and L1 at L3 root. ( range )
 //

 // L2
 // free L2 (range)
 // zero_init_mark L2 (range) (L1 allocations will need to zero initialize)
 // tag filter of L1 at L2 root ( range )
 // if mask empty: free from L3 at L3 root. ( 1 bit )

 // L1
 // free L1 ( range )
 // if mask empty : free L2 at L2 root ( 1 bit )
 // if L2 mask empty from previous step: free L3 at L3 root ( 1 bit )
 */
typedef enum ArenaLevels_t {
    LEVEL_0 = 0, // LEVEL_1/64
    LEVEL_1,     // LEVEL_2/64
    LEVEL_2,     // LEVEL_3/64
    LEVEL_3      // size of heap
} ArenaLevels;

static const uint32_t new_arena_container_overhead =
    ARENA_PARTS + ARENA_PARTS * ARENA_PARTS + ARENA_PARTS * ARENA_PARTS * ARENA_PARTS;
static const uint32_t new_arena_level_size[] = {0, ARENA_PARTS, ARENA_PARTS *ARENA_PARTS};
static const uint32_t new_arena_level_offset[] = {0, ARENA_PARTS, ARENA_PARTS + ARENA_PARTS *ARENA_PARTS};
static const uint32_t arena_level_offset = ARENA_BASE_SIZE_EXPONENT;


// 4, 8, 16, 32, 64, 128, 256
// 22, 23, 24, 25, 26, 27, 28
// arena_idx exponent - 22
typedef struct arena_size_table_t
{
    uint64_t exponents[4];
    uint64_t sizes[4];
} arena_size_table;
typedef struct arena_empty_mask_table_t
{
    uint64_t sizes[3];
} arena_empty_mask_table;
static const arena_size_table arena_tables[7] = {
    {{4, 10, 16, 22}, {1 << 4, 1 << 10, 1 << 16, 1 << 22}},  {{5, 11, 17, 23}, {1 << 5, 1 << 11, 1 << 17, 1 << 23}},
    {{6, 12, 18, 24}, {1 << 6, 1 << 12, 1 << 18, 1 << 24}},  {{7, 13, 19, 25}, {1 << 7, 1 << 13, 1 << 19, 1 << 25}},
    {{8, 14, 20, 26}, {1 << 8, 1 << 14, 1 << 20, 1 << 26}},  {{9, 15, 21, 27}, {1 << 9, 1 << 15, 1 << 21, 1 << 27}},
    {{10, 16, 22, 28}, {1 << 10, 1 << 16, 1 << 22, 1 << 28}}};

static const uint64_t arena_L2_size = sizeof(Arena_L2) + sizeof(Arena);
static const uint64_t arena_L2_range = (arena_L2_size >> 4) + ((arena_L2_size & ((1 << 4) - 1))?1:0);
static const uint64_t arena_L2_mask = ((1ULL << arena_L2_range) - 1ULL);
static const uint64_t arena_L1_size = sizeof(Arena_L1);
static const uint64_t arena_L1_range = (arena_L1_size >> 4) + ((arena_L1_size & ((1 << 4) - 1))?1:0);
static const uint64_t arena_L1_mask = ((1ULL << arena_L1_range) - 1ULL);
static const uint64_t arena_L0_size = sizeof(Arena_L0);
static const uint64_t arena_L0_range = (arena_L0_size >> 4) + ((arena_L0_size & ((1 << 4) - 1))?1:0);
static const uint64_t arena_L0_mask = ((1ULL << arena_L0_range) - 1ULL);
static const uint64_t arena_empty_mask = 1ULL;
static const arena_empty_mask_table empty_masks[7] = {
    {arena_L0_mask, arena_L1_mask, arena_L2_mask},
    {(arena_L0_mask << 2) | arena_empty_mask,(arena_L1_mask << 2) | arena_empty_mask, (arena_L2_mask << 2) | arena_empty_mask},
    {(arena_L0_mask << 4) | arena_empty_mask, (arena_L1_mask << 4) | arena_empty_mask, (arena_L2_mask << 4) | arena_empty_mask},
    {(arena_L0_mask << 6) | arena_empty_mask,(arena_L1_mask << 6) | arena_empty_mask,(arena_L2_mask << 6) | arena_empty_mask},
    {(arena_L0_mask << 8) | arena_empty_mask, (arena_L1_mask << 8) | arena_empty_mask,(arena_L2_mask << 8) | arena_empty_mask},
    {(arena_L0_mask << 10) | arena_empty_mask, (arena_L1_mask << 10) | arena_empty_mask, (arena_L2_mask << 10) | arena_empty_mask},
    {(arena_L0_mask << 12) | arena_empty_mask, (arena_L1_mask << 12) | arena_empty_mask, (arena_L2_mask << 12) | arena_empty_mask}
};

static inline const uint32_t arena_get_arena_index(Arena* a)
{
    return a->container_exponent - arena_level_offset;
}
static inline const arena_size_table *arena_get_size_table_by_idx(uint8_t aidx)
{
    return &arena_tables[aidx];
}

static inline const arena_size_table *arena_get_size_table(Arena* a)
{
    return arena_get_size_table_by_idx(arena_get_arena_index(a));
}

static inline const uint64_t get_base_empty_mask(Arena* a, uint32_t level)
{
    return empty_masks[a->container_exponent - arena_level_offset].sizes[level];
}

void printBits(size_t const size, void const *const ptr);

void print_header(Arena *h, uintptr_t ptr);

uint32_t num_consecutive_zeros(uint64_t test);


uintptr_t new_arena_get_mask_addr(Arena *h, size_t i, size_t j);
uintptr_t new_arena_get_data_addr(Arena *h, size_t i, size_t j, size_t k);

static inline uintptr_t reserve_range_idx(size_t range, size_t idx) { return ((1ULL << range) - 1ULL) << idx; }

static inline void arena_init_zero(uintptr_t baseptr)
{
    *(uint64_t *)baseptr = 1ULL;
    *(uint64_t *)(baseptr + sizeof(uint64_t)) = 0;
    *(uint64_t *)(baseptr + sizeof(uint64_t) * 2) = 0;
}

static inline uint32_t delta_exp_to_idx(uintptr_t a, uintptr_t b, size_t exp)
{
    const ptrdiff_t diff = (uint8_t *)a - (uint8_t *)b;
    return (uint32_t)((size_t)diff >> exp);
}

static inline uint32_t arena_get_range(uint32_t aidx, size_t size, ArenaLevel al)
{
    const arena_size_table *stable = arena_get_size_table_by_idx(aidx);
    size_t range = size >> stable->exponents[al];
    range += (size & (stable->sizes[al] - 1)) ? 1 : 0;
    return (uint32_t)range;
}

static inline uint32_t arena_get_level_exponent(Arena* arena, ArenaLevel al)
{
    uint8_t offset = (18 - (6*al));
    return (uint32_t)(arena->container_exponent - offset);
}

static inline Arena* arena_get_header(uintptr_t addr)
{
    size_t asize = area_size_from_addr((uintptr_t)addr);
    uintptr_t base = ALIGN_DOWN_2((uintptr_t)addr, asize);
    uintptr_t header = base + sizeof(Arena_L2);
    return (Arena*)header;
}

static inline uintptr_t arena_get_parent_block(Arena* arena, uintptr_t addr, ArenaLevel al)
{
    switch(al)
    {
        case AL_HIGH:
            return ALIGN_DOWN_2((uintptr_t)addr, 1ULL << arena->container_exponent);
        case AL_MID:
            return ALIGN_DOWN_2((uintptr_t)addr, 1ULL << (arena->container_exponent - 6));  // 64th part
        case AL_LOW:
            return ALIGN_DOWN_2((uintptr_t)addr, 1ULL << (arena->container_exponent - 12)); // 4kth part
    }
}
static inline void arena_get_start_and_range(uint32_t idx, uint64_t amask, uint64_t rmask, uint32_t* out_start, uint32_t* out_range)
{
    if(rmask == 0LL)
    {
        *out_range = 1;
        *out_start = idx;
        return;
    }
        
    *out_start = 1;
    *out_range = get_range(*out_start, rmask);
    while((*out_start + *out_range) <= idx)
    {
        *out_start += *out_range;
        *out_range = get_range(*out_start, rmask);
    }
}
static inline uint32_t arena_get_local_idx(Arena* arena, uintptr_t addr, uintptr_t base, ArenaLevel al)
{
    return delta_exp_to_idx((uintptr_t)addr, base, arena_get_level_exponent(arena, al));
};

Arena *arena_init(uintptr_t base_addr, int32_t idx, size_t arena_size_exponent);
void arena_init_head_range(Arena *h, uintptr_t mask_offset);
bool arena_has_room(Arena *h, size_t size);

void *arena_alloc(Arena *h, uint32_t range, uint32_t exp, ArenaLevel al);

void *arena_alloc_high(Arena *h, uint32_t range, uint32_t exp, int32_t *midx);
void *arena_alloc_mid(Arena *h, uint32_t range, uint32_t exp, uint32_t midx, int32_t *bidx);
void *arena_alloc_low(Arena *h, uint32_t range, uint32_t exp, uint32_t midx, uint32_t bidx, int32_t *idx);
void *arena_alloc_at(uintptr_t addr, uint32_t range, uint32_t exp, ArenaLevel al);

void arena_free(Arena *h, void *p, bool dummy);
void arena_free_L2(Arena *h, void *p, Arena_L2* al2, uintptr_t sub_mask);
void arena_free_L1(Arena *h, void *p, Arena_L1* al1, Arena_L2* al2, uintptr_t sub_mask, uint32_t ridx,
                   bool needs_zero);
size_t arena_get_block_size(Arena *h, void *p);
/*
    - sizelimits (4m / 64 == 64k)
                base_size = area_size/64
                mid_size = base_size/64
                low_size = mid_size/64
 [64| 4k | 256k]
    find free (size )
        - >= 64k gets 64k piece
          <  64k get 1k pieces.
 -

 [Area|Section 48 bytes ] 4M
 64 bytes
 64 * 64 bytes 4k.
 4k * 64 = 256k
 1/16th is wasted on structures. 6.25%

 8 byte mask. per level.
 56 byte range fild.
 // 64 bytes per range.

 4M heaps.
    4m =    64 * 64k
    64k =   64 * 1k
    1k =    64 * 16bytes
 32M heaps.
    32m =   64 * 512k
    512k =  64 * 8k
    8k =    64 * 128bytes
 64M heaps.
    64m =   64 * 1m
    1m =    64 * 16k
    16k =   64 * 256bytes
 128M heaps.
    128m =  64 * 2m
    2m =    64 * 32k
    32k =   64 * 512bytes
 256M heaps.
    256m =  64 * 4m
    4m =    64 * 64k
    64k =   64 * 1k

 find free.
    - look at top level mask.
    - will be aligned to size value. >= 64 k, aligned to 64 k.
    - look at address alignment. if aligned to 64k, 1k, 16 bytes... which level to look at.
    -
 */


#endif // ARENA_H
