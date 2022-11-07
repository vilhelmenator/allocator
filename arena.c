//
//  MemPoolTests
//
//  Created by Vilhelm Sævarsson on 24.7.2022.
//  Copyright © 2022 Vilhelm Sævarsson. All rights reserved.
//

#include "arena.h"
#include "area.h"
#include "section.h"
#include <stdio.h>

uint32_t num_consecutive_zeros(uint64_t test)
{
    if(test == 0)
    {
        return 64;
    }
    
    uint32_t lz = __builtin_clzll(test);
    uint32_t tz = __builtin_ctzll(test);
    if(lz == 0)
    {
        uint32_t l1 = __builtin_clzll(~test);
        if((64 - l1) <= tz)
        {
            return tz;
        }
        test &= (1ULL << (64 - (l1 - 1))) - 1;
    }
    
    uint32_t mz = MAX(lz, tz);
    if((64 - (lz + tz)) <= mz)
    {
        return mz;
    }
    
    if(tz == 0)
    {
        test = test >> __builtin_ctzll(~test);
    }
    else
    {
        test = test >> (tz + 1);
    }
    
    while (test >= (1ULL << mz))
    {
        tz = __builtin_ctzll(test);
        mz = mz ^ ((mz ^ tz) & -(mz < tz));
        test = test >> (tz + 1);
        test = test >> __builtin_ctzll(~test);
    }
    return mz;
}

void printBits(size_t const size, void const *const ptr)
{
    unsigned char *b = (unsigned char *)ptr;
    unsigned char byte;
    int64_t i, j;

    for (i = size - 1; i >= 0; i--) {
        for (j = 7; j >= 0; j--) {
            byte = (b[i] >> j) & 1;
            printf("%u", byte);
        }
    }
    printf("]:[%16llX]\n", *(uint64_t *)ptr);
}

void print_header(Arena *h, uintptr_t ptr)
{
    const arena_size_table *stable = arena_get_size_table(h);
    Arena_L2 *al2 = (Arena_L2 *)((uintptr_t)h & ~(os_page_size - 1));
    ptrdiff_t rdiff = (uint8_t *)ptr - (uint8_t *)al2;
    uint32_t ridx = (uint32_t)((size_t)rdiff >> stable->exponents[2]);
    
    printf("\nL2!\n");
    printf("L0 Allocated:[0b");
    printBits(sizeof(uint64_t), (uint64_t*)&al2->L0_allocations);
    printf("L0 Ranges   :[0b");
    printBits(sizeof(uint64_t), (uint64_t*)&al2->L0_ranges);
    
    printf("L1 Allocated:[0b");
    printBits(sizeof(uint64_t), (uint64_t*)&al2->L1_allocations);
    printf("L1 Ranges   :[0b");
    printBits(sizeof(uint64_t), (uint64_t*)&al2->L1_ranges);
    
    printf("L2 Allocated:[0b");
    printBits(sizeof(uint64_t), (uint64_t*)&al2->L2_allocations);
    printf("L2 Ranges   :[0b");
    printBits(sizeof(uint64_t), (uint64_t*)&al2->L2_ranges);
    
    printf("L1 ZERO     :[0b");
    printBits(sizeof(uint64_t), (uint64_t*)&al2->L1_zero);
    printf("L2 ZERO     :[0b");
    printBits(sizeof(uint64_t), (uint64_t*)&al2->L2_zero);

    if (ridx != 0) {
        if(((uintptr_t)ptr & (stable->sizes[2] - 1)) != 0)
        {
            Arena_L1 *al1 = (Arena_L1 *)((uintptr_t)al2 + ridx * (1ULL << (h->container_exponent - 6)));
            
            printf("L1!\n");
            printf("L0 Allocated:[0b");
            printBits(sizeof(uint64_t), (uint64_t *)&al1->L0_allocations);
            printf("L0 Ranges   :[0b");
            printBits(sizeof(uint64_t), (uint64_t *)&al1->L0_ranges);
            
            printf("L1 Allocated:[0b");
            printBits(sizeof(uint64_t), (uint64_t *)&al1->L1_allocations);
            printf("L1 Ranges   :[0b");
            printBits(sizeof(uint64_t), (uint64_t *)&al1->L1_ranges);
            printf("L1 ZERO     :[0b");
            printBits(sizeof(uint64_t), (uint64_t *)&al1->L1_zero);
        }
    }
    if(((uintptr_t)ptr & (stable->sizes[1] - 1)) != 0)
    {
        Arena_L1 *al1 = (Arena_L1 *)((uintptr_t)al2 + ridx * (1ULL << (h->container_exponent - 6)));
        uintptr_t mask = ((uintptr_t)ptr & ~((1 << (h->container_exponent - 6)) - 1));
        ptrdiff_t diff = (uint8_t *)ptr - (uint8_t *)mask;
        uint32_t idx = (uint32_t)((size_t)diff >> (h->container_exponent - 12));
        if (idx != 0) {
            Arena_L0 *al0 = (Arena_L0 *)((uintptr_t)al1 + idx * (1ULL << (h->container_exponent - 12)));
            printf("L0!\n");
            printf("L0 Allocated:[0b");
            printBits(sizeof(uint64_t), (uint64_t*)&al0->L0_allocations);
            printf("L0 Ranges   :[0b");
            printBits(sizeof(uint64_t), (uint64_t*)&al0->L0_ranges);
        }
    }
    
}

