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

#define ARENA_BASE_SIZE_EXPONENT 22

static const uint32_t arena_level_offset = ARENA_BASE_SIZE_EXPONENT;

typedef struct arena_size_table_t
{
    uint64_t exponents[2];
    uint64_t sizes[2];
} arena_size_table;
typedef struct arena_empty_mask_table_t
{
    uint64_t sizes[3];
} arena_empty_mask_table;
static const arena_size_table arena_tables[NUM_AREA_PARTITIONS] = {
    {{16, 22}, {1 << 16, 1 << 22}},  {{17, 23}, {1 << 17, 1 << 23}},
    {{18, 24}, {1 << 18, 1 << 24}},  {{19, 25}, {1 << 19, 1 << 25}},
    {{20, 26}, {1 << 20, 1 << 26}},  {{21, 27}, {1 << 21, 1 << 27}},
    {{22, 28}, {1 << 22, 1 << 28}},  {{23, 29}, {1 << 23, 1 << 29}}, {{24, 30}, {1 << 24, 1 << 30}}};

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

static inline Arena* arena_get_header(uintptr_t addr)
{
    size_t asize = area_size_from_addr((uintptr_t)addr);
    uintptr_t base = ALIGN_DOWN_2((uintptr_t)addr, asize);
    uintptr_t header = base;
    return (Arena*)header;
}

Arena *arena_init(uintptr_t base_addr, int32_t idx, size_t arena_size_exponent);
static inline bool arena_is_connected(const Arena *s) { return s->prev != NULL || s->next != NULL; }

#endif // ARENA_H
