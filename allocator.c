

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
    // next come the partition allocator structs.
    Queue *pool_queue = (Queue *)thr_mem;
    thr_mem = ALIGN_CACHE(thr_mem + sizeof(Queue) * POOL_BIN_COUNT);
    Queue *arena_queue = (Queue *)thr_mem;
    thr_mem = ALIGN_CACHE(thr_mem + sizeof(Queue) * ARENA_BIN_COUNT);
    Queue *impll_queue = (Queue *)thr_mem;
    thr_mem = ALIGN_CACHE(thr_mem + sizeof(Queue) * IMPLICIT_LIST_BIN_COUNT);
    
    uintptr_t t = thr_mem + os_page_size;
    alloc->pools = pool_queue;
    alloc->arenas = arena_queue;
    alloc->implicit_lists =impll_queue;
    
    return alloc;
}

void* allocator_slot_alloc_null(Allocator*a,  const size_t as)
{
    return NULL;
}

void* allocator_slot_alloc_pool(Allocator*a,  const size_t as)
{
    return pool_aquire_block((Pool*)(a->c_slot), a);
}

static inline void* _allocator_slot_alloc(AllocatorContext*a)
{
    void* res = (void*)((uintptr_t)a + a->offset);
    a->offset += a->block_size;
    return res;
}

void* allocator_slot_alloc(Allocator*a,  const size_t as)
{
    return _allocator_slot_alloc(a->c_slot);
}


void* allocator_slot_area_alloc(Allocator*a,  const size_t as)
{
    return (void *)((uintptr_t)(a->c_slot));
}

AllocatorContext* allocator_set_pool_slot(Allocator *a, Pool *p)
{
    if(p == NULL)
    {
        return NULL;
    }
    if(p->start == 0 && p->end == 0)
    {
        int32_t rem_blocks = p->num_available - p->num_committed;
        pool_post_reserved(p, a);
        
        if(rem_blocks > 0)
        {
            p->end = (int32_t)(((uintptr_t)pool_base_address(p) - (uintptr_t)p) + (p->num_available * p->block_size));
            p->start = p->end - (rem_blocks *(int32_t)p->block_size);
            if((rem_blocks *(int32_t)p->block_size) > p->end)
            {
                int bb = 0;
            }
            p->offset = p->start;
            // reserve all memory from the pool
            p->num_used = p->num_available;
            p->num_committed = p->num_available;
            return (AllocatorContext*)p;
        }
        p->offset = 0;
        p->end = 0;
        p->start = 0;
    }

    return (AllocatorContext*)p;
}


AllocatorContext* allocator_set_arena_slot(Allocator *a, void *p, uint32_t block_size, uint32_t exp, uint32_t block_count, int32_t start_idx)
{
    if (p == a->c_slot || p == NULL) {
        return NULL;
    }
    Arena* arena = (Arena*)p;
    arena->block_size = block_size;
    //int32_t _max_zeros = num_consecutive_zeros(arena->allocations | ((1ULL << (exp + 1)) - 1));
    //int32_t _offset = find_first_nzeros(arena->allocations, _max_zeros, exp);
    
    uintptr_t end_mask = arena->in_use & ~((1ULL << start_idx) - 1);
    int32_t end_idx = end_mask == 0? 64 :__builtin_ctzll(end_mask);
    int32_t max_zeros = end_idx - start_idx;
    
    // from the start idx... count the number of zeros
    arena->start = start_idx*(int32_t)arena->block_size;
    arena->offset = arena->start;
    arena->end = arena->offset + max_zeros*(int32_t)arena->block_size;
    
    return (AllocatorContext*)arena;
}

