//
//  arena.inl
//  MemPoolTests
//
//  Created by Vilhelm Sævarsson on 24.7.2022.
//  Copyright © 2022 Vilhelm Sævarsson. All rights reserved.
//

#include "arena.h"
#include "area.h"
#include "section.h"
uintptr_t new_arena_get_mask_addr(Arena *h, size_t i, size_t j)
{
    uintptr_t base = ((uintptr_t)h & ~(os_page_size - 1));
    return base + new_arena_level_offset[i] + new_arena_level_size[i] * j;
}

uintptr_t new_arena_get_data_addr(Arena *h, size_t i, size_t j, size_t k)
{
    uintptr_t base = ((uintptr_t)h & ~(os_page_size - 1));
    return base + (1 << (h->container_exponent - 6)) * i + (1 << ((h->container_exponent - 12))) * j +
           (1 << (h->container_exponent - 18)) * k;
}
void printBits(size_t const size, void const *const ptr)
{
    unsigned char *b = (unsigned char *)ptr;
    unsigned char byte;
    ssize_t i, j;

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
    const arena_size_table *stable = get_size_table(h->container_exponent);
    Arena_L2 *al2 = (Arena_L2 *)((uintptr_t)h & ~(os_page_size - 1));
    ptrdiff_t rdiff = (uint8_t *)ptr - (uint8_t *)al2;
    uint32_t ridx = (uint32_t)((size_t)rdiff >> stable->exponents[2]);
    
    printf("\nL2!\n");
    printf("L0 Allocated:[0b");
    printBits(sizeof(uint64_t), (uint64_t*)&al2->L0_allocations);
    printf("L0 Ranges:   [0b");
    printBits(sizeof(uint64_t), (uint64_t*)&al2->L0_ranges);
    printf("L0 Available:[0b");
    printBits(sizeof(uint64_t), (uint64_t*)&al2->L0_available);
    
    printf("L1 Allocated:[0b");
    printBits(sizeof(uint64_t), (uint64_t*)&al2->L1_allocations);
    printf("L1 Ranges:   [0b");
    printBits(sizeof(uint64_t), (uint64_t*)&al2->L1_ranges);
    printf("L1 Available:[0b");
    printBits(sizeof(uint64_t), (uint64_t*)&al2->L1_available);
    
    printf("L2 Allocated:[0b");
    printBits(sizeof(uint64_t), (uint64_t*)&al2->L2_allocations);
    printf("L2 Ranges:   [0b");
    printBits(sizeof(uint64_t), (uint64_t*)&al2->L2_ranges);
    
    printf("L1 ZERO     :[0b");
    printBits(sizeof(uint64_t), (uint64_t*)&al2->L1_zero);
    printf("L2 ZERO     :[0b");
    printBits(sizeof(uint64_t), (uint64_t*)&al2->L2_zero);
    printf("L0 Filter   :[0b");
    printBits(sizeof(uint64_t), (uint64_t*)&al2->L0_filter);
    printf("L1 Filter   :[0b");
    printBits(sizeof(uint64_t), (uint64_t*)&al2->L1_filter);

    if (ridx != 0) {
        Arena_L1 *al1 = (Arena_L1 *)((uintptr_t)al2 + ridx * (1UL << (h->container_exponent - 12)));
        
        printf("L1!\n");
        printf("L0 Allocated:[0b");
        printBits(sizeof(uint64_t), (uint64_t *)&al1->L0_allocations);
        printf("L0 Ranges:   [0b");
        printBits(sizeof(uint64_t), (uint64_t *)&al1->L0_ranges);
        printf("L0 Available:[0b");
        printBits(sizeof(uint64_t), (uint64_t *)&al1->L0_available);
        
        printf("L1 Allocated:[0b");
        printBits(sizeof(uint64_t), (uint64_t *)&al1->L1_allocations);
        printf("L1 Ranges:   [0b");
        printBits(sizeof(uint64_t), (uint64_t *)&al1->L1_ranges);
        printf("L1 ZERO:     [0b");
        printBits(sizeof(uint64_t), (uint64_t *)&al1->L1_zero);
    }
    
    uintptr_t mask = ((uintptr_t)ptr & ~((1 << (h->container_exponent - 6)) - 1));
    ptrdiff_t diff = (uint8_t *)ptr - (uint8_t *)mask;
    uint32_t idx = (uint32_t)((size_t)diff >> (h->container_exponent - 12));
    if (idx != 0) {
        Arena_L0 *al0 = (Arena_L0 *)((uintptr_t)al2 + idx * (1UL << (h->container_exponent - 12)));
        printf("L0!\n");
        printf("L0 Allocated:[0b");
        printBits(sizeof(uint64_t), (uint64_t*)&al0->L0_allocations);
        printf("L0 Ranges   :[0b");
        printBits(sizeof(uint64_t), (uint64_t*)&al0->L0_ranges);
    }
}

