

#include "allocator.h"
#include "partition_allocator.h"
#include "pool.h"
#include "os.h"
#include "arena.h"


extern PartitionAllocator *partition_allocator;
extern uintptr_t main_thread_id;
extern Allocator *main_instance;


Allocator *allocator_aquire(void)
{
    uintptr_t thr_mem = (uintptr_t)alloc_memory((void*)BASE_OS_ALLOC_ADDRESS, os_page_size, true);
    Allocator *alloc = (Allocator *)thr_mem;
    alloc->prev_size = -1;
    thr_mem = ALIGN_CACHE(thr_mem + sizeof(Allocator));
    
    thr_mem = ALIGN_CACHE(thr_mem + sizeof(mutex_t));
    // next come the partition allocator structs.
    Queue *pool_queue = (Queue *)thr_mem;
    thr_mem = ALIGN_CACHE(thr_mem + sizeof(Queue) * POOL_BIN_COUNT);
    Queue *arena_queue = (Queue *)thr_mem;
    thr_mem = (uintptr_t)alloc + DEFAULT_OS_PAGE_SIZE;
    thr_mem -= ALIGN_CACHE(sizeof(PartitionAllocator));
    
    alloc->pools = pool_queue;
    alloc->arenas = arena_queue;
    return alloc;
}

void* allocator_slot_alloc_null(Allocator*a,  const size_t as)
{
    return NULL;
}

void* allocator_slot_alloc_pool(Allocator*a,  const size_t as)
{
    return pool_aquire_block((Pool*)(a->c_slot.header & ~0x3), a);
}

static inline void* _allocator_slot_alloc(alloc_slot*a)
{
    void* res = (void*)(uintptr_t)((a->header & ~0x3) + a->offset);
    a->offset += a->req_size;
    return res;
}

void* allocator_slot_alloc(Allocator*a,  const size_t as)
{
    return _allocator_slot_alloc(&a->c_slot);
}


void* allocator_slot_area_alloc(Allocator*a,  const size_t as)
{
    return (void *)((uintptr_t)(a->c_slot.header& ~0x7));
}

internal_alloc allocator_set_area_slot(Allocator *a, uintptr_t p, int32_t start_idx)
{
    if(p == 0UL)
    {
        return allocator_slot_alloc_null;
    }
    a->c_slot.header = (uintptr_t)p | SLOT_REGION;
    a->c_slot.offset = 0;
    a->c_slot.end = 0;
    
    a->c_slot.block_size = 0;
    a->c_slot.alignment = 0;
    a->c_slot.req_size = 0;

    return allocator_slot_area_alloc;
}

internal_alloc allocator_set_pool_slot(Allocator *a, Pool *p)
{
    if(p == NULL)
    {
        return allocator_slot_alloc_null;
    }
    a->c_slot.header = (uintptr_t)p | SLOT_POOL;
    a->c_slot.block_size = (int32_t)p->block_size;
    a->c_slot.alignment = p->alignment;
    a->c_slot.req_size = (int32_t)p->block_size;
    a->c_slot.pheader = 0;
    a->c_slot.pend = 0;
    int32_t rem_blocks = p->num_available - p->num_committed;
    pool_post_reserved(p, a);
    
    if(rem_blocks > 0)
    {
        a->c_slot.offset = (int32_t)((uintptr_t)pool_base_address(p) - (uintptr_t)p);
        a->c_slot.start = a->c_slot.offset;
        a->c_slot.end = (int32_t)(((uintptr_t)pool_base_address(p) - (uintptr_t)p) + (p->num_available * p->block_size));
        // reserve all memory from the pool
        p->num_used = p->num_available;
        p->num_committed = p->num_available;
        return allocator_slot_alloc;
    }
    a->c_slot.offset = 0;
    a->c_slot.end = 0;
    a->c_slot.start = 0;
    
    return allocator_slot_alloc_pool;
}

