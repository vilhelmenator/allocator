

#include "allocator.h"
#include "partition_allocator.h"
#include "pool.h"
#include "heap.h"
#include "os.h"

#include "../cthread/cthread.h"

extern PartitionAllocator *partition_allocators[MAX_THREADS];
cache_align int64_t partition_owners[MAX_THREADS];
cache_align Allocator *allocator_list[MAX_THREADS];
static const int32_t thread_message_imit = 100;

static spinlock partition_lock = {0};
int8_t reserve_any_partition_set(void)
{
    spinlock_lock(&partition_lock);
    int32_t reserved_id = -1;
    for (int i = 0; i < 1024; i++) {
        if (partition_owners[i] == -1) {
            partition_owners[i] = i;
            reserved_id = i;
            break;
        }
    }
    spinlock_unlock(&partition_lock);
    return reserved_id;
}
int8_t reserve_any_partition_set_for(const int32_t midx)
{
    spinlock_lock(&partition_lock);
    int32_t reserved_id = -1;
    for (int i = 0; i < 1024; i++) {
        if (partition_owners[i] == -1) {
            partition_owners[i] = midx;
            reserved_id = i;
            break;
        }
    }
    spinlock_unlock(&partition_lock);
    return reserved_id;
}
bool reserve_partition_set(const int32_t idx, const int32_t midx)
{
    spinlock_lock(&partition_lock);
    if (partition_owners[idx] == -1) {
        partition_owners[idx] = midx;
        return true;
    }
    spinlock_unlock(&partition_lock);
    return false;
}

void release_partition_set(const int32_t idx)
{
    if (idx >= 0) {
        spinlock_lock(&partition_lock);
        partition_owners[idx] = -1;
        spinlock_unlock(&partition_lock);
    }
}

Allocator *allocator_aquire(size_t idx)
{
    if (allocator_list[idx] == NULL) {
        PartitionAllocator *part_alloc = partition_allocator_aquire(idx);
        Allocator *new_alloc = (Allocator *)((uintptr_t)part_alloc - ALIGN_CACHE(sizeof(Allocator)));
        allocator_list[idx] = new_alloc;
    }
    return allocator_list[idx];
}

void allocator_set_cached_pool(Allocator *a, Pool *p)
{
    uintptr_t start_addr = (uintptr_t)p;
    if (start_addr == a->cache.header) {
        return;
    }
    
    a->cache.header = (uintptr_t)p;
    a->cache.end = sizeof(Pool) + (p->num_available * p->block_size);
    a->cache.remaining_blocks = p->num_available - p->num_committed;
    a->cache.block_size = p->block_size;
    a->cache.cache_type = CACHE_POOL;
    a->cache.freed_blocks = 0;
    if(a->cache.remaining_blocks)
    {
        // reserve all memory from the pool
        p->num_used = p->num_available;
        p->num_committed = p->num_available;
    }
}

void allocator_set_cached_arena(Allocator *a, Arena *p)
{
    uintptr_t start_addr = (uintptr_t)p;
    if (start_addr == a->cache.header) {
        return;
    }
    
    a->cache.header = start_addr;
    a->cache.cache_type = CACHE_ARENA;
}


static inline void allocator_release_cache(Allocator *a)
{
    if (a->cache.header) {
        if(a->cache.cache_type == CACHE_POOL)
        {
            Pool* p = (Pool*)a->cache.header;
            p->num_used -= a->cache.remaining_blocks;
            p->num_committed -= a->cache.remaining_blocks;
            if (!pool_is_empty(p)) {
                Queue *queue = &a->part_alloc->pools[p->block_idx];
                list_enqueue(queue, p);
            }
            
            //p->num_used -= a->cache.freed_blocks;
            if(pool_is_full(p))
            {
                pool_post_free(p);
            }
            else
            {
                //pool_post_reserved(p);
            }
        }
    }
    a->cache.header = 0;
}

static inline void *allocator_malloc_from_cache(Allocator *a, size_t s)
{
    if (s == a->prev_size) {
        if(a->cache.remaining_blocks)
        {
            return (void*)(uintptr_t)(a->cache.header + a->cache.end) - (a->cache.remaining_blocks-- * a->cache.block_size);
        }
        
        if(a->cache.cache_type == CACHE_POOL)
        {
            Pool* p = (Pool*)a->cache.header;
            void *block = pool_aquire_block(p);
            if(pool_is_empty(p))
            {
                allocator_release_cache(a);
            }
            
            return block;
        }
    }
    allocator_release_cache(a);
    return NULL;
}

