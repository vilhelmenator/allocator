
#include "callocator.inl"
#include "pool.h"
#include "heap.h"
#include "partition_allocator.h"
#include "../cthread/cthread.h"
#include "arena.h"

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
        Arena_L2* al2 =  (Arena_L2*)ALIGN_DOWN_2(p, area_size);
        Arena *h = (Arena *)((uintptr_t)al2 + sizeof(Arena_L2));
        int32_t idx = arena_get_local_idx(h, (uintptr_t)p, (uintptr_t)al2, AL_HIGH);
        Arena_L1 * al1 = (Arena_L1*)((uintptr_t)al2 + idx*stable->sizes[2]);
        bool top_aligned = ((uintptr_t)p & (stable->sizes[2] - 1)) == 0;
        uint32_t out_start = 0;
        uint32_t out_range = 0;
        if (top_aligned) {
            if(idx == 0)
            {
                // slab
                PartitionAllocator *_part_alloc = partition_allocators[part_id];
                partition_allocator_free_area(_part_alloc, p);
                a->c_slot.header = 0;
            }
            else
            {
                // a pool will never get more than 1 block at the high level, so we are safe to just release this.
                uint32_t range = get_range(idx, al2->L2_ranges);
                uintptr_t sub_mask = ~reserve_range_idx(range, idx);
                arena_free_L2(h, p, al2, sub_mask);
            }
        }
        else
        {
            if(!is_arena_type((Heap*)al1))
            {
                c->start = (uintptr_t)al1;
                c->end = c->start + stable->sizes[2];
                d = (Heap*)al1;
            }
            else
            {
                int32_t midx = arena_get_local_idx(h, (uintptr_t)p, (uintptr_t)al1, AL_MID);
                Arena_L0 * al0 = (Arena_L0*)((uintptr_t)al1 + midx*stable->sizes[1]);
                
                if(!is_arena_type((Heap*)al0))
                {
                    arena_get_start_and_range(midx, al1->L1_allocations, al1->L1_ranges, &out_start, &out_range);
                    c->start = (uintptr_t)al1 + out_start*stable->sizes[1];
                    c->end = c->start + out_range*stable->sizes[1];
                    d = (Heap*)al1;
                }
                else
                {
                    c->start = (uintptr_t)al0;
                    c->end = c->start + stable->sizes[1];
                    d = (Heap*)al1;
                }
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
        c->items.head = p;
        c->items.tail = p;
        c->num = 1;
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
            d->free = c->items.head;
            Area* area = area_from_addr((uintptr_t)c->start);
            
#ifdef ARENA_PATH
            Arena* header = (Arena*)((uintptr_t)area + sizeof(Arena_L2));
            if(!is_arena_type((Heap*)c->start))
            {
                //
                Pool *pool = (Pool*)c->start;
                pool->num_used -= c->num;
                if(pool->num_used == 0)
                {
                    header->num_allocations--;
                    if(header->num_allocations == 0)
                    {
                        AreaType at = area_get_type(area);
                        Partition* partition = &a->part_alloc->area[at];
                        int32_t aidx = partition_allocator_get_area_idx_from_queue(a->part_alloc, area, partition);
                        partition->full_mask &= ~(1ULL << aidx);
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
#else
            
            
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
            deferred_thread_enqueue(&d->thread_free, c->items.head, c->items.tail);
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