void arena_init_head_range(Arena *h, uintptr_t mask_offset)
{
    const arena_size_table *stable = arena_get_size_table(h);
    Arena_L2* al2 = (Arena_L2*)((uintptr_t)mask_offset & ~(stable->sizes[3] - 1));
    Arena_L1* al1 = (Arena_L1*)((uintptr_t)mask_offset & ~(stable->sizes[2] - 1));
    Arena_L0 *al0 = (Arena_L0 *)mask_offset;
    int32_t idx = 0;
    if (mask_offset == (uintptr_t)al2)
    {
        idx = 2;
        al2->L1_allocations =  arena_empty_mask;
        al2->L2_allocations = arena_empty_mask;
        al2->L2_ranges = 0;
        al2->L2_zero = 1ULL;
    }
    else if (mask_offset == (uintptr_t)al1)
    {
        idx = 1;
        al1->L1_allocations =  arena_empty_mask;
        al1->L1_ranges = 0;
        uint32_t pidx = delta_exp_to_idx((uintptr_t)al1, (uintptr_t)al2, stable->exponents[2]);
        al2->L2_zero |= (1ULL << pidx);
    }
    // setup initial masks.
    uint32_t pidx = delta_exp_to_idx((uintptr_t)al0, (uintptr_t)al1, stable->exponents[1]);
    al1->L1_zero |= (1ULL << pidx);
    al0->prev = NULL;
    al0->next = NULL;
    al0->L0_allocations = get_base_empty_mask(h, idx);
    al0->L0_ranges = 0;
}

Arena *arena_init(uintptr_t base_addr, int32_t idx, size_t arena_size_exponent)
{
    Arena *h = (Arena *)(base_addr + sizeof(Arena_L2));

    h->container_exponent = (uint32_t)arena_size_exponent;
    // high allocations
    arena_init_head_range(h, base_addr);
    //print_header(h, (uintptr_t)h);
    return h;
}

bool arena_has_room(Arena *h, size_t size)
{
    const arena_size_table *stable = arena_get_size_table(h);
    uintptr_t level_size = stable->sizes[2];
    uintptr_t mask_offset = ((uintptr_t)h + sizeof(Arena) + (CACHE_LINE - 1)) & ~(CACHE_LINE - 1);
    uintptr_t data_offset = mask_offset + sizeof(uint64_t) * 10;
    uintptr_t end_offset = (data_offset + (level_size - 1)) & ~(level_size - 1);
    uint64_t *masks[] = {(uint64_t *)mask_offset, (uint64_t *)(mask_offset + sizeof(uint64_t) * 3),
                         (uint64_t *)(mask_offset + sizeof(uint64_t) * 6)};

    if ((*masks[0] == UINT64_MAX) && (*masks[1] == UINT64_MAX) && (*masks[2] == UINT64_MAX)) {
        return false;
    }
    size_t limit = end_offset - data_offset;
    if ((*masks[1] == UINT64_MAX) || (*masks[2] == UINT64_MAX)) {
        // there is no room for anything more than a small multiple of the smallest
        // size. new requests will be rejected. Only resize requests will be permitted.
        if ((*masks[1] == UINT64_MAX) && (*masks[2] == UINT64_MAX)) {
            limit = 1 << (h->container_exponent - 18);
        } else if (*masks[2] == UINT64_MAX) {
            limit = 1 << (h->container_exponent - 12);
        } else {
            limit = 1 << (h->container_exponent - 6);
        }
    }

    if (size > limit) {
        return false;
    }
    return true;
}

