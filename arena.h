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
#include <stdio.h>

#define WSIZE 4
#define DSIZE 8
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
static const uint64_t arena_empty_mask = 1UL << 63;
static const uint64_t arena_empty_mask_z = 15UL << 60;
static const uint64_t arena_mask_size = sizeof(uint64_t) * 3;
static const uint64_t arena_filter_size = sizeof(uint64_t) * 2;

// 4, 8, 16, 32, 64, 128, 256
// 22, 23, 24, 25, 26, 27, 28
// arena_idx exponent - 22
typedef struct arena_size_table_t
{
    uint64_t exponents[4];
    uint64_t sizes[4];
} arena_size_table;

static const arena_size_table arena_tables[7] = {
    {{4, 10, 16, 22}, {1 << 4, 1 << 10, 1 << 16, 1 << 22}},  {{5, 11, 17, 23}, {1 << 5, 1 << 11, 1 << 17, 1 << 23}},
    {{6, 12, 18, 24}, {1 << 6, 1 << 12, 1 << 18, 1 << 24}},  {{7, 13, 19, 25}, {1 << 7, 1 << 13, 1 << 19, 1 << 25}},
    {{8, 14, 20, 26}, {1 << 8, 1 << 14, 1 << 20, 1 << 26}},  {{9, 15, 21, 27}, {1 << 9, 1 << 15, 1 << 21, 1 << 27}},
    {{10, 16, 22, 28}, {1 << 10, 1 << 16, 1 << 22, 1 << 28}}};

static inline const arena_size_table *get_size_table(uint32_t exponent)
{
    return &arena_tables[exponent - arena_level_offset];
}

void printBits(size_t const size, void const *const ptr);

void print_header(Arena *h, uintptr_t ptr);
static inline uint64_t apply_range(uint32_t range, uint32_t at)
{
    // range == 1 -> nop
    // set bit at (at)
    // set bit at at + (range - 1).
    //
    // 111110011
    return (1UL << at) | (1UL << (at - (range - 1)));
}

static inline uint32_t get_range(uint32_t at, uint64_t mask)
{
    //  at == 7
    //  00001000111
    //  00000111111 &
    //  00000000111 tz8 - at == 3
    //  lz8 - at == 3
    //  3 + 2 == 5
    // zero all bits from at to the highest bit.
    uint64_t top_mask = ((1UL << at) - 1);
    return at - __builtin_ctzll(mask & top_mask) + 1;
}

uintptr_t new_arena_get_mask_addr(Arena *h, size_t i, size_t j);
uintptr_t new_arena_get_data_addr(Arena *h, size_t i, size_t j, size_t k);

static inline uintptr_t reserve_range_idx(size_t range, size_t idx) { return ((1UL << range) - 1UL) << (idx - (range - 1)); }

static inline void arena_init_zero(uintptr_t baseptr)
{
    *(uint64_t *)baseptr = 1UL << 63;
    *(uint64_t *)(baseptr + sizeof(uint64_t)) = 0;
    *(uint64_t *)(baseptr + sizeof(uint64_t) * 2) = 0;
}

int32_t arena_init_head_range(Arena *h, uintptr_t mask_offset, size_t size);

Arena *arena_init(uintptr_t base_addr, int32_t idx, size_t arena_size_exponent);

bool arena_has_room(Arena *h, size_t size);
void *arena_get_block(Arena *h, size_t size);
void arena_free(Arena *h, void *p, bool dummy);

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
inline uint8_t size_to_arena(const size_t as)
{
    if (as <= SMALL_OBJECT_SIZE) {
        return 0;
    } else if (as <= MEDIUM_OBJECT_SIZE) {
        return 1; // 128Mb pages
    } else if (as <= LARGE_OBJECT_SIZE) {
        return 2; // 128Mb pages
    } else if (as <= HUGE_OBJECT_SIZE) {
        return 3; // 128Mb pages
    } else {
        return 4; // 256Mb pages
    }
}
inline bool arena_is_connected(const Arena *h) { return h->prev != NULL || h->next != NULL; }

#endif // ARENA_H