void allocator_thread_enqueue(message_queue *queue, message *first, message *last)
{
    atomic_store_explicit(&last->next, (uintptr_t)NULL,
                          memory_order_release); // last.next = null
    message *prev = (message *)atomic_exchange_explicit(&queue->tail, (uintptr_t)last,
                                                        memory_order_release); // swap back and last
    atomic_store_explicit(&prev->next, (uintptr_t)first,
                          memory_order_release); // prev.next = first
}

static inline void allocator_flush_thread_free(Allocator *a)
{
    if (a->thread_free_part_alloc != NULL) {
        // get the first and last item of the tf queue
        message *lm = partition_allocator_get_last_message(a->part_alloc);
        if (lm != NULL) {
            allocator_thread_enqueue(a->thread_free_part_alloc->thread_free_queue, a->part_alloc->thread_messages, lm);
            a->part_alloc->message_count = 0;
        }
    }
}

void allocator_thread_free(Allocator *a, void *p, const uint64_t pid)
{
    PartitionAllocator *_part_alloc = partition_allocators[pid];
    if (_part_alloc != a->thread_free_part_alloc) {
        allocator_flush_thread_free(a);
        a->thread_free_part_alloc = _part_alloc;
    }
    partition_allocator_thread_free(a->part_alloc, p);
    if (a->part_alloc->message_count > thread_message_imit) {
        allocator_flush_thread_free(a);
    }
}

void allocator_free_from_section(Allocator *a, void *p, Section *section, uint32_t part_id)
{
    if (section->type != ST_HEAP_4M) {
        Pool *pool = (Pool *)section_find_collection(section, p);
        //pool->num_used--;
        pool_free_block(pool, p);
        allocator_set_cached_pool(a, pool);
    } else {
        Heap *heap = (Heap *)((uint8_t *)section + sizeof(Section));
        uint32_t heapIdx = area_get_type((Area *)section);
        heap_free(heap, p, false);
        // if the free pools list is empty.
        if (!heap_is_connected(heap)) {
            // reconnect
            PartitionAllocator *_part_alloc = partition_allocators[part_id];
            Queue *queue = &_part_alloc->heaps[heapIdx];
            if (queue->head != heap && queue->tail != heap) {
                list_enqueue(queue, heap);
            }
        }
    }
    if (!section_is_connected(section)) {
        PartitionAllocator *_part_alloc = partition_allocators[part_id];
        Queue *sections = _part_alloc->sections;
        if (sections->head != section && sections->tail != section) {
            list_enqueue(sections, section);
        }
    }
}

void allocator_free_from_container(Allocator *a, void *p, const size_t area_size)
{
    Area *area = (Area *)((uintptr_t)p & ~(area_size - 1));
    const uint32_t part_id = area_get_id(area);
    if (a->idx == partition_owners[part_id]) {
        switch (area_get_container_type(area)) {
        case CT_SECTION: {
            Section *section = (Section *)((uintptr_t)p & ~(SECTION_SIZE - 1));
            allocator_free_from_section(a, p, section, part_id);
            break;
        }
        case CT_HEAP: {
            Heap *heap = (Heap *)((uintptr_t)area + sizeof(Area));
            heap_free(heap, p, true);
            // if the pool is disconnected from the queue
            if (!heap_is_connected(heap)) {
                PartitionAllocator *_part_alloc = partition_allocators[part_id];
                Queue *queue = &_part_alloc->heaps[(area_get_type(area))];
                // reconnect
                if (queue->head != heap && queue->tail != heap) {
                    list_enqueue(queue, heap);
                }
            }
            break;
        }
        default: {
            PartitionAllocator *_part_alloc = partition_allocators[part_id];
            partition_allocator_free_area(_part_alloc, area);
            break;
        }
        }
    } else {
        allocator_thread_free(a, p, part_id);
    }
}

