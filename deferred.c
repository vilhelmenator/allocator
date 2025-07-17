
#include "callocator.inl"
#include "pool.h"
#include "partition_allocator.h"
#include "../cthread/cthread.h"
#include "arena.h"

extern int64_t partition_owners[MAX_THREADS]; 
extern PartitionAllocator *partition_allocators[MAX_THREADS];



// compute bounds and initialize
void deferred_init(Allocator* a, void*p)
{
    int32_t pid = partition_id_from_addr((uintptr_t)p);
    if (pid >= 0 && pid < NUM_AREA_PARTITIONS) {
        
        deferred_free*c = &a->c_deferred;
        const int32_t part_id = partition_allocator_from_addr_and_part((uintptr_t)p, (uint32_t)pid);
        c->owned = a->idx == partition_owners[part_id];
        size_t area_size = area_size_from_partition_id(pid);
        Heap *d = NULL;
        
        const arena_size_table *stable = arena_get_size_table_by_idx(pid);
        Arena* h =  (Arena*)ALIGN_DOWN_2(p, area_size);
        int32_t idx = delta_exp_to_idx((uintptr_t)p, (uintptr_t)h, stable->exponents[0]);
        if(idx == 0)
        {
            // slab
            PartitionAllocator *_part_alloc = partition_allocators[part_id];
            partition_allocator_free_area(_part_alloc, (uintptr_t)p);
            a->c_slot.header = 0;
            return;
        }
        else
        {

            bool top_aligned = ((uintptr_t)p & (stable->sizes[0] - 1)) == 0;
            if(top_aligned)
            {
                uint32_t range = get_range(idx, h->ranges);
                uintptr_t sub_mask = ~reserve_range_idx(range, idx);
                h->allocations &= sub_mask;
                h->ranges &= sub_mask;
                a->c_slot.header = 0;
                return;
            }
            else
            {
                c->start = ((uintptr_t)h + idx*stable->sizes[0]);
                c->end = c->start + stable->sizes[0];
                d = (Heap*)c->start;
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
            Heap *d = (Heap*)c->start;
            d->free = c->items.next;
            
            if(!is_arena_type((Heap*)c->start))
            {
                Pool *pool = (Pool*)c->start;
                pool->num_used -= c->num;
                if(pool_is_full(pool))
                {
                    pool_set_full(pool, a);
                    Queue *queue = &a->pools[pool->block_idx];
                    if(pool_is_connected(pool) && queue->head == pool)
                    {
                        list_remove(queue, pool);
                    }
                }
            }
        }
        else
        {
            Heap *d = (Heap*)c->start;
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