int32_t arena_init_head_range(Arena *h, uintptr_t mask_offset)
{
    const arena_size_table *stable = get_size_table(h->container_exponent);
    size_t size = 0;
    uintptr_t range = 0;
    int32_t idx = 0;
    if ((mask_offset & (stable->sizes[3] - 1)) == 0)
    {
        size = sizeof(Arena_L2) + sizeof(Arena);
        Arena_L2 *al2 = (Arena_L2*)mask_offset;
        al2->L0_available = 0;
        al2->L1_allocations =  1UL << 63;
        al2->L1_available = 0;
        al2->L2_allocations = 1UL << 63;
        al2->L2_ranges = 0;
        al2->L1_zero = 1UL << 63;
        al2->L2_zero = 1UL << 63;
        al2->L0_filter = 0;
        al2->L1_filter = 0;
    }
    else if ((mask_offset & (stable->sizes[2] - 1)) == 0)
    {
        size = sizeof(Arena_L1);
        Arena_L1 *al1 = (Arena_L1 *)mask_offset;
        al1->L0_available = 0;
        al1->L1_allocations =  1UL << 63;
        al1->L1_ranges = 0;
        al1->L1_zero = 1UL << 63;
    }
    else
    {
        size = sizeof(Arena_L0);
    }
    // setup initial masks.
    uintptr_t header_size = size;
    uintptr_t rem = (header_size & ((stable->sizes[0]) - 1));
    range = header_size >> stable->exponents[0];
    range += (rem) ? 1 : 0;
    idx = (int32_t)(64 - range);
    Arena_L1 *al1 = (Arena_L1 *)mask_offset;
    al1->L0_allocations = reserve_range_idx(range, 63);
    al1->L0_ranges = 0;
    
    return idx - 1;
}

Arena *arena_init(uintptr_t base_addr, int32_t idx, size_t arena_size_exponent)
{
    // 64 bytes for root mask data. [a,s,l][a,s,l][a,s]
    // 24 bytes for root zero and filter data. [z,f1,f2]
    // heap data and header info.
    Arena *h = (Arena *)(base_addr + sizeof(Arena_L2));

    h->container_exponent = (uint32_t)arena_size_exponent;
    
    //
    h->num_allocations = 0;
    h->previous_l1_offset = -1;
    // high allocations
    arena_init_head_range(h, base_addr);
    print_header(h, (uintptr_t)h);
    return h;
}