static inline void allocator_release_pool_slot(Allocator *a, AllocatorContext* c)
{
    Pool* p = (Pool*)(c);
    Queue *queue = &a->pools[p->block_idx];
    uint32_t rem_blocks = 0;
    if(p->offset < p->end)
    {
        rem_blocks = (p->end - p->offset)/p->block_size;
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
    p->offset = 0;
    p->end = 0;
    p->start = 0;
}

static inline void allocator_release_arena_slot(Allocator *a, AllocatorContext* c)
{
    Arena* p = (Arena*)c;
    
    size_t count = (p->offset - p->start)/ p->block_size;
    size_t cidx = p->offset / p->block_size;
    uint64_t mask = (1ULL << (count) ) - 1;
    size_t sidx = cidx - (count);
    p->in_use |= (mask << sidx);
    
    if(p->in_use == UINT64_MAX)
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
    p->offset = 0;
    p->end = 0;
    p->start = 0;
}


static inline void _allocator_release_slot(Allocator *a)
{
    allocator_release_slot(a, a->c_slot);
}

void allocator_release_slot(Allocator *a, AllocatorContext* c)
{
    if(c)
    {
        if(c->parent_idx & 0x1){
            return allocator_release_arena_slot(a, c);
        }
        else{
            return allocator_release_pool_slot(a, c);
        }
    }
}

void * allocator_alloc_arena(Allocator* alloc, int32_t partition_idx, int32_t* region_idx, bool zero)
{
    if (partition_idx < 0) {
        return NULL;
    }
    
    return partition_allocator_get_free_region(partition_allocator, partition_idx, region_idx);
}


AllocatorContext* allocator_malloc_pool_find_fit(Allocator* alloc,
                                                const uint32_t pc)
{
    // check if there are any pools available.
    Queue *queue = &alloc->pools[pc];
    AllocatorContext* start = queue->head;
    
    if(start != NULL)
    {
        if((start->offset + start->block_size) <= start->end)
        {
            return start;
        }
        else
        {
            allocator_release_slot(alloc, start);
        }
        AllocatorContext* next = start->next;
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
            if(is_connected(start) || queue->head == start)
            {
                list_remove(queue, start);
            }
        }
        start = next;
    }
    return NULL;
}

