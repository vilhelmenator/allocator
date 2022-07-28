

#include "pool.h"



void pool_init(Pool *p, const int8_t pidx, const uint32_t block_idx, const uint32_t block_size, const int32_t psize)
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

Block *pool_get_free_block(Pool *p)
{
    if (p->num_used++ == 0) {
        Section *section = (Section *)((uintptr_t)p & ~(SECTION_SIZE - 1));
        section_reserve_idx(section, p->idx);
    }
    Block *res = p->free;
    p->free = res->next;
    return res;
}

void *pool_aquire_block(Pool *p)
{
    if (p->free != NULL) {
        return pool_get_free_block(p);
    }

    if (!pool_is_fully_commited(p)) {
        return pool_extend(p);
    }

    return NULL;
}