void *arena_get_block_L2(Arena *h, size_t range, Arena_L2* al2, int32_t midx)
{
    const arena_size_table *stable = arena_get_size_table(h);
    uintptr_t mask = reserve_range_idx(range, midx);
    
    al2->L2_allocations |= mask;
    al2->L2_ranges |= apply_range((uint32_t)range, midx);
    return (void *)((uintptr_t)al2 + (midx * stable->sizes[2]));
}
void *arena_find_block_L2(Arena *h, size_t range, uint32_t exp, int32_t* midx)
{
    uintptr_t base = ((uintptr_t)h & ~(os_page_size - 1));
    Arena_L2* al2 = (Arena_L2*)base;
    *midx = find_first_nzeros(al2->L2_allocations, range, exp);
    if(*midx != -1)
    {
        return arena_get_block_L2(h, range, al2, *midx);
    }
    return NULL;
}

void *arena_get_block_L1(Arena *h, size_t range, Arena_L1* al1, int32_t idx)
{
    const arena_size_table *stable = arena_get_size_table(h);
    uintptr_t mask = reserve_range_idx(range, idx);
    
    al1->L1_allocations |= mask;
    al1->L1_ranges |= apply_range((uint32_t)range, idx);

    
    return (void *)((uintptr_t)al1 + (idx * stable->sizes[1]));
}

void *arena_find_block_L1(Arena *h, size_t range, uint32_t exp, uint32_t midx, int32_t *bidx)
{
    const arena_size_table *stable = arena_get_size_table(h);
    uintptr_t base = ((uintptr_t)h & ~(os_page_size - 1));
    Arena_L2* al2 = (Arena_L2*)base;
    
    Arena_L1* al1 = (Arena_L1*)(base + (midx * stable->sizes[2]));
    if (((1ULL << midx) & al2->L2_zero) == 0) {
        
        arena_init_head_range(h, (uintptr_t)al1);
    }
    *bidx = find_first_nzeros(al1->L1_allocations, range, exp);
    if(*bidx != -1)
    {
        al2->L2_allocations |= (1ULL << midx);
        return arena_get_block_L1(h, range, al1, *bidx);
    }
    return NULL;
}

void *arena_get_block_L0(Arena *h, size_t range, Arena_L0* al0, int32_t idx)
{
    const arena_size_table *stable = arena_get_size_table(h);
    uintptr_t mask = reserve_range_idx(range, idx);
    al0->L0_allocations |= mask;
    al0->L0_ranges |= apply_range((uint32_t)range, idx);
    
    return (void *)((uintptr_t)al0 + (idx * stable->sizes[0]));
}

void *arena_find_block_L0(Arena *h, size_t range, uint32_t exp, uint32_t midx, uint32_t bidx, int32_t *idx)
{
    const arena_size_table *stable = arena_get_size_table(h);
    uintptr_t base = ((uintptr_t)h & ~(os_page_size - 1));
    Arena_L2* al2 = (Arena_L2*)base;
    Arena_L1* al1 = (Arena_L1*)(base + (midx * stable->sizes[2]));
    Arena_L0* al0 = (Arena_L0*)((uintptr_t)al1 + (bidx * stable->sizes[1]));
    if (((1UL << bidx) & al1->L1_zero) == 0) {
        
        arena_init_head_range(h, (uintptr_t)al0);
    }
    *idx = find_first_nzeros(al0->L0_allocations, range, exp);
    if(*idx != -1)
    {
        al1->L1_allocations |= (1ULL << bidx);
        al2->L2_allocations |= (1ULL << midx);
        return arena_get_block_L0(h, range, al0, *idx);
    }
    return NULL;
}

void *arena_get_block_at(Arena *h, size_t l3idx, size_t l2idx, size_t l1idx)
{
    uintptr_t base = ((uintptr_t)h & ~(os_page_size - 1));
    const arena_size_table *stable = arena_get_size_table(h);
    uintptr_t offset = base + stable->sizes[2] * l3idx + stable->sizes[1] * l2idx + stable->sizes[0] * l1idx;
    return (void *)offset;
}

