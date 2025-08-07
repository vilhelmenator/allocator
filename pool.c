#include "pool.h"

void pool_init(Pool *p, const uint8_t pidx, const uint32_t block_idx, const int32_t psize)
{
    p->idx = pidx << 4 | SLOT_POOL;
    p->block_idx = block_idx;
    p->block_size = pool_sizes[block_idx];
    p->num_committed = 0;
    p->alignment = (uint32_t)(1ULL << __builtin_ctzll(p->block_size));
    p->thread_free_counter = 0;
    p->deferred_free = NULL;
    p->num_used = 0;
    p->next = NULL;
    p->prev = NULL;
    p->free = NULL;
    
    void *blocks = pool_base_address(p);
    const uintptr_t section_end = ALIGN_UP_2((uintptr_t)blocks, psize);
    
    const size_t block_memory = psize - ALIGN_UP_2(sizeof(Pool), p->alignment);
    const size_t remaining_size = section_end - (uintptr_t)blocks;
    p->num_available = (uint32_t)(MIN(remaining_size, block_memory)/p->block_size);
}
