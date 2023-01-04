
#include "callocator.inl"
#include "pool.h"
#include "heap.h"
#include "partition_allocator.h"
#include "../cthread/cthread.h"

extern int64_t partition_owners[MAX_THREADS]; 
extern PartitionAllocator *partition_allocators[MAX_THREADS];
void deferred_move_thread_free(Heap* d)
{
    AtomicMessage *back = (AtomicMessage *)atomic_load_explicit(&d->thread_free.tail, memory_order_relaxed);
    AtomicMessage *curr = (AtomicMessage *)(uintptr_t)d->thread_free.head;
    
    if (curr == back)
    {
        return;
    }
    
    // skip over separator
    if (curr == d->thread_i) {
        curr = (AtomicMessage*)atomic_load_explicit(&curr->next, memory_order_acquire);
    }
    
    // loop between start and end address
    while (curr != back) {
        AtomicMessage *next = (AtomicMessage *)atomic_load_explicit(&curr->next, memory_order_acquire);
        if (next == NULL)
            break;
        
        ((Block*)curr)->next = d->free;
        d->free = (Block*)curr;
        curr = next;
    }
    d->thread_free.head = (uintptr_t)curr;
}

void deferred_thread_enqueue(AtomicQueue *queue, AtomicMessage *first, AtomicMessage *last)
{
    atomic_store_explicit(&last->next, (uintptr_t)NULL, memory_order_release); // last.next = null
    AtomicMessage *prev = (AtomicMessage *)atomic_exchange_explicit(&queue->tail, (uintptr_t)last,
                                                         memory_order_release); // swap back and last
    atomic_store_explicit(&prev->next, (uintptr_t)first, memory_order_release); // prev.next = first
}



// compute bounds and initialize
void deferred_cache_init(Allocator* a, void*p)
{
    int8_t pid = partition_from_addr((uintptr_t)p);
    if (pid >= 0 && pid < NUM_AREA_PARTITIONS) {
        deferred_cache*c = &a->c_deferred;
        const uint32_t part_id = partition_allocator_from_addr_and_part((uintptr_t)p, pid);
        c->owned = a->idx == partition_owners[part_id];
        size_t area_size = area_size_from_partition_id(pid);
        Area *area = (Area *)((uintptr_t)p & ~(area_size - 1));
        
        // Arenas
        /*
         size_t asize = area_size_from_partition_id(pid);
        // 64th part
        size_t psize = asize >> 6;
        c->start = ((uintptr_t)p & ~(psize - 1));
        c->end = c->start + psize;
        */
        Heap *d = NULL;
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
                a->c_cache.header = 0;
                return;
            }
                
        };
        
        ((Block*)p)->next = d->free;
        d->free = NULL;
        c->items.head = p;
        c->items.tail = p;
        c->num = 1;
    }
}

// release to owning structures.
void deferred_cache_release(Allocator* a, void* p)
{
    deferred_cache*c = &a->c_deferred;
    if(c->end != 0)
    {
        if(c->owned)
        {
            Heap *d = (Heap*)c->start;
            d->free = c->items.head;
            
            Area *area = area_from_addr((uintptr_t)c->start);
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
                    if(pool->num_used == 0)
                    {
                        pool_set_empty(pool);
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
                    int8_t pid = partition_from_addr((uintptr_t)section);
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
        }
        else
        {
            Heap *d = (Heap*)c->start;
            deferred_thread_enqueue(&d->thread_free, c->items.head, c->items.tail);
        }
        if(p)
        {
            deferred_cache_init(a, p);
        }
        else
        {
            c->start = UINT64_MAX;
            c->end = 0;
            c->num = 0;
        }
    }
}