Section *allocator_get_free_section(Allocator *a, const size_t s, SectionType st)
{
    Section *free_section = (Section *)a->part_alloc->sections->head;

    // find free section.
    while (free_section != NULL) {
        Section *next = free_section->next;
        if (free_section->type == st) {
            if (!section_is_full(free_section)) {
                break;
            } else {
                list_remove(a->part_alloc->sections, free_section);
            }
        }
        free_section = next;
    }

    if (free_section == NULL) {
        Section *new_section = partition_allocator_alloc_section(a->part_alloc, s);
        if (new_section == NULL) {
            return NULL;
        }
        new_section->type = st;

        new_section->next = NULL;
        new_section->prev = NULL;
        list_enqueue(a->part_alloc->sections, new_section);

        free_section = new_section;
    }
    return free_section;
}

void *allocator_alloc_from_heap(Allocator *a, const size_t s)
{
    const uint32_t heap_sizes[] = {1 << HT_4M, 1 << HT_32M, 1 << HT_64M, 1 << HT_128M, 1 << HT_256M};
    const uint32_t heap_size_cls = size_to_heap(s);
    Queue *queue = &a->part_alloc->heaps[heap_size_cls];
    Heap *start = (Heap *)queue->head;
    while (start != NULL) {
        Heap *next = start->next;
        if (heap_has_room(start, s)) {
            return heap_get_block(start, (uint32_t)s);
        } else {
            list_remove(queue, start);
        }
        start = next;
    }

    if (heap_size_cls == 0) {
        Section *new_section = allocator_get_free_section(a, s, ST_HEAP_4M);
        if (new_section != NULL) {
            const unsigned int coll_idx = section_reserve_next(new_section);
            int32_t psize = (1 << size_clss_to_exponent[ST_HEAP_4M]);
            start = (Heap *)section_get_collection(new_section, coll_idx, psize);
            heap_init(start, coll_idx, heap_sizes[0]);
            section_claim_idx(new_section, coll_idx);
            list_enqueue(queue, start);
            return heap_get_block(start, (uint32_t)s);
        }
    }

    AreaType at = get_area_type_for_heap(s);
    Area *new_area = partition_allocator_get_free_area(a->part_alloc, s, at);
    if (new_area == NULL) {
        return NULL;
    }

    Partition* partition = &a->part_alloc->area[at];
    uint32_t idx = partition_allocator_get_area_idx_from_queue(a->part_alloc, new_area, partition);
    uint32_t range = get_range(idx, partition->range_mask);
    uint64_t area_size = area_get_size(new_area)*range;
    area_set_container_type(new_area, CT_HEAP);
    area_reserve_all(new_area);
    start = (Heap *)((uintptr_t)new_area + sizeof(Area));
    heap_init(start, 0, area_size);

    list_enqueue(queue, start);
    return heap_get_block(start, (uint32_t)s);
}

void *allocator_alloc_slab(Allocator *a, const size_t s)
{
    const size_t totalSize = sizeof(Area) + s;
    Area *area = partition_allocator_get_free_area(a->part_alloc, totalSize, AT_FIXED_256);
    if (area == NULL) {
        return NULL;
    }
    area_reserve_all(area);
    area_set_container_type(area, CT_SLAB);
    return (void *)((uintptr_t)area + sizeof(Area));
}

static inline Pool *allocator_alloc_pool(Allocator *a, const uint32_t idx, const uint32_t s)
{
    Section *sfree_section = allocator_get_free_section(a, s, get_pool_size_class(s));
    if (sfree_section == NULL) {
        return NULL;
    }

    const unsigned int coll_idx = section_reserve_next(sfree_section);
    int32_t psize = (1 << size_clss_to_exponent[sfree_section->type]);
    Pool *p = (Pool *)section_get_collection(sfree_section, coll_idx, psize);
    pool_init(p, coll_idx, idx, psize, s);
    section_claim_idx(sfree_section, coll_idx);
    return p;
}

void *allocator_alloc_from_pool(Allocator *a, const size_t s)
{
    const int32_t pool_idx = size_to_pool(s);
    Queue *queue = &a->part_alloc->pools[pool_idx];
    Pool *start = queue->head;
    void*res = NULL;
    if (start != NULL) {
        // there are no empty pools in the queue.
        list_remove(queue, start);
        res = pool_aquire_block(start);
    }
    else
    {
        start = allocator_alloc_pool(a, pool_idx, (uint32_t)s);
        if (start == NULL) {
            return NULL;
        }
        res = pool_get_free_block(start);
    }
    allocator_set_cached_pool(a, start);
    return res;
}

