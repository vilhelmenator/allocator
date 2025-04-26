
#include "callocator.inl"
#include "pool.h"
#include "heap.h"
#include "partition_allocator.h"
#include "../cthread/cthread.h"
#include "arena.h"

extern int64_t partition_owners[MAX_THREADS]; 
extern PartitionAllocator *partition_allocators[MAX_THREADS];



// compute bounds and initialize
void deferred_init(Allocator* a, void*p)
{
    int8_t pid = partition_id_from_addr((uintptr_t)p);
    if (pid >= 0 && pid < NUM_AREA_PARTITIONS) {
        
        deferred_free*c = &a->c_deferred;
        const uint32_t part_id = partition_allocator_from_addr_and_part((uintptr_t)p, pid);
        c->owned = a->idx == partition_owners[part_id];
        size_t area_size = area_size_from_partition_id(pid);
        Heap *d = NULL;
        
#ifdef ARENA_PATH
        const arena_size_table *stable = arena_get_size_table_by_idx(pid);
        Arena* h =  (Arena*)ALIGN_DOWN_2(p, area_size);
        
        int32_t idx = delta_exp_to_idx((uintptr_t)p, (uintptr_t)h, stable->exponents[0]);
        if(idx == 0)
        {
            // slab
            PartitionAllocator *_part_alloc = partition_allocators[part_id];
            partition_allocator_free_area(_part_alloc, p);
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
#else
        Area *area = (Area *)((uintptr_t)p & ~(area_size - 1));
        
        // Arenas
        /*
         size_t asize = area_size_from_partition_id(pid);
        // 64th part
        size_t psize = asize >> 6;
        c->start = ((uintptr_t)p & ~(psize - 1));
        c->end = c->start + psize;
        */
        
        switch(area_get_container_type(area))
        {
            case CT_SECTION:
            {
                Section *section = (Section *)((uintptr_t)p & ~(SECTION_SIZE - 1));
                void* collection = section_find_collection(section, p);
                c->start = (uintptr_t)collection;
                if(section->type != ST_HEAP_4M)
                {
                    Pool* pool = (Pool*)collection;
                    c->end = c->start + sizeof(Pool) + (pool->num_available * pool->block_size);
                }
                else
                {
                    ImplicitList* il = (ImplicitList*)collection;
                    c->end = c->start + il->total_memory;
                }
                d = (Heap*)collection;
                break;
            }
            case CT_HEAP:
            {
                ImplicitList *il = (ImplicitList *)((uintptr_t)area + sizeof(Area));
                c->start = (uintptr_t)il;
                c->end = c->start + il->total_memory;
                d = (Heap*)il;
                break;
            }
            default:
            {
                PartitionAllocator *_part_alloc = partition_allocators[part_id];
                partition_allocator_free_area(_part_alloc, area);
                a->c_slot.header = 0;
                return;
            }
                
        };
#endif
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
            
#ifdef ARENA_PATH
            
            if(!is_arena_type((Heap*)c->start))
            {
                Pool *pool = (Pool*)c->start;
                pool->num_used -= c->num;
                if(pool_is_full(pool))
                {
                    pool_set_full(pool, a);
                }
            }
#else
            
            Area* area = area_from_addr((uintptr_t)c->start);
            if(area_is_full(area))
            {
                AreaType at = area_get_type(area);
                Partition* partition = &a->part_alloc->area[at];
                int32_t aidx = partition_allocator_get_area_idx_from_queue(a->part_alloc, area, partition);
                partition->full_mask &= ~(1ULL << aidx);
            }
            if(area_get_container_type(area) == CT_SECTION)
            {
                Section *section = (Section *)((uintptr_t)c->start & ~(SECTION_SIZE - 1));

                if(section->type != ST_HEAP_4M)
                {
                    Pool *pool = (Pool*)c->start;
                    pool->num_used -= c->num;
                    if(pool_is_full(pool))
                    {
                        pool_set_full(pool, a);
                        Queue *queue = &a->part_alloc->pools[pool->block_idx];
                        if(pool_is_connected(pool) && queue->head == pool)
                        {
                            list_remove(queue, pool);
                        }
                    }
                    else
                    {
                        Queue *queue = &a->part_alloc->pools[pool->block_idx];
                        if(!pool_is_connected(pool) && queue->head != pool)
                        {
                            list_enqueue(queue, pool);
                        }
                    }
                }
                else
                {
                    ImplicitList *heap = (ImplicitList*)c->start;
                    heap->num_allocations -= c->num;
                    if(heap->num_allocations == 0)
                    {
                        implicitList_freeAll(heap);
                        init_heap((Heap*)heap);
                    }
                    else
                    {
                        uint32_t heapIdx = area_get_type((Area *)section);
                        // if the free pools list is empty.
                        if (!implicitList_is_connected(heap)) {
                            // reconnect
                            Queue *queue = &a->part_alloc->heaps[heapIdx];
                            if (queue->head != heap && queue->tail != heap) {
                                list_enqueue(queue, heap);
                            }
                        }
                    }
                }
            
                if (!section_is_connected(section)) {
                    int8_t pid = partition_id_from_addr((uintptr_t)section);
                    const uint32_t part_id = partition_allocator_from_addr_and_part((uintptr_t)section, pid);
                    PartitionAllocator *_part_alloc = partition_allocators[part_id];
                    Queue *sections = _part_alloc->sections;
                    if (sections->head != section && sections->tail != section) {
                        list_enqueue(sections, section);
                    }
                }
            }
            else
            {
                ImplicitList *heap = (ImplicitList*)c->start;
                heap->num_allocations -= c->num;
                if(heap->num_allocations == 0)
                {
                    implicitList_freeAll(heap);
                    init_heap((Heap*)heap);
                }
                else
                {
                    uint32_t heapIdx = area_get_type((Area *)area);
                    // if the free pools list is empty.
                    if (!implicitList_is_connected(heap)) {
                        // reconnect
                        Queue *queue = &a->part_alloc->heaps[heapIdx];
                        if (queue->head != heap && queue->tail != heap) {
                            list_enqueue(queue, heap);
                        }
                    }
                }
            }
#endif
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
