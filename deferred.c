
#include "callocator.inl"
#include "pool.h"
#include "partition_allocator.h"
#include <stdatomic.h>
#include "arena.h"
#include "allocator.h"

extern PartitionAllocator *partition_allocator;
void arena_allocate_blocks(Allocator* alloc, Arena *a, int start_bit, int size_in_blocks) {
    // Clear area_mask bits.
    uint64_t area_add_mask =
        (size_in_blocks == 64 ? ~0ULL : ((1ULL << size_in_blocks) - 1)) << start_bit;
    // we are using this memory
    atomic_fetch_or_explicit(&a->in_use,
                              area_add_mask,
                              memory_order_release);
    // this region is not free anymore.
    atomic_fetch_and_explicit(&a->retained_but_free,
                              ~area_add_mask,
                              memory_order_release);
    // Clear range_mask bits if needed.
    if (size_in_blocks > 1) {
        uint64_t range_add_mask = (1ULL << start_bit) | (1ULL << (start_bit + size_in_blocks - 1));
        atomic_fetch_or_explicit(&a->ranges,
                                  range_add_mask,
                                  memory_order_relaxed);
    }
    
    if(a->in_use == UINT64_MAX)
    {
        // if arena is full... we remove from allocator queue.
        Queue *queue = &alloc->arenas[a->partition_id];
        if(arena_is_connected(a) || queue->head == a)
        {
            list_remove(queue, a);
        }
    }
}

void arena_retain_blocks(Allocator* alloc, Arena *a, int start_bit)
{
    uint64_t ranges = atomic_load(&a->ranges);
    uint32_t size_in_blocks = get_range((uint32_t)start_bit, ranges);
    uint64_t area_clear_mask =
        (size_in_blocks == 64 ? ~0ULL : ((1ULL << size_in_blocks) - 1)) << start_bit;
    atomic_fetch_and_explicit(&a->retained_but_free,
                              ~area_clear_mask,
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
        size_t area_size = region_size_from_partition_id(pid);
        AllocatorContext *d = NULL;
        
        const arena_size_table *stable = arena_get_size_table_by_idx(pid);
        Arena* h =  (Arena*)ALIGN_DOWN_2(p, area_size);
        c->owned = h->thread_id == a->thread_id;
        int32_t idx = delta_exp_to_idx((uintptr_t)p, (uintptr_t)h, stable->exponents[0]);
        if(idx == 0)
        {
            partition_allocator_free_blocks(partition_allocator, p, true);
            // we can decommit the memory in this case.
            a->c_slot->start = 0;
            a->c_slot->end = 0;
            return;
        }
        else
        {

            bool top_aligned = ((uintptr_t)p & (stable->sizes[0] - 1)) == 0;
            if(top_aligned)
            {
                allocator_release_slot(a, (AllocatorContext*)h);
                arena_free_blocks(a, h, idx);
                // if an arena becomes empty
                // we remove it from our lists of arenas...
                // but we still keep one arena around.
                a->c_slot->start = 0;
                a->c_slot->end = 0;
                return;
            }
            else
            {
                c->start = ((uintptr_t)h + idx*stable->sizes[0]);
                c->end = c->start + stable->sizes[0];
                d = (AllocatorContext*)c->start;
                allocator_release_slot(a, d);
            }
        }
        ((Block*)p)->next = d->free;
        d->free = NULL;
        c->items.next = p;
        c->num = 1;
        c->block_size = d->block_size;
        if(c->start > c->end)
        {
            int bb = 0;
        }
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
            AllocatorContext *d = (AllocatorContext*)c->start;
            d->free = c->items.next;
            
            if(!is_arena_type((AllocatorContext*)c->start))
            {
                Pool *pool = (Pool*)c->start;
                pool->num_used -= c->num;
                if(pool_is_full(pool))
                {
                    pool_post_free(pool, a);   
                }
            }
        }
        else
        {
            AllocatorContext *d = (AllocatorContext*)c->start;
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