internal_alloc allocator_set_counter_slot(Allocator *a, void *p, uint32_t slot_size, uintptr_t pheader, int32_t pend)
{
    a->c_slot.header = (uintptr_t)p | SLOT_COUNTER;
    a->c_slot.offset = sizeof(uintptr_t);
    a->c_slot.start = a->c_slot.offset;
    a->c_slot.end = slot_size;
    a->c_slot.block_size = slot_size;
    a->c_slot.counter = 0;
    a->c_slot.req_size = 1;
    a->c_slot.alignment = sizeof(uintptr_t);
    
    // set up our parent links back
    a->c_slot.pheader = pheader;
    a->c_slot.pend = pend;
    
    return allocator_slot_alloc;
}

internal_alloc allocator_set_arena_slot(Allocator *a, void *p, uint32_t block_size, uint32_t exp, uint32_t block_count, int32_t start_idx)
{
    uintptr_t start_addr = (uintptr_t)p;
    if (start_addr == a->c_slot.header || p == NULL) {
        return allocator_slot_alloc_null;
    }
    Arena* arena = (Arena*)p;
    a->c_slot.header = (uintptr_t)p| SLOT_ARENA;
    
    a->c_slot.block_size = block_size;
    a->c_slot.alignment = block_size << exp;
    a->c_slot.req_size = block_size*block_count;
    
    //int32_t _max_zeros = num_consecutive_zeros(arena->allocations | ((1ULL << (exp + 1)) - 1));
    //int32_t _offset = find_first_nzeros(arena->allocations, _max_zeros, exp);
    
    uintptr_t end_mask = arena->allocations & ~((1ULL << start_idx) - 1);
    int32_t end_idx = end_mask == 0? 64 :__builtin_ctzll(end_mask);
    int32_t max_zeros = end_idx - start_idx;
    
    // from the start idx... count the number of zeros
    a->c_slot.start = start_idx*a->c_slot.block_size;
    a->c_slot.offset = a->c_slot.start;
    a->c_slot.end = a->c_slot.offset + max_zeros*a->c_slot.block_size;
    
    return allocator_slot_alloc;
}

static inline void allocator_release_pool_slot(Allocator *a)
{
    Pool* p = (Pool*)(a->c_slot.header & ~0x3);
    Queue *queue = &a->pools[p->block_idx];
    uint32_t rem_blocks = 0;
    if(a->c_slot.offset < a->c_slot.end)
    {
        rem_blocks = (a->c_slot.end - a->c_slot.offset)/a->c_slot.block_size;
        p->num_used -= rem_blocks;
        p->num_committed -= rem_blocks;
        if (!pool_is_empty(p)) {
            
            if(pool_is_full(p))
            {
                pool_post_free(p, a);
            }
        }
        if(!pool_is_connected(p) && queue->head != p)
        {
            list_enqueue(queue, p);
        }
    }
    else
    {
        if(pool_is_connected(p) || queue->head == p)
        {
            list_remove(queue, p);
        }
    }

    //
    a->c_slot.offset = 0;
    a->c_slot.end = 0;
    a->c_slot.header = 0;
    a->c_slot.start = 0;
}

static inline void allocator_release_arena_slot(Allocator *a)
{
    Arena* p = (Arena*)(a->c_slot.header & ~0x3);
    
    size_t count = (a->c_slot.offset - a->c_slot.start)/ a->c_slot.req_size;
    size_t cidx = a->c_slot.offset / a->c_slot.block_size;
    uint32_t block_count = a->c_slot.req_size/a->c_slot.block_size;
    uint64_t mask = (1ULL << (count*block_count) ) - 1;
    size_t sidx = cidx - (count*block_count);
    p->allocations |= (mask << sidx);
    
    if(block_count > 1)
    {
        // we need to assign our ranges.
        for(int32_t i = 0; i < count; i++)
        {
            size_t offset = sidx+(i*block_count);
            p->ranges |= apply_range(block_count, (uint32_t)offset);
        }
    }
    
    if(p->allocations == UINT64_MAX)
    {
        // the arenas that have free items in them are in the front.
        // so anything that gets free memory will be promoted to the front of
        // of the list.
        // if the first item in the list is empty, you can assume that all
        // the items in the list are empty.
        Queue *queue = &a->arenas[p->partition_id];
        if(arena_is_connected(p) || queue->head == p)
        {
            list_remove(queue,p);
        }
    }
    a->c_slot.offset = 0;
    a->c_slot.end = 0;
    a->c_slot.header = 0;
    a->c_slot.start = 0;
}

