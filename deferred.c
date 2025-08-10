
#include "callocator.inl"
#include "pool.h"
#include "partition_allocator.h"
#include <stdatomic.h>
#include "arena.h"
#include "implicit_list.h"

/*
    When freeing memory, it is important that we can infer where the memory
 comes from. What subordinate structure and what partition.
 
 The address will tell us what partition it is residing in.
 The alignment will tell us what structure is holding it.
 
 A partition is split into sections.
 Partition 0 -> Aligned at 1TB. Address Range:[2TB - 3TB]
    - section: Aligned/Size at 256mb.
        - region: Aligned/Size at 4mb
 Partition 1 -> Aligned at 1TB. Address Range:[3TB - 4TB]
    - section: Aligned/Size at 512mb
        - region: Aligned/Size at 8mb
 ..
 Partition 8 -> Aligned at 1TB. Address Range:[10TB - 11TB]
    - section: Aligned/Size at 64gb
        - region: Aligned/Size at 1gb
 
 Implicit Lists are allocated as regions.
    - header matches region alignment.
 
 Arenas allocated as regions.
    - header matches region alignment.
        - any chunk handed out 1/64th the size and alignment.
 
 Pools are allocated as chunks from Arenas.
    - header matches chunk alignment.
 
 */
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

// compute bounds and initialize
void deferred_init(Allocator* a, void*p)
{
    int32_t pid = partition_id_from_addr((uintptr_t)p);
    if (pid >= 0 && pid < PARTITION_COUNT) {
        
        deferred_free*c = &a->c_deferred;
        // compute the size of the region.
        size_t area_size = region_size_from_partition_id(pid);
        alloc_base *d = NULL;
        // If the address is not aligned to the region size, we cannot use it.
        uint32_t c_exp = ARENA_CHUNK_SIZE_EXPONENT(pid);
        uint64_t c_size = ARENA_CHUNK_SIZE(pid);
        // c_size is the size of the chunk in bytes.
        uint64_t a_size = c_size * 64;
        // If the address is not aligned to the chunk size, we cannot use it.
        Arena* h =  (Arena*)ALIGN_DOWN_2(p, area_size);
        uint64_t thread_id = atomic_load(&h->thread_id);
        // claim the arena if it is not claimed by a thread.
        if(thread_id == -1)
        {   
            if(partition_allocator_claim_abandoned(partition_allocator, h))
            {
                // we can use this arena/implicit list.
                atomic_store_explicit(&h->thread_id, a->thread_id, memory_order_release);
                thread_id = a->thread_id;
            }
        }
        c->owned = thread_id == a->thread_id;
        int32_t idx = delta_exp_to_idx((uintptr_t)p, (uintptr_t)h, c_exp);
        slot_type st = get_base_type((alloc_base*)h);
        bool top_aligned = ((uintptr_t)p & (c_size - 1)) == 0;
        if(idx == 0)
        {
            // If this partition is not active, that would mean we can safely
            // free it from the partition allocator.
            // but only if it aligned to the very top.
            if(top_aligned)
            {
                // This would be a whole region that is allocated
                partition_allocator_free_blocks(partition_allocator, p, true);
                // we can decommit the memory in this case.
                a->c_slot.header = 0;
                return;
            }
            else
            {
                // this could be a pool or an implicit list.
                switch (st) {
                    case SLOT_POOL:
                    {
                        c->start = (uintptr_t)h;
                        c->end = c->start + c_size;
                        d = (alloc_base*)c->start;
                        break;
                    }
                    case SLOT_IMPLICIT:
                    {
                        // the implicit list will always be contained
                        // within a sub-region of a partition.
                        c->start = (uintptr_t)h;
                        c->end = c->start + a_size;
                        d = (alloc_base*)c->start;
                        break;
                    }
                    default:
                        break;
                }
            }
        }
        else
        {

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
        if(c->owned)
        {
            ((Block*)p)->next = d->deferred_free;
            d->deferred_free = NULL;
        }
        
        c->items.next = p;
        c->tail = (uintptr_t)p;
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
            d->deferred_free = c->items.next;
            slot_type st = get_base_type((alloc_base*)c->start);
            if(st == SLOT_POOL)
            {
                
                Pool *pool = (Pool*)c->start;
                Queue* pqueue = &a->pools[pool->block_idx];
                pool->num_used -= c->num;
                if(pool_is_unused(pool))
                {
                    // if the pool is unused, we can reset it.
                    pool_set_unused(pool, a);
                    if(is_not_connected_to_list(pqueue,pool))
                    {   
                        // if the pool is not connected to the list, we add it to the list.
                        list_enqueue(pqueue, pool);
                    }
                    else
                    {
                        // we move pools that have memory to the front.
                        list_move_to_front(pqueue, pool);
                    }
                }
            }
            else if(st ==  SLOT_IMPLICIT)
            {
                implicitList_move_deferred((ImplicitList*)c->start);
            }
        }
        else
        {
            // we don't own the deferred free list.
            slot_type st = get_base_type((alloc_base*)c->start);
            if(st == SLOT_POOL)
            {
                // we are releasing to a pool.
                alloc_base *d = (alloc_base*)c->start;
                // just increment the thread free counter.
                atomic_fetch_add_explicit(&d->thread_free_counter, c->num, memory_order_relaxed);
            }
            else if(st ==  SLOT_IMPLICIT)
            {
                // we are releasing to an implicit list as a batch.
                implicit_list_thread_free_batch((ImplicitList*)c->start, c->items.next, (Block*)c->tail);
            }
        }
        // reset the deferred free list.
        if(p)
        {   
            deferred_init(a, p);
        }
        else
        {
            c->owned = false;
            c->items.next = 0;
            c->tail = 0;
            c->start = UINT64_MAX;
            c->end = 0;
            c->num = 0;
        }
    }
}