void *arena_get_block(Arena *h, size_t size)
{
    void *res = NULL;
    size_t range = 0;
    uint32_t level_idx = 2;
    
    const arena_size_table *stable = arena_get_size_table(h);
    uintptr_t data_offset = (uintptr_t)h + sizeof(Arena);
    uintptr_t end_offset = (data_offset + (stable->sizes[3] - 1)) & ~(stable->sizes[3] - 1);
    size_t limit = end_offset - data_offset;
    

    if (size > limit) {
        return NULL;
    }
    
    if (size < stable->sizes[1]) {
        level_idx = 0;
    } else if (size < stable->sizes[2]) {
        level_idx = 1;
    }

    range = size >> stable->exponents[level_idx];
    range += (size & (stable->sizes[level_idx] - 1)) ? 1 : 0;
    
    switch (level_idx) {
    case 0: {
        int32_t idx;
        res = arena_find_block_L0(h, range, 0, 0, 0, &idx);
        break;
    }
    case 1: {
        int32_t bidx;
        res = arena_find_block_L1(h, range, 0, 0, &bidx);
        break;
    }
    default: {
        int32_t midx;
        res = arena_find_block_L2(h, range, 0,  &midx);
        break;
    }
    }
    //print_header(h, (uintptr_t)res);
    return res;
}

void *arena_alloc_high(Arena *arena, uint32_t range, uint32_t exp, int32_t *midx)
{
    return arena_find_block_L2(arena, range, exp, midx);
}

void *arena_alloc_mid(Arena *arena, uint32_t range, uint32_t exp, uint32_t midx, int32_t *bidx)
{
    return arena_find_block_L1(arena, range, exp, midx, bidx);
}

void *arena_alloc_low(Arena *arena, uint32_t range, uint32_t exp, uint32_t midx, uint32_t bidx, int32_t *idx)
{
    return arena_find_block_L0(arena, range, exp, midx, bidx, idx);
}

void *arena_alloc_at(uintptr_t addr, uint32_t range, uint32_t exp, ArenaLevel al)
{
    int32_t idx;
    Arena* header = arena_get_header(addr);
    switch(al)
    {
        case AL_HIGH:
        {
            return arena_find_block_L2(header, range, exp, &idx);
        }
        case AL_MID:
        {
            // get midx
            uintptr_t base = arena_get_parent_block(header, addr, AL_HIGH);
            uint32_t midx = arena_get_local_idx(header, addr, base, AL_HIGH);
            return arena_find_block_L1(header, range, exp, midx, &idx);
        }
        default:
        {
            // get midx
            // get bidx
            uintptr_t base = arena_get_parent_block(header, addr, AL_HIGH);
            uint32_t midx = arena_get_local_idx(header, addr, base, AL_HIGH);
            uintptr_t base_l = arena_get_parent_block(header, addr, AL_MID);
            uint32_t bidx = arena_get_local_idx(header, addr, base_l, AL_MID);
            return arena_find_block_L0(header, range, exp, midx, bidx, &idx);
        }
    }
}

void arena_reset_L2(Arena *h, void *p, Arena_L2* al2, uintptr_t sub_mask, bool needs_zero)
{
    if((uintptr_t)p == (uintptr_t)al2)
    {
        al2->L0_allocations &= sub_mask| get_base_empty_mask(h, 2);
        al2->L1_allocations &= sub_mask| arena_empty_mask;
    }

    al2->L2_allocations &= sub_mask| arena_empty_mask;
    al2->L2_ranges &= sub_mask;
    if(needs_zero)
    {
        al2->L1_zero &= sub_mask;
    }
}

void arena_free_L2(Arena *h, void *p, Arena_L2* al2, uintptr_t sub_mask) { arena_reset_L2(h, p, al2, sub_mask, true); }

void arena_free_L1(Arena *h, void *p, Arena_L1* al1, Arena_L2* al2, uintptr_t sub_mask, uint32_t ridx,
                   bool needs_zero)
{
    const arena_size_table *stable = arena_get_size_table(h);
    uintptr_t previous_mask = al1->L1_allocations;
    al1->L1_allocations = (al1->L1_allocations & sub_mask) | arena_empty_mask;
    al1->L1_ranges = al1->L1_ranges & sub_mask;
    if ((al1->L1_allocations == arena_empty_mask) && (previous_mask != al1->L1_allocations)) {
        arena_reset_L2(h, al1, al2, ~(1ULL << ridx), needs_zero);
        if(needs_zero)
        {
            uint32_t idx = delta_exp_to_idx((uintptr_t)p, (uintptr_t)al2, stable->exponents[2]);
            al1->L1_zero &= ~(1ULL << idx);
        }
    }
}