void free_extended_part(size_t pid, void *p)
{
    if (((uintptr_t)p & (os_page_size - 1)) != 0) {
        return;
    }

    uintptr_t header = (uintptr_t)p - CACHE_LINE;
    uint64_t size = 0;
    uint64_t addr = *(uint64_t *)header;
    if ((void *)addr == p) {
        size = *(uint64_t *)(header + sizeof(uint64_t));
        free_memory((void *)header, size);
    }
}

static inline __attribute__((always_inline)) void _allocator_free(Allocator *a, void *p)
{
    if(a->cache.header != 0)
    {
        if(pool_free_block((Pool*)a->cache.header, p))
        {
            //a->cache.freed_blocks++;
            return;
        }
        allocator_release_cache(a);
    }
    
    
    int8_t pid = partition_from_addr((uintptr_t)p);
    if (pid >= 0 && pid < NUM_AREA_PARTITIONS) {
        allocator_free_from_container(a, p, area_size_from_partition_id(pid));
    } else {
        free_extended_part(pid, p);
    }
}



void allocator_thread_dequeue_all(Allocator *a, message_queue *queue)
{
    message *back = (message *)atomic_load_explicit(&queue->tail, memory_order_relaxed);
    message *curr = (message *)(uintptr_t)queue->head;
    // loop between start and end addres
    while (curr != back) {
        message *next = (message *)atomic_load_explicit(&curr->next, memory_order_acquire);
        if (next == NULL)
            break;
        _allocator_free(a, curr);
        curr = next;
    }
    queue->head = (uintptr_t)curr;
}

static inline void allocator_flush_thread_free_queue(Allocator *a)
{
    message_queue *q = a->part_alloc->thread_free_queue;
    if (q->head != q->tail) {
        allocator_thread_dequeue_all(a, q);
    }
}

static inline void *allocator_try_malloc(Allocator *a, size_t as)
{
    if (as <= LARGE_OBJECT_SIZE) {
        return allocator_alloc_from_pool(a, as);
    } else if (as <= AREA_SIZE_LARGE) {
        // allocate form the large page
        return allocator_alloc_from_heap(a, as);
    } else {
        return allocator_alloc_slab(a, as);
    }
}

static inline int64_t size_to_arena(size_t s)
{
    if(s < (1 << 10))
    {
        return 0;
    }
    static const int32_t lookup[] = {0,1,2,3,4,5,6,6,
        6,6,6,6,0,1,2,3,
        4,5,6,6,6,6,6,6
    };
    int32_t lz = __builtin_clzll(s);
    if(lz < 31)
    {
        int64_t t = (64 - 8 - lz);
        return lookup[t];
    }
    return -1;
}

static inline void *allocator_malloc_fallback(Allocator *a, size_t as)
{
    void* ptr = NULL;
    // reset caching structs
    a->cache.header = 0;
    // try again by fetching a new partition set to use
    const int8_t new_partition_set_idx = reserve_any_partition_set_for((int32_t)a->idx);
    if (new_partition_set_idx != -1) {
        // flush our threaded pools.
        allocator_flush_thread_free(a);
        // move our default partition allocator to the new slot and try again.
        PartitionAllocator *part_alloc = partition_allocator_aquire(new_partition_set_idx);
        list_enqueue(&a->partition_allocators, part_alloc);
        a->part_alloc = part_alloc;
        ptr = allocator_try_malloc(a, as);
    }
    // hopefully this is not NULL
    return ptr;
}

void *allocator_malloc(Allocator *a, size_t s)
{
    // do we have  cached pool to use of a fitting size?
    void *ptr = NULL;
    if (a->cache.header != 0) {
        ptr = allocator_malloc_from_cache(a, s);
        if(ptr != NULL)
        {
            return ptr;
        }
    }
    
    a->prev_size = (uint32_t)s;
    
    const size_t as = ALIGN(s);
    // if we have some memory waiting in our thread free queue.
    // lets make it available.
    allocator_flush_thread_free_queue(a);
    // attempt to get the memory requested
    ptr = allocator_try_malloc(a, as);
    if(ptr == NULL)
    {
        return allocator_malloc_fallback(a, as);
    }
    return ptr;
}

void *allocator_malloc_heap(Allocator *a, size_t s)
{
    const size_t size = ALIGN4(s);
    a->cache.header = 0;
    allocator_flush_thread_free_queue(a);
    if (s <= AREA_SIZE_LARGE) {
        // allocate form the large page
        return allocator_alloc_from_heap(a, size);
    } else {
        return allocator_alloc_slab(a, size);
    }
}

