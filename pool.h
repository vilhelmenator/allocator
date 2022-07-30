
#ifndef POOL_H
#define POOL_H

#include "section.h"

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
        const uint32_t top_mask = UINT32_MAX;
        const int tz = __builtin_clz((uint32_t)as);
        const uint64_t bottom_mask = (top_mask >> (tz + 4));
        const uint64_t incr = (bottom_mask & as) > 0;
        const size_t row = (26 - tz) << 3;
        return (row + ((as >> (28 - tz)) & 0x7)) + incr - 1;
    }
}

static inline void *pool_extend(Pool *p)
{
    p->num_used++;
    return ((uint8_t *)p + sizeof(Pool) + (p->num_committed++ * p->block_size));
}

static inline bool pool_is_full(const Pool *p) { return p->num_used >= p->num_available; }
static inline bool pool_is_fully_commited(const Pool *p) { return p->num_committed >= p->num_available; }

static inline void pool_free_block(Pool *p, void *block)
{

    if (--p->num_used == 0) {
        Section *section = (Section *)((uintptr_t)p & ~(SECTION_SIZE - 1));
        section_free_idx(section, p->idx);

        p->free = (Block *)((uint8_t *)p + sizeof(Pool));
        p->free->next = NULL;
        return;
    }
    *(uint64_t **)block = (uint64_t *)p->free;
    p->free = (Block *)block;
}

static inline Block *pool_get_free_block(Pool *p)
{
    if (p->num_used++ == 0) {
        Section *section = (Section *)((uintptr_t)p & ~(SECTION_SIZE - 1));
        section_reserve_idx(section, p->idx);
    }
    Block *res = p->free;
    p->free = res->next;
    return res;
}

static inline void *pool_aquire_block(Pool *p)
{
    if (p->free != NULL) {
        return pool_get_free_block(p);
    }

    if (!pool_is_fully_commited(p)) {
        return pool_extend(p);
    }

    return NULL;
}

static void pool_init(Pool *p, const int8_t pidx, const uint32_t block_idx, const uint32_t block_size,
                      const int32_t psize)
{
    void *blocks = (uint8_t *)p + sizeof(Pool);
    const size_t block_memory = psize - sizeof(Pool) - sizeof(uintptr_t);
    const uintptr_t section_end = ((uintptr_t)p + (SECTION_SIZE - 1)) & ~(SECTION_SIZE - 1);
    const size_t remaining_size = section_end - (uintptr_t)blocks;
    p->idx = pidx;
    p->block_idx = block_idx;
    p->block_size = block_size;
    p->num_available = (int32_t)(MIN(remaining_size, block_memory) / block_size);
    p->num_committed = 1;
    p->num_used = 0;
    p->next = NULL;
    p->prev = NULL;
    p->free = (Block *)blocks;
    p->free->next = NULL;
}

#endif
