

#include "allocator.h"
#include "partition_allocator.h"
#include "pool.h"
#include "heap.h"
#include "os.h"
#include "arena.h"

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

void allocator_set_cached_pool(Allocator *a, Pool *p, bool alloc)
{
    a->c_cache.header = (uintptr_t)p;
    a->c_cache.end = sizeof(Pool) + (p->num_available * p->block_size);
    a->c_cache.block_size = p->block_size;
    a->c_cache.cache_type = CACHE_POOL;
    a->c_cache.rem_blocks = 0;
    if(alloc)
    {
        a->c_cache.rem_blocks = p->num_available - p->num_committed;
        if(a->c_cache.rem_blocks)
        {
            // reserve all memory from the pool
            p->num_used = p->num_available;
            p->num_committed = p->num_available;
        }
    }
    
}

void allocator_set_cached_arena(Allocator *a, Arena *p)
{
    uintptr_t start_addr = (uintptr_t)p;
    if (start_addr == a->c_cache.header) {
        return;
    }
    
    a->c_cache.header = start_addr;
    a->c_cache.cache_type = CACHE_ARENA;
}

static inline void allocator_release_cached_pool(Allocator *a)
{
    Pool* p = (Pool*)a->c_cache.header;
    p->num_used -= a->c_cache.rem_blocks;
    p->num_committed -= a->c_cache.rem_blocks;
    if (!pool_is_empty(p)) {
        Queue *queue = &a->part_alloc->pools[p->block_idx];
        if(!pool_is_connected(p) && queue->head != p)
        {
            list_enqueue(queue, p);
        }
        else
        {
            list_remove(queue, p);
            list_enqueue(queue, p);
        }
        if(pool_is_full(p))
        {
            pool_post_free(p);
        }
    }
    //
    a->c_cache.rem_blocks = 0;
    a->c_cache.header = 0;
}

static inline void allocator_release_cache(Allocator *a)
{
    if (a->c_cache.header) {
        if(a->c_cache.cache_type == CACHE_POOL)
        {
            allocator_release_cached_pool(a);
        }
    }
    a->c_cache.header = 0;
}

static inline void *allocator_malloc_from_cache(Allocator *a, size_t s)
{
    if(a->prev_size != s)
    {
        allocator_release_cached_pool(a);
        return NULL;
    }
    if(a->c_cache.rem_blocks)
    {
        return (void*)(uintptr_t)(a->c_cache.header + a->c_cache.end) - (a->c_cache.rem_blocks-- * a->c_cache.block_size);
    }
    else
    {
        if(a->c_cache.cache_type == CACHE_POOL)
        {
            Pool* p = (Pool*)a->c_cache.header;
            if(pool_is_full(p))
            {
                allocator_set_cached_pool(a, p, true);
                return pool_aquire_block(p);
            }
            else
            {
                if(!pool_is_empty(p))
                {
                    if(p->num_committed < p->num_available)
                    {
                        allocator_set_cached_pool(a, p, true);
                        return (void*)(uintptr_t)(a->c_cache.header + a->c_cache.end) - (a->c_cache.rem_blocks-- * a->c_cache.block_size);
                    }
                    else
                    {
                        return pool_aquire_block(p);
                    }
                }
            }
            
            allocator_release_cached_pool(a);
        }
    }
    return NULL;
}

void allocator_thread_enqueue(AtomicQueue *queue, AtomicMessage *first, AtomicMessage *last)
{
    atomic_store_explicit(&last->next, (uintptr_t)NULL,
                          memory_order_release); // last.next = null
    AtomicMessage *prev = (AtomicMessage *)atomic_exchange_explicit(&queue->tail, (uintptr_t)last,
                                                        memory_order_release); // swap back and last
    atomic_store_explicit(&prev->next, (uintptr_t)first,
                          memory_order_release); // prev.next = first
}