bool arena_has_room(Arena *h, size_t size)
{
    const arena_size_table *stable = get_size_table(h->container_exponent);
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

void *arena_get_block_L2(Arena *h, uintptr_t base, size_t range, uint64_t **masks)
{
    const arena_size_table *stable = get_size_table(h->container_exponent);
    // test to see if the structs have been initilized.
    // just look at the base masks..
    uint64_t *base_mask = masks[2];
    // special case for zero... because our header data is there
    int32_t idx = find_first_nzeros(*base_mask, range);
    uint64_t *filters = (uint64_t *)((uint8_t *)masks[0] + CACHE_LINE);
    if (idx != -1) {
        // found memory
        uintptr_t mask = reserve_range_idx(range, idx);
        *base_mask = *base_mask | mask;
        filters[0] = filters[0] | mask;
        filters[1] = filters[1] | mask;
        idx = 63 - idx;
        if (range > 1) {
            *(base_mask + 1) |= apply_range((uint32_t)range, idx);
        }
        h->num_allocations++;
        return (void *)(base + (idx * stable->sizes[2]));
    }
    return NULL;
}

void *arena_get_block_L1(Arena *h, uintptr_t base, size_t range, uint64_t **masks)
{
    uint64_t *filters = (uint64_t *)((uint8_t *)masks[0] + CACHE_LINE);
    uint64_t invmask = ~filters[1];
    uint32_t midx = get_next_mask_idx(invmask, 0);
    const arena_size_table *stable = get_size_table(h->container_exponent);
    // test to see if the structs have been initilized.
    // just look at the base masks..
    if (midx == 0) {
        // zero pos is always zero initialized
        uint64_t *base_mask = masks[1];
        // special case for zero... because our header data is there
        int32_t idx = find_first_nzeros(*base_mask, range);
        if (idx != -1) {
            // found memory

            uintptr_t mask = reserve_range_idx(range, idx);
            *base_mask = *base_mask | mask;
            idx = 63 - idx;
            if (range > 1) {
                *(base_mask + 1) |= apply_range((uint32_t)range, idx);
            }
            h->num_allocations++;
            return (void *)(base + (idx * stable->sizes[1]));
        }
    }

    uint64_t *zmask = (uint64_t *)((uint8_t *)masks[2] + sizeof(uint64_t));
    uint64_t *l3_reserve = masks[2];
    while ((midx = get_next_mask_idx(invmask, midx + 1)) != -1) {
        uintptr_t base_addr = (base + (midx * stable->sizes[2]));
        int32_t idx = 62;
        if (((1UL << (63 - midx)) & *zmask) == 0) {
            // if it is empty... we can safely just get the first available.
            arena_init_zero(base_addr);
            arena_init_zero(base_addr + sizeof(int64_t) * 3);
            *zmask = *zmask | (1UL << (63 - midx));
            base_addr += sizeof(int64_t) * 3;
        } else {
            base_addr += sizeof(int64_t) * 3;
            idx = find_first_nzeros(*(uint64_t *)base_addr, range);
        }

        if (idx != -1) {
            uintptr_t mask = reserve_range_idx(range, idx);
            *(uint64_t *)base_addr |= mask;
            idx = 63 - idx;
            if (range > 1) {
                *(uint64_t *)(base_addr + sizeof(uint64_t)) |= apply_range((uint32_t)range, idx);
            }
            h->num_allocations++;
            *l3_reserve = *l3_reserve | (1UL << (63 - midx));
            if (*(uint64_t *)base_addr == UINT64_MAX) {
                uint64_t *l1_filter = (uint64_t *)((uint8_t *)masks[0] + sizeof(uint64_t));
                *l1_filter = *l1_filter | (1UL << (63 - midx));
            }
            return (void *)(base_addr + (idx * stable->sizes[1]));
        }
    }

    return NULL;
}
void *arena_get_block_L0(Arena *h, uintptr_t base, size_t range, Arena_L0* al0)
{
    const arena_size_table *stable = get_size_table(h->container_exponent);
    Arena_L2* al2 = (Arena_L2*)base;
    int32_t idx = find_first_nzeros(al0->L0_allocations, range);
    if (idx != -1) {
        uintptr_t mask = reserve_range_idx(range, idx);
        idx = 63 - idx;
        
        al0->L0_allocations |= mask;
        al0->L0_ranges |= apply_range((uint32_t)range, idx);
        
        h->num_allocations++;
        if (al0->L0_allocations == UINT64_MAX) {
            
            Arena_L1* al1 = (Arena_L1*)((uintptr_t)al0 & ~(stable->sizes[2] - 1));
            uint32_t pidx = delta_exp_to_idx((uintptr_t)al0, (uintptr_t)al1, stable->exponents[1]);
            al1->L0_available |= (1UL << (63 - pidx));
            if (al1->L0_available == UINT64_MAX) {
                al2->L0_filter |= (1UL << (63 - pidx));
            }
        }
        return (void *)((uintptr_t)al0 + (idx * stable->sizes[0]));
    }
    return NULL;
}

void *arena_find_block_L0(Arena *h, uintptr_t base, size_t range, uint64_t **masks)
{
    const arena_size_table *stable = get_size_table(h->container_exponent);
    Arena_L2* al2 = (Arena_L2*)base;
    if (h->previous_l1_offset != -1) {
        void* res = arena_get_block_L0(h, base, range, (Arena_L0*)h->previous_l1_offset);
        if(res != NULL)
        {
            return res;
        }
        h->previous_l1_offset = -1;
    }

    uint32_t midx = -1;
    while ((midx = get_next_mask_idx(~al2->L0_filter, midx + 1)) != -1) {

        Arena_L1* al1 = (Arena_L1*)(base + (midx * stable->sizes[2]));
        uint32_t bidx = -1;
        while ((bidx = get_next_mask_idx(~al1->L0_available, bidx + 1)) != -1) {
            Arena_L0* al0 = (Arena_L0*)((uintptr_t)al1 + (bidx * stable->sizes[1]));
            
            uint64_t *bzmask = &al1->L1_zero;
            if (((1UL << (63 - bidx)) & *bzmask) == 0) {
                
                arena_init_head_range(h, (uintptr_t)al0);
                *bzmask = *bzmask | (1UL << (63 - bidx));
            }
            void* res = arena_get_block_L0(h, base, range, al0);
            if(res != NULL)
            {
                h->previous_l1_offset = (uintptr_t)al0;
                return res;
            }
        }
    }
    h->previous_l1_offset = 0;
    return NULL;
}

void *arena_get_block_at(Arena *h, size_t l3idx, size_t l2idx, size_t l1idx)
{
    uintptr_t base = ((uintptr_t)h & ~(os_page_size - 1));
    const arena_size_table *stable = get_size_table(h->container_exponent);
    uintptr_t offset = base + stable->sizes[2] * l3idx + stable->sizes[1] * l2idx + stable->sizes[0] * l1idx;
    return (void *)offset;
}

void *arena_get_block(Arena *h, size_t size)
{
    const arena_size_table *stable = get_size_table(h->container_exponent);

    uintptr_t base = ((uintptr_t)h & ~(os_page_size - 1));
    uintptr_t mask_offset = base;
    uintptr_t data_offset = (uintptr_t)h + sizeof(Arena);
    uintptr_t end_offset = (data_offset + (stable->sizes[3] - 1)) & ~(stable->sizes[3] - 1);
    uint64_t *masks[] = {(uint64_t *)mask_offset, (uint64_t *)(mask_offset + sizeof(uint64_t) * 3),
                         (uint64_t *)(mask_offset + sizeof(uint64_t) * 6)};

    if ((*masks[0] == UINT64_MAX) && (*masks[1] == UINT64_MAX) && (*masks[2] == UINT64_MAX)) {
        return NULL;
    }

    size_t limit = end_offset - data_offset;
    if ((*masks[1] == UINT64_MAX) || (*masks[2] == UINT64_MAX)) {
        // there is no room for anything more than a small multiple of the smallest
        // size. new requests will be rejected. Only resize requests will be permitted.
        if ((*masks[1] == UINT64_MAX) && (*masks[2] == UINT64_MAX)) {
            limit = stable->sizes[0];
        } else if (*masks[2] == UINT64_MAX) {
            limit = stable->sizes[1];
        } else {
            limit = stable->sizes[2];
        }
    }

    if (size > limit) {
        return NULL;
    }
    int32_t level_idx = 2;
    if (size < stable->sizes[1]) {
        level_idx = 0;
    } else if (size < stable->sizes[2]) {
        level_idx = 1;
    }

    size_t range = size >> stable->exponents[level_idx];
    range += (size & (stable->sizes[level_idx] - 1)) ? 1 : 0;
    void *res = NULL;
    switch (level_idx) {
    case 0: {
        res = arena_find_block_L0(h, base, range, masks);
        break;
    }
    case 1: {
        res = arena_get_block_L1(h, base, range, masks);
        break;
    }
    default: {
        res = arena_get_block_L2(h, base, range, masks);
        break;
    }
    }
    print_header(h, (uintptr_t)res);
    return res;
}

void arena_reset_L2(Arena *h, void *p, Arena_L2* al2, uintptr_t sub_mask, bool needs_zero)
{
    //
}

void arena_free_L2(Arena *h, void *p, Arena_L2* al2, uintptr_t sub_mask) { arena_reset_L2(h, p, al2, sub_mask, true); }

void arena_free_L1(Arena *h, void *p, Arena_L1* al1, Arena_L2* al2, uint32_t ridx,
                   bool needs_zero)
{
    const arena_size_table *stable = get_size_table(h->container_exponent);
    al2->L1_filter &= ~(1UL << (63 - ridx));
    if (al1->L1_allocations == arena_empty_mask) {
        uint32_t idx = delta_exp_to_idx((uintptr_t)p, (uintptr_t)al1, stable->exponents[2]);
        al2->L1_available &= ~(1UL << (63 - idx));
        // if you have released a whole l0 plate.
        // let the parent know.
        arena_reset_L2(h, al1, al2, ~(1UL << (63 - ridx)), needs_zero);
    }
}

void arena_free_L0(Arena *h, void *p, Arena_L0* al0, Arena_L2* al2, uint32_t ridx)
{
    const arena_size_table *stable = get_size_table(h->container_exponent);
    Arena_L1* al1 = (Arena_L1*)((uintptr_t)p & ~(stable->sizes[2] - 1));
    al2->L0_filter &= ~(1UL << (63 - ridx));
    if (al0->L0_allocations == arena_empty_mask_z) {
        uint32_t idx = delta_exp_to_idx((uintptr_t)p, (uintptr_t)al1, stable->exponents[1]);
        al1->L0_available &= ~(1UL << (63 - idx));
        // if you have released a whole l0 plate.
        // let the parent know.
        arena_free_L1(h, al0, al1, al2, ridx, false);
    }
}

void arena_free(Arena *h, void *p, bool dummy)
{
    const arena_size_table *stable = get_size_table(h->container_exponent);

    Arena_L2* al2 = (Arena_L2*)((uintptr_t)h & ~(os_page_size - 1));
    uint32_t ridx = delta_exp_to_idx((uintptr_t)p, (uintptr_t)al2, stable->exponents[2]);
    for (int i = 0; i < 3; i++) {
        if (((uintptr_t)p & (stable->sizes[i] - 1)) == 0) {
            // aligned to base level
            uintptr_t pc = ((uintptr_t)p & ~(stable->sizes[i + 1] - 1));
            uint32_t idx = delta_exp_to_idx((uintptr_t)p, pc, stable->exponents[i]);
            
            switch (i) {
            case 0:
                {
                    Arena_L0* al0 = (Arena_L0*)pc;
                    uint32_t range = get_range(idx, al0->L0_ranges);
                    uintptr_t sub_mask = ~reserve_range_idx(range, 63 - idx);
                    al0->L0_allocations = al0->L0_allocations & sub_mask;
                    al0->L0_ranges = al0->L0_ranges & sub_mask;
                    arena_free_L0(h, p, al0, al2, ridx);
                }
                break;
            case 1:
                {
                    Arena_L1* al1 = (Arena_L1*)pc;
                    uint32_t range = get_range(idx, al1->L1_ranges);
                    uintptr_t sub_mask = ~reserve_range_idx(range, 63 - idx);
                    al1->L1_allocations = al1->L1_allocations & sub_mask;
                    al1->L1_ranges = al1->L1_ranges & sub_mask;
                    arena_free_L1(h, p, al1, al2, ridx, true);
                }
                break;
            default:
                {
                    Arena_L2* al2 = (Arena_L2*)pc;
                    uint32_t range = get_range(idx, al2->L2_ranges);
                    uintptr_t sub_mask = ~reserve_range_idx(range, 63 - idx);
                    al2->L2_allocations = al2->L2_allocations & sub_mask;
                    al2->L2_ranges = al2->L2_ranges & sub_mask;
                    arena_free_L2(h, p, al2, sub_mask);
                }
                
                break;
            };
            h->num_allocations--;
            if (h->num_allocations == 0) {

            }

            break;
        }
    }
    print_header(h, (uintptr_t)p);
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
