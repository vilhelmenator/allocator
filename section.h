//
//  section.h
//  MemPoolTests
//
//  Created by Vilhelm Sævarsson on 23.6.2022.
//  Copyright © 2022 Vilhelm Sævarsson. All rights reserved.
//
#ifndef SECTION_H
#define SECTION_H
#include "area.h"
#include "bitmask.h"
#include "callocator.inl"

static cache_align const uintptr_t size_clss_to_exponent[] = {
    22,
    17, // 128k
    19, // 512k
    22, // 4Mb
};
static inline bool section_is_connected(const Section *s) { return s->prev != NULL || s->next != NULL; }

static inline uint8_t section_get_collection_count(const Section *s)
{
    switch (s->type) {
    case ST_POOL_128K: {
        return 32;
    }
    case ST_POOL_512K: {
        return 8;
    }
    default: {
        return 1;
    }
    }
}

static inline void section_free_idx(Section *s, uint8_t i)
{
    bitmask_free_idx_lo(&s->active_mask, i);
    const bool section_empty = bitmask_is_empty_lo(&s->active_mask);
    if (section_empty) {
        Area *area = area_from_addr((uintptr_t)s);
        area_free_idx(area, s->idx);
    }
}

static inline bool section_is_claimed(const Section *s, const uint8_t idx) { return bitmask_is_set_lo(&s->constr_mask, idx); }

static inline void section_reserve_idx(Section *s, uint8_t i)
{
    bitmask_reserve_lo(&s->active_mask, i);
    Area *area = area_from_addr((uintptr_t)s);
    area_reserve_idx(area, s->idx);
}

static inline void section_claim_idx(Section *s, uint8_t i)
{
    bitmask_reserve_lo(&s->constr_mask, i);
    Area *area = area_from_addr((uintptr_t)s);
    area_claim_idx(area, s->idx);
}

static inline uint8_t section_reserve_next(Section *s) { return bitmask_allocate_bit_lo(&s->active_mask); }
static inline void section_reserve_all(Section *s)
{
    Area *area = area_from_addr((uintptr_t)s);
    area_reserve_idx(area, s->idx);
    bitmask_reserve_all_lo(&s->active_mask);
}
static inline void section_free_all(Section *s)
{
    Area *area = area_from_addr((uintptr_t)s);
    area_free_idx(area, s->idx);
    bitmask_free_all_lo(&s->active_mask);
}
static inline bool section_is_full(const Section *s)
{
    switch (s->type) {
    case ST_POOL_128K: {
        return (s->active_mask.whole & 0xffffffff) == 0xffffffff;
    }
    case ST_POOL_512K: {
        return (s->active_mask.whole & 0xff) == 0xff;
    }
    default: {
        return (s->active_mask.whole & 0x1) == 0x1;
    }
    }
}

static inline void *section_find_collection(Section *s, void *p)
{
    void *collection = (uint8_t *)s + sizeof(Section);
    switch (s->type) {
    case ST_POOL_4M: {
        return collection;
    }
    default: {
        const ptrdiff_t diff = (uint8_t *)p - (uint8_t *)collection;
        const uintptr_t exp = size_clss_to_exponent[s->type];
        const int32_t collection_size = 1 << exp;
        const int32_t idx = (int32_t)((size_t)diff >> exp);
        return (void *)((uint8_t *)collection + collection_size * idx);
    }
    };
}

static inline uintptr_t section_get_collection(Section *s, int8_t idx, const int32_t psize)
{
    return (uintptr_t)((uint8_t *)s + sizeof(Section) + psize * idx);
}

#endif