bool allocator_release_local_areas(Allocator *a)
{
    allocator_release_cache(a);
    bool result = false;
    PartitionAllocator *palloc = a->partition_allocators.head;
    while (palloc != NULL) {
        PartitionAllocator *next = palloc->next;
        allocator_thread_dequeue_all(a, palloc->thread_free_queue);
        bool was_released = partition_allocator_release_local_areas(palloc);
        if (was_released) {
            palloc->sections->head = NULL;
            palloc->sections->tail = NULL;

            for (int j = 0; j < HEAP_TYPE_COUNT; j++) {
                palloc->heaps[j].head = NULL;
                palloc->heaps[j].tail = NULL;
            }

            for (int j = 0; j < POOL_BIN_COUNT; j++) {
                if (palloc->pools[j].head != NULL || palloc->pools[j].tail != NULL) {
                    palloc->pools[j].head = NULL;
                    palloc->pools[j].tail = NULL;
                }
            }
            if (a->idx != palloc->idx) {
                release_partition_set((int32_t)palloc->idx);
                list_remove(&a->partition_allocators, palloc);
            }
        }

        result |= !was_released;
        palloc = next;
    }
    a->part_alloc = partition_allocators[a->idx];
    return !result;
}

void allocator_free(Allocator *a, void *p)
{
    _allocator_free(a, p);
}

void allocator_free_th(Allocator *a, void *p)
{
    if (a->idx == -1) {
        // free is being called before anythiing has been allocated for this thread.
        // the address is either bogus or belong to some other thread.
        if (partition_from_addr((uintptr_t)p) >= 0) {
            // it is within our address ranges.
            size_t area_size = area_size_from_addr((uintptr_t)p);
            Area *area = (Area *)((uintptr_t)p & ~(area_size - 1));
            const uint32_t part_id = area_get_id(area);
            if (part_id < 1024) {
                // well we are able to read the contents of this memory address.
                // get the idx of the area within its partition.
                if (reserve_partition_set(part_id, 1024)) {
                    // no one had this partition reserved.
                    //
                    Allocator *temp = allocator_list[part_id];
                    _allocator_free(temp, p);
                    // try and release whatever is in it.
                    allocator_release_local_areas(temp);
                    release_partition_set(part_id);
                } else {
                    // someone has this partition reserved, so we just pluck the
                    // address to its thread free queue.
                    PartitionAllocator *_part_alloc = partition_allocators[part_id];
                    // if there is a partition allocator that owns this, we can just
                    // push it on to its queue.
                    message *new_free = (message *)p;
                    new_free->next = (uintptr_t)0;
                    allocator_thread_enqueue(_part_alloc->thread_free_queue, new_free, new_free);
                }
            }
        }
    } else {
        allocator_free(a, p);
    }
}

size_t allocator_get_allocation_size(Allocator *a, void *p)
{
    int8_t pid = partition_from_addr((uintptr_t)p);
    if (pid >= 0 && pid < NUM_AREA_PARTITIONS) {
        size_t area_size = area_size_from_partition_id(pid);
        Area *area = (Area *)((uintptr_t)p & ~(area_size - 1));
        switch (area_get_container_type(area)) {
        case CT_SECTION: {
            Section *section = (Section *)((uintptr_t)p & ~(SECTION_SIZE - 1));
            if (section->type != ST_HEAP_4M) {
                Pool *pool = (Pool *)section_find_collection(section, p);
                return pool->block_size;
            } else {
                Heap *heap = (Heap *)((uint8_t *)section + sizeof(Section));
                return heap_get_block_size(heap, p);
            }
        }
        case CT_HEAP: {
            Heap *heap = (Heap *)((uintptr_t)area + sizeof(Area));
            return heap_get_block_size(heap, p);
        }
        default: {
            AreaType at = area_get_type(area);
            Partition* partition = &a->part_alloc->area[at];
            uint32_t idx = partition_allocator_get_area_idx_from_queue(a->part_alloc, area, partition);
            uint32_t range = get_range(idx, partition->range_mask);
            return area_get_size(area)*range;
        }
        }
    } else {
        uintptr_t header = (uintptr_t)p - CACHE_LINE;
        uint64_t size = 0;
        uint64_t addr = *(uint64_t *)header;
        if ((void *)addr == p) {
            size = *(uint64_t *)(header + sizeof(uint64_t));
            return size;
        }
    }
    return 0;
}