static inline void allocator_flush_thread_free(Allocator *a)
{
    if (a->thread_free_part_alloc != NULL) {
        // get the first and last item of the tf queue
        AtomicMessage *lm = partition_allocator_get_last_message(a->part_alloc);
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
        pool_free_block(pool, p);
        //allocator_set_cached_pool(a, pool, false);
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
    if(area_is_full(area))
    {
        AreaType at = area_get_type(area);
        Partition* partition = &a->part_alloc->area[at];
        int32_t aidx = partition_allocator_get_area_idx_from_queue(a->part_alloc, area, partition);
        partition->full_mask &= ~(1ULL << aidx);
    }
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
                Area *area = area_from_addr((uintptr_t)free_section);
                if(area_is_full(area))
                {
                    AreaType at = area_get_type(area);
                    Partition* partition = &a->part_alloc->area[at];
                    int32_t aidx = partition_allocator_get_area_idx_from_queue(a->part_alloc, area, partition);
                    partition->full_mask |= (1ULL << aidx);
                }
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
    int32_t area_idx = -1;
    Partition* partition = partition_allocator_get_free_area(a->part_alloc, s, at, &area_idx);
    Area* new_area = partition_allocator_area_at_idx(a->part_alloc, partition, area_idx);
    if (new_area == NULL) {
        return NULL;
    }
    
    if((partition->zero_mask & 1ULL << area_idx) == 0)
    {
        area_init(new_area, a->idx, at);
        partition->zero_mask |= (1ULL << area_idx);
    }
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
    int32_t area_idx = -1;
    Partition *partition = partition_allocator_get_free_area(a->part_alloc, totalSize, AT_FIXED_256, &area_idx);
    if (partition == NULL) {
        return NULL;
    }
    Area *area = partition_allocator_area_at_idx(a->part_alloc, partition, area_idx);
    if((partition->zero_mask & 1ULL << area_idx) == 0)
    {
        area_init(area, a->idx, AT_FIXED_256);
        partition->zero_mask |= (1ULL << area_idx);
    }
    area_reserve_all(area);
    area_set_container_type(area, CT_SLAB);
    return (void *)((uintptr_t)area + sizeof(Area));
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

void * allocator_alloc_arena(Allocator* alloc, size_t s)
{
    int64_t partition_idx = size_to_arena(s);
    if (partition_idx < 0) {
        return NULL;
    }
    int32_t area_idx = -1;
    Partition* partition = partition_allocator_get_free_area(alloc->part_alloc, s, (AreaType)partition_idx, &area_idx);
    if(partition == NULL)
    {
        return NULL;
    }
    void* arena = partition_allocator_area_at_idx(alloc->part_alloc, partition, area_idx);
    Arena *header = (Arena *)((uintptr_t)arena + sizeof(Arena_L2));
    header->partition_id = (uint32_t)alloc->part_alloc->idx;
    AreaType at = partition_allocator_get_partition_idx(alloc->part_alloc, partition);
    if((partition->zero_mask & 1ULL << area_idx) == 0)
    {
        arena_init((uintptr_t)arena, area_idx, area_type_to_exponent[at]);
        partition->zero_mask |= (1ULL << area_idx);
    }
    
    if (arena == NULL) {
        return NULL;
    }
    return arena;
}

void *allocator_alloc_from_arena(Allocator *a, const size_t s, Arena** source_arena)
{
    *source_arena = allocator_alloc_arena(a, s);
    return NULL;
}

static inline Pool *allocator_alloc_pool(Allocator *a, const uint32_t idx, const uint32_t s)
{
#ifdef ARENA_PATH
    Arena* arena = NULL;
    void *section = allocator_alloc_from_arena(a, get_pool_size_class(s), &arena);
    if (section == NULL) {
        return NULL;
    }

    // what is the index of the section in the arena.
    // what is the size of the section.
    // reserve size of pool
    size_t arena_size = 1ULL << arena->container_exponent;
    int32_t pool_size = 1 << (arena->container_exponent - 6);
    uintptr_t base_addr = ((uintptr_t)section & ~(arena_size - 1));
    unsigned int coll_idx = delta_exp_to_idx((uintptr_t)section, base_addr, arena->container_exponent - 6);
    pool_init((Pool*)section, coll_idx, idx, pool_size, s);
    return (Pool*)section;
#else
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
#endif
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
    allocator_set_cached_pool(a, start, true);
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

static inline void _allocator_free_ex(Allocator *a, void *p)
{
    int8_t pid = partition_from_addr((uintptr_t)p);
    if (pid >= 0 && pid < NUM_AREA_PARTITIONS) {
        allocator_free_from_container(a, p, area_size_from_partition_id(pid));
    } else {
        free_extended_part(pid, p);
    }
}

static inline __attribute__((always_inline)) void _allocator_free(Allocator *a, void *p)
{
    
    if(a->c_deferred.end == 0)
    {
        deferred_cache_init(a, p);
    }
    else
    {
        //
        // is address within the current deferred 64th.
        if((uintptr_t)p < a->c_deferred.start || (uintptr_t)p >= (a->c_deferred.end))
        {
            deferred_cache_release(a, p);
        }
        else
        {
            
            if(a->c_cache.rem_blocks != 0)
            {
                allocator_release_cached_pool(a);
            }
            deferred_cache_add(&a->c_deferred, p);
        }
    }
    /*
    if(a->c_cache.header == 0)
    {
        _allocator_free_ex(a, p);
    }
    else
    {
        if((uintptr_t)p < a->c_cache.header)
        {
            allocator_release_cache(a);
            _allocator_free_ex(a, p);
        }
        else
        {
            if((uintptr_t)p >= (a->c_cache.header + a->c_cache.end))
            {
                allocator_release_cache(a);
                _allocator_free_ex(a, p);
            }
            else
            {
                
                Pool* pool = (Pool*)a->c_cache.header;
                if(a->c_cache.rem_blocks != 0)
                {
                    pool->num_used -= a->c_cache.rem_blocks;
                    pool->num_committed -= a->c_cache.rem_blocks;
                    a->c_cache.rem_blocks = 0;
                }
                pool_free_block(pool, p);
            }
        }
    }
     */
}



void allocator_thread_dequeue_all(Allocator *a, AtomicQueue *queue)
{
    AtomicMessage *back = (AtomicMessage *)atomic_load_explicit(&queue->tail, memory_order_relaxed);
    AtomicMessage *curr = (AtomicMessage *)(uintptr_t)queue->head;
    // loop between start and end addres
    while (curr != back) {
        AtomicMessage *next = (AtomicMessage *)atomic_load_explicit(&curr->next, memory_order_acquire);
        if (next == NULL)
            break;
        _allocator_free(a, curr);
        curr = next;
    }
    queue->head = (uintptr_t)curr;
}

static inline void *allocator_try_malloc(Allocator *a, size_t as)
{
#ifdef ARENA_PATH
    if (as <= (1 << 10)) {
        return allocator_alloc_from_pool(a, as);
    }
    else
    {
        // allocate from arena.
        Arena* arena = NULL;
        void* res = allocator_alloc_from_arena(a, as, &arena);
        return res;
    }
#else
    if (as <= LARGE_OBJECT_SIZE) {
        return allocator_alloc_from_pool(a, as);
    } else if (as <= AREA_SIZE_LARGE) {
        // allocate form the large page
        return allocator_alloc_from_heap(a, as);
    } else {
        return allocator_alloc_slab(a, as);
    }
#endif
}



static inline void *allocator_malloc_fallback(Allocator *a, size_t as)
{
    void* ptr = NULL;
    // reset caching structs
    a->c_cache.header = 0;
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
    if (a->c_cache.header != 0) {
        ptr = allocator_malloc_from_cache(a, s);
        if(ptr != NULL)
        {
            return ptr;
        }
    }
    
    a->prev_size = (uint32_t)s;
    
    const size_t as = ALIGN(s);
    
    deferred_cache_release(a, NULL);
    // if we have some memory waiting in our thread free queue.
    // lets make it available.
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
    allocator_release_cache(a);
    deferred_cache_release(a, NULL);
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
    deferred_cache_release(a, NULL);
    bool result = false;
    PartitionAllocator *palloc = a->partition_allocators.head;
    while (palloc != NULL) {
        PartitionAllocator *next = palloc->next;
        allocator_thread_dequeue_all(a, palloc->thread_free_queue);
        partition_allocator_release_deferred(palloc);
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
                    AtomicMessage *new_free = (AtomicMessage *)p;
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