void arena_free_L0(Arena *h, void *p, Arena_L0* al0, Arena_L2* al2, uintptr_t sub_mask, uint32_t ridx)
{
    // if one becomes available.... the filter needs to know.
    // if a plate becomes available... the parent needs to know.
    // if the parent becomes available... its parent .... etc.
    const arena_size_table *stable = arena_get_size_table(h);
    Arena_L1* al1 = (Arena_L1*)((uintptr_t)p & ~(stable->sizes[2] - 1));
    uintptr_t base_empty_mask = 0;
    if((uintptr_t)al0 != (uintptr_t)al1)
    {
        base_empty_mask = get_base_empty_mask(h, 0);
    }
    else
    {
        if((uintptr_t)al1 != (uintptr_t)al2)
        {
            base_empty_mask = get_base_empty_mask(h, 1);
        }
        else
        {
            base_empty_mask = get_base_empty_mask(h, 2);
        }
    }
    uintptr_t previous_mask = al0->L0_allocations;
    al0->L0_allocations = (al0->L0_allocations & sub_mask) | base_empty_mask;
    al0->L0_ranges = al0->L0_ranges & sub_mask;
    if ((al0->L0_allocations == base_empty_mask) && (previous_mask != al0->L0_allocations)) {
        uint32_t idx = delta_exp_to_idx((uintptr_t)p, (uintptr_t)al1, stable->exponents[1]);
        sub_mask = ~(1ULL << idx);
        // if you have released a whole l0 plate.
        // let the parent know.
        arena_free_L1(h, al0, al1, al2, sub_mask, ridx, false);
    }
}

void arena_free(Arena *h, void *p, bool dummy)
{
    if(p == NULL)
    {
        return;
    }
    const arena_size_table *stable = arena_get_size_table(h);

    Arena_L2* al2 = (Arena_L2*)((uintptr_t)h & ~(os_page_size - 1));
    uint32_t ridx = delta_exp_to_idx((uintptr_t)p, (uintptr_t)al2, stable->exponents[2]);
    for (int i = 2; i >= 0; i--) {
        if (((uintptr_t)p & (stable->sizes[i] - 1)) == 0) {
            // aligned to base level
            uintptr_t pc = ((uintptr_t)p & ~(stable->sizes[i + 1] - 1));
            uint32_t idx = delta_exp_to_idx((uintptr_t)p, pc, stable->exponents[i]);
            switch (i) {
            case 0:
                {
                    Arena_L0* al0 = (Arena_L0*)pc;
                    uint32_t range = get_range(idx, al0->L0_ranges);
                    uintptr_t sub_mask = ~reserve_range_idx(range, idx);
                    arena_free_L0(h, p, al0, al2, sub_mask, ridx);
                }
                break;
            case 1:
                {
                    Arena_L1* al1 = (Arena_L1*)pc;
                    uint32_t range = get_range(idx, al1->L1_ranges);
                    uintptr_t sub_mask = ~reserve_range_idx(range, idx);
                    arena_free_L1(h, p, al1, al2, sub_mask, ridx, true);
                }
                break;
            default:
                {
                    Arena_L2* al2 = (Arena_L2*)pc;
                    uint32_t range = get_range(idx, al2->L2_ranges);
                    uintptr_t sub_mask = ~reserve_range_idx(range, idx);
                    arena_free_L2(h, p, al2, sub_mask);
                }
                break;
            };

            break;
        }
    }
    //print_header(h, (uintptr_t)p);
}

size_t arena_get_block_size(Arena *h, void *p)
{
    const uint64_t level_exponents[] = {(h->container_exponent), (h->container_exponent - 6),
                                        (h->container_exponent - 12), (h->container_exponent - 18)};

    const uint64_t level_sizes[] = {(1 << level_exponents[0]), (1 << level_exponents[1]), (1 << level_exponents[2]),
                                    (1 << level_exponents[3])};

    for (int i = 1; i < 4; i++) {
        if (((uintptr_t)p & (level_sizes[i] - 1)) == 0) {
            // aligned to base level
            uintptr_t mask = ((uintptr_t)p & ~(level_sizes[i - 1] - 1));
            const ptrdiff_t diff = (uint8_t *)p - (uint8_t *)mask;
            const uint32_t idx = (uint32_t)((size_t)diff >> level_exponents[i]);
            if (mask < (uintptr_t)h) {
                // we at the start
                uintptr_t mask_offset = ((uintptr_t)h + sizeof(Arena) + (CACHE_LINE - 1)) & ~(CACHE_LINE - 1);
                mask = mask_offset + (i - 1) * CACHE_LINE;
            }

            uint32_t range = get_range(idx, mask);
            return level_sizes[i] * range;
        }
    }
    return 0;
}
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