static inline void allocator_release_counter_slot(Allocator *a)
{
    int32_t* counter = (int32_t*)(a->c_slot.header & ~0x3);
    uint32_t count = (a->c_slot.offset - a->c_slot.start)/a->c_slot.req_size;
    a->c_slot.counter += count;
    *counter = a->c_slot.counter;
    a->c_slot.offset = 0;
    a->c_slot.end = 0;
    a->c_slot.header = 0;
    a->c_slot.start = 0;
    // reinstate the previous slot if we had been dealt out of a parent container
    if(a->c_slot.pheader != 0)
    {
        slot_type st = a->c_slot.pheader & 0x3;
        a->c_slot.start = (int32_t)((uintptr_t)counter + a->c_slot.block_size);
        a->c_slot.offset = a->c_slot.start;
        a->c_slot.end = a->c_slot.pend;
        a->c_slot.header = a->c_slot.pheader;
        a->c_slot.req_size = a->c_slot.block_size;
        if(st == SLOT_POOL)
        {
            return allocator_release_pool_slot(a);
        }
        else
        {
            return allocator_release_arena_slot(a);
        }
    }
}

static inline void allocator_release_slot(Allocator *a)
{
    
    if (a->c_slot.header) {
        slot_type st = a->c_slot.header & 0x3;
        switch(st)
        {
            case SLOT_POOL:
                return allocator_release_pool_slot(a);
            case SLOT_COUNTER:
                return allocator_release_counter_slot(a);
            case SLOT_ARENA:
                return allocator_release_arena_slot(a);
            default:
                break;
        }
    }
}


internal_alloc allocator_load_area_slot(Allocator *a, const size_t s)
{
    int32_t region_idx = -1;
    void* region = partition_allocator_get_free_region(partition_allocator, AT_FIXED_256, &region_idx);
    if(region)
    {
        return allocator_set_area_slot(a, (uintptr_t)region, region_idx);
    }
    else
    {
        return allocator_slot_alloc_null;
    }
    
}

void * allocator_alloc_arena(Allocator* alloc, int32_t partition_idx, int32_t* region_idx, bool zero)
{
    if (partition_idx < 0) {
        return NULL;
    }
    
    return partition_allocator_get_free_region(partition_allocator, partition_idx, region_idx);
}


internal_alloc allocator_malloc_pool_find_fit(Allocator* alloc, const uint32_t pc)
{
    // check if there are any pools available.
    Queue *queue = &alloc->pools[pc];
    Heap* start = queue->head;
    
    while(start != NULL)
    {
        if(pool_is_full((Pool*)start))
        {
            pool_set_full((Pool*)start, alloc);
            return allocator_set_pool_slot(alloc, (Pool *)start);
        }
        else if(!pool_is_empty((Pool*)start))
        {
            return allocator_set_pool_slot(alloc, (Pool *)start);
        }
        else if(pool_is_empty((Pool*)start))
        {
            Heap* next = start->next;
            list_remove(queue, start);
            start = next;
        }
    }
    return allocator_slot_alloc_null;
}

