#include "arena.h"
#include "pool.h"
#include "partition_allocator.h"

void arena_allocate_blocks(Allocator* alloc, Arena *a, int start_bit, int size_in_blocks) {
    // Clear area_mask bits.
    uint64_t area_add_mask =
        (size_in_blocks == 64 ? ~0ULL : ((1ULL << size_in_blocks) - 1)) << start_bit;
    // we are using this memory
    atomic_fetch_or_explicit(&a->active,
                              area_add_mask,
                              memory_order_release);
    
    // Set range_mask bits if needed.
    if (size_in_blocks > 1) {
        uint64_t range_add_mask = (1ULL << start_bit) | (1ULL << (start_bit + size_in_blocks - 1));
        atomic_fetch_or_explicit(&a->ranges,
                                  range_add_mask,
                                  memory_order_relaxed);
    }
    
    Queue *queue = &alloc->arenas[a->partition_id];
    if(a->active == UINT64_MAX)
    {
        // if arena is full... we move it to the back of the list.
        if(is_connected_to_list(queue, a))
        {
            list_move_to_back(queue, a);
        }
    }
    else
    {   
        if(is_not_connected_to_list(queue, a))
        {
            list_enqueue(queue, a);
        }
    }
}

void arena_unuse_blocks(Arena *a, int start_bit)
{
    uint64_t ranges = atomic_load(&a->ranges);
    uint32_t size_in_blocks = get_range((uint32_t)start_bit, ranges);
    uint64_t area_clear_mask =
        (size_in_blocks == 64 ? ~0ULL : ((1ULL << size_in_blocks) - 1)) << start_bit;
    atomic_fetch_and_explicit(&a->in_use,
                              ~area_clear_mask,
                              memory_order_release);
    // these are not guarenteed zero anymore
    atomic_fetch_and_explicit(&a->zero,
                              ~area_clear_mask,
                              memory_order_release);
    if(a->in_use <= 1)
    {
        a->last_used = current_time_ms();
    }
}

void arena_use_blocks(Arena *a, int start_bit)
{
    uint64_t ranges = atomic_load(&a->ranges);
    uint32_t size_in_blocks = get_range((uint32_t)start_bit, ranges);
    uint64_t area_clear_mask =
        (size_in_blocks == 64 ? ~0ULL : ((1ULL << size_in_blocks) - 1)) << start_bit;
    atomic_fetch_or_explicit(&a->in_use,
                              area_clear_mask,
                              memory_order_release);
}

void arena_set_dirty_blocks(Arena *a, int start_bit)
{
    uint64_t ranges = atomic_load(&a->ranges);
    uint32_t size_in_blocks = get_range((uint32_t)start_bit, ranges);
    uint64_t area_clear_mask =
        (size_in_blocks == 64 ? ~0ULL : ((1ULL << size_in_blocks) - 1)) << start_bit;
    atomic_fetch_or_explicit(&a->dirty,
                              area_clear_mask,
                              memory_order_release);
}

void arena_free_blocks(Allocator* alloc, Arena *a, int start_bit) {
    
    uint64_t ranges = atomic_load(&a->ranges);
    uint32_t size_in_blocks = get_range((uint32_t)start_bit, ranges);
    // Clear area_mask bits.
    uint64_t area_clear_mask =
        (size_in_blocks == 64 ? ~0ULL : ((1ULL << size_in_blocks) - 1)) << start_bit;
    atomic_fetch_and_explicit(&a->in_use,
                              ~area_clear_mask,
                              memory_order_release);
    // these are not guarenteed zero anymore
    atomic_fetch_and_explicit(&a->zero,
                              ~area_clear_mask,
                              memory_order_release);
    // Clear range_mask bits if needed.
    if (size_in_blocks > 1) {
        uint64_t range_clear_mask = (1ULL << start_bit) | (1ULL << (start_bit + size_in_blocks - 1));
        atomic_fetch_and_explicit(&a->ranges,
                                  ~range_clear_mask,
                                  memory_order_relaxed);
    }
    if(a->in_use >= 1)
    {
        Queue *queue = &alloc->arenas[a->partition_id];
        if(!arena_is_connected(a) && queue->head != a)
        {
            list_enqueue(queue, a);
        }
    }
}

