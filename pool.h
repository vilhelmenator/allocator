
#ifndef POOL_H
#define POOL_H

#include "callocator.inl"
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
    Block *new_free = (Block *)block;
    new_free->next = p->free;
    p->free = new_free;
}
void pool_init(Pool *p, const int8_t pidx, const uint32_t block_idx, const uint32_t block_size, const int32_t psize);
Block *pool_get_free_block(Pool *p);
void *pool_aquire_block(Pool *p);

#endif