static inline uintptr_t allocator_get_arena_blocks(Allocator* alloc, int32_t arena_idx, int32_t min_free_blocks, uint8_t exp, bool zero, int32_t* midx, size_t* block_size)
{
    Queue* aqueue = &alloc->arenas[arena_idx];


    Heap* start = NULL;
    *midx = 0;
    if(exp == 0)
    {
        start = aqueue->head;
        int32_t c = 0;
        while(start != NULL)
        {
            Heap* next = start->next;
            if(((Arena*)start)->allocations != UINT64_MAX)
            {
                if((*midx = find_first_nzeros(((Arena*)start)->allocations, min_free_blocks, exp)) != -1)
                {
                    break;
                }
            }
            
            start = next;
            c++;
        }
    }

    
    if(start == NULL)
    {
        int32_t region_idx = 0;
        start = allocator_alloc_arena(alloc, arena_idx, &region_idx, zero);
        if(start != NULL)
        {
            *midx = 1<<exp;
            Arena* arena = (Arena*)start;
            arena->thread_id = alloc->thread_id;
            arena->allocations = 1;
            arena->partition_id = arena_idx;
            arena->idx = (region_idx << 1)| 0x1;
            
            if(!heap_is_connected(start) && aqueue->head != start)
            {
                list_enqueue(aqueue, start);
            }
            
        }
        else
        {
            return 0;
        }
    }
    int8_t pid = partition_id_from_addr((uintptr_t)start);
    size_t area_size = region_size_from_partition_id(pid);
    *block_size = area_size >> 6;
    
    return ((uintptr_t)start + (*midx * *block_size));
}

static inline internal_alloc allocator_malloc_leq_32k(Allocator* alloc, const size_t size, const size_t alignment, const bool zero)
{
    int32_t row_map[] = {0,1,2,3,4,5,5,5,5,5};
    uint8_t pc = size_to_pool(size);
    internal_alloc res = allocator_malloc_pool_find_fit(alloc, pc);
    if(res == allocator_slot_alloc_null)
    {
        int32_t row = pc/8;
        uint8_t arena_idx = row_map[row];
        
        int32_t midx = 0;
        size_t block_size = 0;
        uintptr_t start = allocator_get_arena_blocks(alloc, arena_idx, 1, 0, zero, &midx, &block_size);
        if(start != 0)
        {
            Pool* new_pool = (Pool*)start;
            pool_init(new_pool, midx, pc, (uint32_t)block_size);
            res = allocator_set_pool_slot(alloc, new_pool);
        }
    }
    
    return res;
}

static inline internal_alloc allocator_malloc_base(Allocator* alloc, size_t size, size_t alignment, const bool zero)
{
    internal_alloc res = allocator_slot_alloc_null;
    
    // alignment needs to be a power of 2.
    if(!POWER_OF_TWO(alignment))
    {
        return res;
    }
    
    if(size < alignment)
    {
        size = alignment;
    }
    else
    {
        // the size needs to be a multiple of the alignment.
        size_t rem = size % alignment;
        size += rem;
    }
    
    if(size <= (1 << 15)) // 8 <= n <= 32k
    {
        res = allocator_malloc_leq_32k(alloc, size, alignment, zero);
    }
    else
    {
        // these large allocation do not go through this path
        if(size < (1ULL << 22)) // 32k < n <= 4m
        {
            size_t offset = (63 - __builtin_clzll(size));
            size_t arena_idx = (offset - 16);
            int32_t midx = 0;
            size_t block_size = 0;
            uintptr_t start = allocator_get_arena_blocks(alloc, (int32_t)arena_idx, 1, 0, zero, &midx, &block_size);
            // we allocate a single block
            // we create an arena slot
            if(start != 0)
            {
                size_t area_size = block_size << 6;
                Arena *arena = (Arena *)((uintptr_t)start & ~(area_size - 1));
                res = allocator_set_arena_slot(alloc, (void*)arena, (uint32_t)block_size, 0, 1, midx);
            }
        }
        else
        {
            size_t rsize = size;
            if(!POWER_OF_TWO(size))
            {
                rsize = NEXT_POWER_OF_TWO(size);
            }
            if(rsize < (1ULL << 28)) // 4m < n <= 256m
            {
                const uint32_t arena_idx = 6;
                const arena_size_table *stable = arena_get_size_table_by_idx(arena_idx);
                
                size_t num_blocks = rsize >> stable->exponents[0];
                size_t delta = (alignment >> stable->exponents[0]);
                size_t exp = delta == 0? 0 : __builtin_ctzll(delta);
                int32_t midx = 0;
                size_t block_size = 0;
                uintptr_t start = allocator_get_arena_blocks(alloc, arena_idx, (uint32_t)num_blocks, exp, zero, &midx, &block_size);
                if(start != 0)
                {
                    size_t area_size = block_size << 6; // * 64
                    Arena *arena = (Arena *)((uintptr_t)start & ~(area_size - 1));
                    res = allocator_set_arena_slot(alloc, (void*)arena, (uint32_t)block_size, (uint32_t)exp, (uint32_t)num_blocks, midx);
                }
            }
            else if(rsize < (1ULL << 33)) // 256m < n < 8g
            {
                res = allocator_load_area_slot(alloc, rsize);  //
            }
            else
            {
                res = allocator_slot_alloc_null;
            }
        }
    }
    return res;
}