void arena_clear_dirty(Arena *a)
{
    // for every dirty block, we clear the dirty bit.  
    uint32_t c_size = (uint32_t)ARENA_CHUNK_SIZE(a->partition_id);
    uint64_t dirty = atomic_load(&a->dirty);
    if(dirty != 0)
    {
        uint64_t new_mask = 0ULL;
        if(atomic_compare_exchange_strong(&a->dirty, &dirty, new_mask))
        {
            int32_t chunk_idx = get_next_mask_idx(dirty, 0);
            while (chunk_idx != -1) {
                Pool* pool = (Pool*)((uintptr_t)a + (chunk_idx * c_size));
                pool_claim_thread_frees(pool);
                chunk_idx = get_next_mask_idx(dirty, chunk_idx + 1);
            }
        }
    }
}


bool arena_free_active(Allocator* alloc, Arena *a, bool decommit)
{
    // for every dirty block, we clear the dirty bit.
    uint32_t c_size = (uint32_t)ARENA_CHUNK_SIZE(a->partition_id);
    uint64_t in_use = atomic_load(&a->in_use);
    uint64_t active = atomic_load(&a->active);
    if(in_use <= 1 && active != 0)
    {   
        uint64_t new_mask = 0ULL;
        if(atomic_compare_exchange_strong(&a->active, &active, new_mask))
        {
            int32_t chunk_idx = get_next_mask_idx(active, 0);
            while (chunk_idx != -1) {
                Pool* pool = (Pool*)((uintptr_t)a + (chunk_idx * c_size));
                Queue* pqueue = &alloc->pools[pool->block_idx];
                list_remove(pqueue, pool);
                chunk_idx = get_next_mask_idx(active, chunk_idx + 1);
            }
            Queue*aqueue = &alloc->arenas[a->partition_id];
            // remove from the list before we decommit
            list_remove(aqueue, a);
            partition_allocator_free_blocks(partition_allocator, a, decommit);
            return true;
        }   
    }
    return false;
}

bool arena_reallocate(Arena *a, int32_t start_idx, int32_t new_size, bool zero)
{
    // how many blocks does the new_size need
    int32_t aexp = ARENA_CHUNK_SIZE_EXPONENT( a->partition_id);
    int32_t range = get_range(start_idx, a->ranges);
    
    // divide by the exponent to get the number of blocks
    int32_t num_blocks = new_size >> aexp;
    int32_t rem = new_size - (num_blocks * (1 << aexp));
    if(rem > 0)
    {
        // If there is a remainder, we need to allocate an additional block
        num_blocks++;
    }
    // Compute the number of additional blocks needed
    int32_t additional_blocks = num_blocks - range;
    uint64_t ranges = atomic_load(&a->ranges);
    uint64_t in_use = atomic_load(&a->in_use);
    uint64_t zeros = atomic_load(&a->zero);
    if (additional_blocks > 0) {
        // we need to check if the blocks after the start idx and range
        uint64_t new_block_mask = ((1ULL << additional_blocks) - 1) << (start_idx + range);
        if ((in_use & new_block_mask) == 0) {
            if(zero)
            {
                // if we are zeroing, we need to check if the blocks are zeroed
                if ((zeros & new_block_mask) != new_block_mask) {
                    // we can't allocate zeroed memory
                    return false;
                }
            }
            // If the region is free, we can allocate it
            atomic_fetch_or(&a->in_use, new_block_mask);
            // update the range ... 
            // first we need to clear it.
            ranges &= ~new_block_mask;
            uint64_t new_range = apply_range(num_blocks, start_idx);
            atomic_fetch_or(&a->ranges, new_range);

            return true;
        }
    }
    return false;
}
