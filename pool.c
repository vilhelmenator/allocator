#include "pool.h"

void pool_init(Pool *p, const uint8_t pidx, const uint32_t block_idx, const int32_t psize)
{
    p->idx = pidx << 4 | SLOT_POOL;
    p->block_idx = block_idx;
    p->block_size = pool_sizes[block_idx];
    p->num_committed = 0;
    p->alignment = (uint32_t)(1ULL << __builtin_ctzll(p->block_size));
    p->thread_free_counter = 0;
    p->thread_free = NULL;
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

void pool_thread_free_batch(Pool* pool, Block* head, Block* tail, uint32_t num) {
    
    // update our count
    atomic_fetch_add_explicit(&pool->thread_free_counter, num, memory_order_relaxed);
    // Link the batch to current head
    tail->next = atomic_load_explicit(&pool->thread_free, memory_order_relaxed);
    
    // CAS loop to atomically prepend the batch
    while (!atomic_compare_exchange_weak_explicit(
        &pool->thread_free,
        &tail->next,  // Expected current head
        head,         // New head (start of batch)
        memory_order_release,
        memory_order_relaxed
    )) {
        // Update tail->next if CAS fails
        tail->next = atomic_load_explicit(&pool->thread_free, memory_order_relaxed);
    }
}

// Moves all thread_free blocks to deferred_free (call from owning thread)
void pool_claim_thread_frees(Pool* pool) {
    uint64_t thread_free_count = atomic_load(&pool->thread_free_counter);
    // Atomically extract the entire thread_free list
    Block* head = atomic_exchange_explicit(
        &pool->thread_free,
        NULL,
        memory_order_acquire  // Ensures we see all prior releases
    );
    atomic_fetch_sub_explicit(&pool->thread_free_counter, thread_free_count, memory_order_relaxed);
    // Prepend to deferred_free (no atomic needed - owner thread only)
    if (head) {
        Block* tail = head;
        while (tail->next) {
            tail = tail->next;
        }
        tail->next = pool->deferred_free;
        pool->deferred_free = head;
    }
}