static inline __attribute__((always_inline)) void _allocator_free(Allocator *a, void *p)
{
    
    void* res = (void*)(uintptr_t)((a->c_slot.header & ~0x3) + a->c_slot.offset) - a->c_slot.req_size;
    if(res == p)
    {
        // we just returned the last memory allocated
        // so we just offset our slot 
        a->c_slot.offset -=a->c_slot.req_size;
        return;
    }
    if(a->c_deferred.end == 0)
    {
        allocator_release_slot(a);
        deferred_init(a, p);
    }
    else
    {
        // compute the index of the address relative to release block.
        // if address is negative we are beyond scope.
        // we need the size of the container.
        if((uintptr_t)p < a->c_deferred.start || (uintptr_t)p >= (a->c_deferred.end))
        {
            deferred_release(a, p);
        }
        else
        {
            deferred_add(&a->c_deferred, p);
        }
    }
}



static inline internal_alloc allocator_load_memory_slot(Allocator *a, size_t as, size_t alignment, bool zero)
{
    allocator_release_slot(a);
    deferred_release(a, NULL);
    a->prev_size = (uint64_t)as;
    return allocator_malloc_base(a, ALIGN(as), alignment, zero);
}



static void *allocator_malloc_slot_pool(Allocator *a, size_t s, void *res)
{
    // we make the assumption we are handing out memory from a slot
    // just pre-loading this address just in case. Is true for most cases.
    if(a->prev_size != s)
    {
        if(s > a->prev_size)
        {
            if(s <= a->c_slot.block_size)
            {
                //
                a->c_slot.offset += a->c_slot.req_size;
                if(a->c_slot.offset <= a->c_slot.end)
                {
                    a->prev_size = s;
                    return res;
                }
            }
        }
        else
        {
            Pool* p = (Pool*)(a->c_slot.header & ~0x3);
            if(p->block_idx == size_to_pool(s))
            {
                a->c_slot.offset += a->c_slot.req_size;
                if(a->c_slot.offset <= a->c_slot.end)
                {
                    a->prev_size = s;
                    return res;
                }
            }
        }
    }
    
    return allocator_slot_alloc_pool(a, s);
}

static void *allocator_malloc_slot_counter(Allocator *a, size_t s, void* res)
{
    // we make the assumption we are handing out memory from a slot
    // just pre-loading this address just in case. Is true for most cases.
    
    //
    uint32_t count = (a->c_slot.offset - a->c_slot.start)/a->c_slot.req_size;
    a->c_slot.start = a->c_slot.offset;
    a->c_slot.counter += count;
    
    if(a->prev_size != s)
    {
        a->c_slot.req_size = (int32_t)ALIGN(s);
    
        if(s > a->prev_size)
        {
            if(s <= a->c_slot.block_size)
            {
                //
                a->c_slot.offset += a->c_slot.req_size;
                if(a->c_slot.offset <= a->c_slot.end)
                {
                    a->prev_size = s;
                    return res;
                }
            }
        }
        else
        {
            // there is no difference of size request
            a->c_slot.offset += a->c_slot.req_size;
            if(a->c_slot.offset <= a->c_slot.end)
            {
                a->prev_size = s;
                return res;
            }
        }
    }
    
    if(a->c_slot.pend != 0)
    {
        uintptr_t offset = (a->c_slot.header & ~0x3) + a->c_slot.block_size;
        if(offset <= a->c_slot.pend)
        {
            int32_t* counter = (int32_t*)(a->c_slot.header & ~0x3);
            *counter = a->c_slot.counter;
            a->c_slot.header += a->c_slot.block_size;
            a->c_slot.counter = 0;
            a->c_slot.offset = sizeof(uintptr_t);
            a->c_slot.start = sizeof(uintptr_t);
            res = (void*)(uintptr_t)(offset + a->c_slot.offset);
            return res;
        }
    }
    return NULL;
}