static inline uintptr_t allocator_get_arena_blocks(Allocator* alloc, int32_t arena_idx, int32_t min_free_blocks, uint8_t exp, bool zero, int32_t* midx, size_t* block_size)
{
    Queue* aqueue = &alloc->arenas[arena_idx];


    AllocatorContext* start = NULL;
    *midx = 0;
    if(exp == 0)
    {
        start = aqueue->head;
        int32_t c = 0;
        while(start != NULL)
        {
            AllocatorContext* next = start->next;
            if(((Arena*)start)->in_use != UINT64_MAX)
            {
                if((*midx = find_first_nzeros(((Arena*)start)->in_use, min_free_blocks, exp)) != -1)
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
            arena->in_use = 1;
            arena->partition_id = arena_idx;
            arena->idx = (region_idx << 1)| 0x1;
            
            if(!is_connected(start) && aqueue->head != start)
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

static inline AllocatorContext* allocator_malloc_leq_32k(Allocator* alloc,
                                                        const size_t size,
                                                        const size_t alignment,
                                                        const bool zero)
{
    int32_t row_map[] = {0,1,2,3,4,5,5,5,5,5};
    uint8_t pc = size_to_pool(size);
    AllocatorContext* res = allocator_malloc_pool_find_fit(alloc, pc);
    if(res == NULL)
    {
        int32_t row = pc/8;
        uint8_t arena_idx = row_map[row];
        
        int32_t midx = 0;
        size_t block_size = 0;
        uintptr_t start = allocator_get_arena_blocks(alloc, arena_idx, 1, 0, zero, &midx, &block_size);
        if(start != 0)
        {
            Queue* aqueue = &alloc->pools[pc];
            Pool* new_pool = (Pool*)start;
            pool_init(new_pool, midx, pc, (uint32_t)block_size);
            res = allocator_set_pool_slot(alloc, new_pool);
            if(!pool_is_connected(new_pool) && aqueue->head != new_pool)
            {
                list_enqueue(aqueue, new_pool);
            }
        }
    }
    
    return res;
}

static inline AllocatorContext* allocator_malloc_base(Allocator* alloc, size_t size, size_t alignment, const bool zero)
{
    AllocatorContext* res = NULL;
    
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
                //res = allocator_load_area_slot(alloc, rsize);  //
                // alloc from partition allocator
            }
            else
            {
                res = NULL;
            }
        }
    }
    return res;
}


static inline __attribute__((always_inline)) void _allocator_free(Allocator *a, void *p)
{
    
    uintptr_t slot_top = (uintptr_t)(a->c_slot);
    if(a->c_slot)
    {
        void* res = (void*)(slot_top + a->c_slot->offset - a->c_slot->block_size);
        if(res == p)
        {
            // we just returned the last memory allocated
            // so we just offset our slot
            a->c_slot->offset -=a->c_slot->block_size;
            return;
        }
    }

    if(a->c_deferred.end == 0)
    {
        if((uintptr_t)p >= slot_top && (uintptr_t)p <  (slot_top + a->c_slot->end))
        {
            _allocator_release_slot(a);
        }
        
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



static inline AllocatorContext* allocator_load_memory_slot(Allocator *a, size_t as, size_t alignment, bool zero)
{
    //allocator_release_slot(a);
    deferred_release(a, NULL);
    a->prev_size = (uint64_t)as;
    return allocator_malloc_base(a, ALIGN(as), alignment, zero);
}



static void *allocator_malloc_slot_pool(Allocator *a, size_t s, void *res)
{
    
    // we make the assumption we are handing out memory from a slot
    // just pre-loading this address just in case. Is true for most cases.
    Pool* p = (Pool*)(a->c_slot);
    if(a->prev_size != s)
    {
        if(s > a->prev_size)
        {
            if(s <= p->block_size)
            {
                //
                a->c_slot->offset += a->c_slot->block_size;
                if(a->c_slot->offset <= a->c_slot->end)
                {
                    a->prev_size = s;
                    return res;
                }
            }
        }
        else
        {
            //
            // new size is smaller then the previous size.
            // what if the min size is smaller than the cache alignment supplies.
            // lets see if the size is small enough to justify creating a counter
            // alloc within the current slot
            //
            /*
            int64_t delta = (int64_t)(a->prev_size - s);
            if(delta < a->c_slot.alignment)
            {
                a->c_slot.offset += a->c_slot.req_size;
                if(a->c_slot.offset <= a->c_slot.end)
                {
                    a->prev_size = s;
                    return res;
                }
            }
            else
            {
                
                //if(POWER_OF_TWO(a->c_slot.block_size))
                {
                    if((a->c_slot.block_size/s) >= MIN_BLOCKS_PER_COUNTER_ALLOC)
                    {
                        a->c_slot.offset += a->c_slot.req_size;
                        allocator_set_counter_slot(a, res, a->c_slot.req_size, a->c_slot.header, a->c_slot.end);
                        a->c_slot.req_size = (int32_t)ALIGN(s);
                        a->c_slot.offset += a->c_slot.req_size;
                        a->prev_size = s;
                        return (void*)(uintptr_t)res + a->c_slot.start;
                    }
                }
            }
             */
        }
    }
    

    if(p->block_idx == size_to_pool(s))
    {
        return pool_aquire_block(p, a);
    }
    else
    {
        return NULL;
    }
}


static void *allocator_malloc_slot_arena(Allocator *a, size_t s, void* res)
{
    return NULL;
}

static void *allocator_malloc_slot(Allocator *a, size_t s, void* res)
{
    if(a->c_slot)
    {
        if(a->c_slot->parent_idx & 0x1)
        {
            return allocator_malloc_slot_arena(a, s, res);
        }
        else
        {
            return allocator_malloc_slot_pool(a, s, res);
        }
    }
    return NULL;
}

void *allocator_malloc(Allocator_param *prm)
{
    size_t s = prm->size;
    size_t align = prm->alignment;
    bool zero = prm->zero;
    Allocator* a = NULL;
    void* res = NULL;
    if(prm->thread_id == main_thread_id)
    {
        a = main_instance;
    }
    else
    {
        a = get_instance(prm->thread_id);
    }
    if(a->c_slot)
    {
        res = (void*)((uintptr_t)a->c_slot + a->c_slot->offset);
        //
        if(a->prev_size == s)
        {
            if(IS_ALIGNED(res, align))
            {
                int32_t offset = a->c_slot->offset + (int32_t)a->c_slot->block_size;
                if(offset <= a->c_slot->end)
                {
                    a->c_slot->offset = offset;
                    return res;
                }
                else
                {
                    _allocator_release_slot(a);
                }
            }
        }
    }
    
    res = allocator_malloc_slot(a, s, res);
    if(res != NULL)
    {
        return res;
    }
    
    // get the internal allocation slot
    a->c_slot = allocator_load_memory_slot(a, s, align, zero);

    // if we were handed the null allocator
    if(a->c_slot == NULL)
    {
        // out of memory... 
        return NULL;
    }
    // commit our memory slot and return the address
    // the null allocator slot, just returns NULL.
    return _allocator_slot_alloc(a->c_slot);
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
    _allocator_release_slot(a);
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

