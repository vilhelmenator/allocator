
#include "callocator.inl"
#include "pool.h"
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
        
        // Arenas
        /*
         size_t asize = area_size_from_partition_id(pid);
        // 64th part
        size_t psize = asize >> 6;
        c->start = ((uintptr_t)p & ~(psize - 1));
        c->end = c->start + psize;
        */
        
        Section *section = (Section *)((uintptr_t)p & ~(SECTION_SIZE - 1));
        Pool *pool = (Pool *)section_find_collection(section, p);
        c->start = (uintptr_t)pool;
        c->end = c->start + sizeof(Pool) + (pool->num_available * pool->block_size);
        Heap *d = (Heap*)pool;
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
            Pool *pool = (Pool*)c->start;
            Section *section = (Section *)((uintptr_t)pool & ~(SECTION_SIZE - 1));
            Area *area = area_from_addr((uintptr_t)pool);
            if(area_is_full(area))
            {
                AreaType at = area_get_type(area);
                Partition* partition = &a->part_alloc->area[at];
                int32_t aidx = partition_allocator_get_area_idx_from_queue(a->part_alloc, area, partition);
                partition->full_mask &= ~(1ULL << aidx);
            }
            
            pool->num_used -= c->num;
            if(pool->num_used == 0)
            {
                section_free_idx(section, pool->idx);
                // the last piece was returned so make the first item the start of the free
                pool->free = (Block*)((uintptr_t)pool + sizeof(Pool));
                pool->free->next = NULL;
                pool->num_committed = 1;
                init_heap((Heap*)pool);
            }
            else
            {
                Queue *queue = &a->part_alloc->pools[pool->block_idx];
                if(!pool_is_connected(pool) && queue->head != pool)
                {
                    list_enqueue(queue, pool);
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