static void *allocator_malloc_slot_arena(Allocator *a, size_t s, void* res)
{
    return NULL;
}

static void *allocator_malloc_slot(Allocator *a, size_t s, void* res)
{
    slot_type st = a->c_slot.header & 0x3;
    switch(st)
    {
        case SLOT_POOL:
            return allocator_malloc_slot_pool(a, s, res);
        case SLOT_COUNTER:
            return allocator_malloc_slot_counter(a, s, res);
        case SLOT_ARENA:
            return allocator_malloc_slot_arena(a, s, res);
        default:
            break;
    }
    return NULL;
}

void *allocator_malloc(Allocator_param *prm)
{
    size_t s = prm->size;
    size_t align = prm->alignment;
    bool zero = prm->zero;
    Allocator* a = NULL;
    
    if(prm->thread_id == main_thread_id)
    {
        a = main_instance;
    }
    else
    {
        a = get_instance(prm->thread_id);
    }
    
    void* res = (void*)(uintptr_t)((a->c_slot.header & ~0x3) + a->c_slot.offset);
    //
    if(a->prev_size == s)
    {
        if(IS_ALIGNED(res, align))
        {
            int32_t offset = a->c_slot.offset + a->c_slot.req_size;
            if(offset <= a->c_slot.end)
            {
                a->c_slot.offset = offset;
                return res;
            }
        }
    }
    
    res = allocator_malloc_slot(a, s, res);
    if(res != NULL)
    {
        return res;
    }
    
    // get the internal allocation slot
    internal_alloc ialloc = allocator_load_memory_slot(a, s, align, zero);
    // if we were handed the null allocator
    if(ialloc == allocator_slot_alloc_null)
    {
        // out of memory... 
        return NULL;
    }
    // commit our memory slot and return the address
    // the null allocator slot, just returns NULL.
    return ialloc(a, s);
}
void allocator_release_deferred(Allocator* a)
{
    for (int j = 0; j < POOL_BIN_COUNT; j++) {
        Pool* start = a->pools[j].head;
        while(start != NULL)
        {
            Pool* next = start->next;
            pool_move_deferred(start);
            if(pool_is_full(start))
            {
                pool_set_full(start, a);

            }
            start = next;
        }
    }
}

bool allocator_release_local_areas(Allocator *a)
{
    allocator_release_slot(a);
    deferred_release(a, NULL);
    bool result = false;
    
    
    allocator_release_deferred(a);
    /*
    bool was_released = partition_allocator_release_local_areas(partition_allocator, a);
    
    if (was_released) {
        for (int j = 0; j < POOL_BIN_COUNT; j++) {
            if (a->pools[j].head != NULL || a->pools[j].tail != NULL) {
                a->pools[j].head = NULL;
                a->pools[j].tail = NULL;
            }
        }
        for (int j = 0; j < ARENA_SBIN_COUNT; j++) {
            a->arenas[j].head = NULL;
            a->arenas[j].tail = NULL;
        }
    }
     
    result |= !was_released;
     */
    return !result;
}

void allocator_free(Allocator *a, void *p)
{
    _allocator_free(a, p);
}

void allocator_free_th(Allocator *a, void *p)
{
    allocator_free(a, p);
}

