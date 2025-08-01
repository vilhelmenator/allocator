
#include "callocator.inl"
#include "pool.h"
#include "partition_allocator.h"
#include <stdatomic.h>
#include "arena.h"

extern PartitionAllocator *partition_allocator;
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
        // if arena is full... we remove from allocator queue.
        if(arena_is_connected(a) || queue->head == a)
        {
            list_remove(queue, a);
            list_append(queue, a);
        }
    }
    else
    {   
        if(!arena_is_connected(a) && queue->head != a)
        {
            list_enqueue(queue, a);
        }
    }
}

void arena_unuse_blocks(Allocator* alloc, Arena *a, int start_bit)
{
    uint64_t ranges = atomic_load(&a->ranges);
    uint32_t size_in_blocks = get_range((uint32_t)start_bit, ranges);
    uint64_t area_clear_mask =
        (size_in_blocks == 64 ? ~0ULL : ((1ULL << size_in_blocks) - 1)) << start_bit;
    atomic_fetch_and_explicit(&a->in_use,
                              ~area_clear_mask,
                              memory_order_release);
}

void arena_use_blocks(Allocator* alloc, Arena *a, int start_bit)
{
    uint64_t ranges = atomic_load(&a->ranges);
    uint32_t size_in_blocks = get_range((uint32_t)start_bit, ranges);
    uint64_t area_clear_mask =
        (size_in_blocks == 64 ? ~0ULL : ((1ULL << size_in_blocks) - 1)) << start_bit;
    atomic_fetch_or_explicit(&a->in_use,
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

    // Clear range_mask bits if needed.
    if (size_in_blocks > 1) {
        uint64_t range_clear_mask = (1ULL << start_bit) | (1ULL << (start_bit + size_in_blocks - 1));
        atomic_fetch_and_explicit(&a->ranges,
                                  ~range_clear_mask,
                                  memory_order_relaxed);
    }
    if(a->in_use > 1)
    {
        Queue *queue = &alloc->arenas[a->partition_id];
        if(!arena_is_connected(a) && queue->head != a)
        {
            list_enqueue(queue, a);
        }
    }
    else
    {
        // if arena is full... we remove from allocator queue.
        //Queue *queue = &alloc->arenas[a->partition_id];
        //if(arena_is_connected(a) || queue->head == a)
        //{
        //    list_remove(queue, a);
        //}
        //partition_allocator_free_blocks(partition_allocator, a, true);
    }
}

void purge_unused_pools(Allocator* alloc, Arena *a)
{
    uint64_t used = atomic_load(&a->in_use);
    if(used == 0)
    {
        // for each bit that is set in the active mask.
        // those are in cached queues. Only pools set the active flag
        // in arenas.
        uint64_t active = atomic_load(&a->active);
        uint64_t area_clear_mask = 1;
        atomic_fetch_and_explicit(&a->active,
                                  area_clear_mask,
                                  memory_order_release);
        
        int32_t active_idx = get_next_mask_idx(active, 1);
        while (active_idx != -1) {
            
            // get pool address.
            int8_t pid = partition_id_from_addr((uintptr_t)a);
            size_t area_size = region_size_from_partition_id(pid);
            size_t block_size = area_size >> 6;
            Pool* inactive_pool = (Pool*)((uintptr_t)a + (active_idx * block_size));
            Queue* pqueue = &alloc->pools[inactive_pool->block_idx];
            if(pool_is_connected(inactive_pool) || pqueue->head == inactive_pool)
            {
                list_remove(pqueue, inactive_pool);
            }
            active_idx = get_next_mask_idx(active, active_idx + 1);
        }
        
        Queue *queue = &alloc->arenas[a->partition_id];
        if(!arena_is_connected(a) && queue->head != a)
        {
            list_enqueue(queue, a);
        }
    }
}

// compute bounds and initialize
void deferred_init(Allocator* a, void*p)
{
    int32_t pid = partition_id_from_addr((uintptr_t)p);
    if (pid >= 0 && pid < PARTITION_COUNT) {
        
        deferred_free*c = &a->c_deferred;
        size_t area_size = region_size_from_partition_id(pid);
        alloc_base *d = NULL;
        
        uint32_t c_exp = ARENA_CHUNK_SIZE_EXPONENT(pid);
        uint32_t c_size = ARENA_CHUNK_SIZE(pid);
        Arena* h =  (Arena*)ALIGN_DOWN_2(p, area_size);
        c->owned = h->thread_id == a->thread_id;
        int32_t idx = delta_exp_to_idx((uintptr_t)p, (uintptr_t)h, c_exp);
        if(idx == 0)
        {
            // This would be a whole region that is allocated
            // since an arena would have its first area reserved for
            // masks.
            partition_allocator_free_blocks(partition_allocator, p, true);
            // we can decommit the memory in this case.
            a->c_slot.header = 0;
            return;
        }
        else
        {

            bool top_aligned = ((uintptr_t)p & (c_size - 1)) == 0;
            if(top_aligned)
            {
                arena_free_blocks(a, h, idx);
                // if an arena becomes empty
                // we remove it from our lists of arenas...
                // but we still keep one arena around.
                a->c_slot.header = 0;
                return;
            }
            else
            {
                c->start = ((uintptr_t)h + idx*c_size);
                c->end = c->start + c_size;
                d = (alloc_base*)c->start;
            }
        }
        ((Block*)p)->next = d->free;
        d->free = NULL;
        c->items.next = p;
        c->num = 1;
        c->block_size = d->block_size;
    }
}

// release to owning structures.
void deferred_release(Allocator* a, void* p)
{
    deferred_free*c = &a->c_deferred;
    if(c->end != 0)
    {
        if(c->owned)
        {
            alloc_base *d = (alloc_base*)c->start;
            d->free = c->items.next;
            
            if(!is_arena_type((alloc_base*)c->start))
            {
                
                Pool *pool = (Pool*)c->start;
                Queue* pqueue = &a->pools[pool->block_idx];
                pool->num_used -= c->num;
                if(pool_is_unused(pool))
                {
                    pool_post_unused(pool, a);
                    if(!pool_is_connected(pool) && pqueue->head != pool)
                    {
                        list_enqueue(pqueue, pool);
                    }
                }
            }
        }
        else
        {
            alloc_base *d = (alloc_base*)c->start;
            atomic_fetch_add_explicit(&d->thread_free_counter,c->num,memory_order_relaxed);
        }
        if(p)
        {
            deferred_init(a, p);
        }
        else
        {
            c->start = UINT64_MAX;
            c->end = 0;
            c->num = 0;
        }
    }
}
