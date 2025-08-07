

#include "allocator.h"
#include "partition_allocator.h"
#include "pool.h"
#include "os.h"
#include "arena.h"
#include "implicit_list.h"

extern PartitionAllocator *partition_allocator;
extern uintptr_t main_thread_id;
extern Allocator *main_instance;

Allocator *allocator_aquire(uintptr_t thread_id, uintptr_t thr_mem)
{
    if(thr_mem == 0UL)
    {
        return NULL;
    }
    Allocator *alloc = (Allocator *)thr_mem;
    alloc->prev_size = -1;
    thr_mem = ALIGN_CACHE(thr_mem + sizeof(Allocator));
    alloc->thread_id = thread_id;
    // next come the partition allocator structs.
    Queue *pool_queue = (Queue *)thr_mem;
    thr_mem = ALIGN_CACHE(thr_mem + sizeof(Queue) * POOL_BIN_COUNT);
    Queue *arena_queue = (Queue *)thr_mem;
    thr_mem = ALIGN_CACHE(thr_mem + sizeof(Queue) * PARTITION_COUNT);
    Queue *implicit_queue = (Queue *)thr_mem;
    thr_mem = (uintptr_t)alloc + DEFAULT_OS_PAGE_SIZE;
    thr_mem -= ALIGN_CACHE(sizeof(PartitionAllocator));
    
    alloc->pools = pool_queue;
    alloc->arenas = arena_queue;
    alloc->implicit = implicit_queue;
    return alloc;
}

void* allocator_slot_alloc_null(Allocator*a,  const size_t as)
{
    return NULL;
}

void* allocator_slot_alloc_pool(Allocator*a,  const size_t as)
{
    return pool_aquire_block((Pool*)(a->c_slot.header), a);
}

void* allocator_slot_alloc_implicit(Allocator*a,  const size_t as)
{
    //
    // if the size is not a power of two, we need to use the implicit list.
    if(!POWER_OF_TWO(as))
    {
        return implicitList_get_block((ImplicitList*)(a->c_slot.header),
                                      (int32_t)as,
                                      a->c_slot.alignment);
    }
    else
    {
        return NULL; // forward exact 2 powers to the arenas.
    }
}

static inline void* _allocator_slot_alloc(alloc_slot_front*a)
{
    void* res = (void*)(uintptr_t)((a->header) + a->offset);
    a->offset += a->req_size;
    return res;
}

void* allocator_slot_alloc(Allocator*a,  const size_t as)
{
    return _allocator_slot_alloc(&a->c_slot);
}

void* allocator_slot_region_alloc(Allocator*a,  const size_t as)
{
    return (void *)((uintptr_t)(a->c_slot.header));
}

internal_alloc allocator_set_implicit_slot(Allocator *a, uintptr_t p, uint32_t as_exp)
{
    if (p == 0UL)
    {
        return allocator_slot_alloc_null;
    }
    a->c_slot.header = (uintptr_t)p;
    a->c_slot.type = SLOT_IMPLICIT;
    a->c_slot.block_size = 1 << as_exp;
    a->c_slot.alignment = a->c_slot.block_size;
    a->c_slot.req_size = a->c_slot.block_size;
    a->c_slot.offset = 0;
    a->c_slot.start = 0;
    a->c_slot.end = PARTITION_SECTION_SIZE(a->c_back.partition_index);
    a->c_slot.is_zero = a->c_slot.is_zero;
    a->c_slot.counter = 0;

    return allocator_slot_alloc_implicit;
}

internal_alloc allocator_set_region_slot(Allocator *a, uintptr_t p)
{
    if(p == 0UL)
    {
        return allocator_slot_alloc_null;
    }
    a->c_slot.header = (uintptr_t)p;
    a->c_slot.type = SLOT_REGION;
    a->c_slot.offset = 0;
    a->c_slot.end = 0;
    
    a->c_slot.block_size = 0;
    a->c_slot.alignment = 0;
    a->c_slot.req_size = 0;

    return allocator_slot_region_alloc;
}

internal_alloc allocator_set_pool_slot(Allocator *a, Pool *p)
{
    if(p == NULL)
    {
        return allocator_slot_alloc_null;
    }
    
    a->c_slot.type = SLOT_POOL;
    a->c_slot.header = (uintptr_t)p;
    a->c_slot.block_size = (int32_t)p->block_size;
    a->c_slot.alignment = p->alignment;
    a->c_slot.req_size = (int32_t)p->block_size;
    
    int32_t rem_blocks = p->num_available - p->num_committed;
    pool_post_used(p, a);
    
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


internal_alloc allocator_set_arena_slot(Allocator *a, void *p, uint32_t block_size, uint32_t exp, uint32_t block_count, int32_t start_idx)
{
    uintptr_t start_addr = (uintptr_t)p;
    if (start_addr == a->c_slot.header || p == NULL) {
        return allocator_slot_alloc_null;
    }
    Arena* arena = (Arena*)p;
    a->c_slot.header = (uintptr_t)p;
    a->c_slot.type = SLOT_ARENA;
    a->c_slot.block_size = block_size;
    a->c_slot.alignment = block_size << exp;
    a->c_slot.req_size = block_size*block_count;
    
    //int32_t _max_zeros = num_consecutive_zeros(arena->allocations | ((1ULL << (exp + 1)) - 1));
    //int32_t _offset = find_first_nzeros(arena->allocations, _max_zeros, exp);
    
    uintptr_t end_mask = arena->in_use & ~((1ULL << start_idx) - 1);
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
    Pool* p = (Pool*)(a->c_slot.header);
    Queue *queue = &a->pools[p->block_idx];
    uint32_t rem_blocks = 0;
    if(a->c_slot.offset < a->c_slot.end)
    {
        rem_blocks = (a->c_slot.end - a->c_slot.offset)/a->c_slot.block_size;
        p->num_used -= rem_blocks;
        p->num_committed -= rem_blocks;
        if (!pool_is_consumed(p)) {
            
            if(pool_is_unused(p))
            {
                pool_post_unused(p, a);
            }
        }
        
        if(!pool_is_connected(p) && queue->head != p)
        {
            list_enqueue(queue, p);
            pool_post_used(p, a);
        }
    }
    else
    {
        if(pool_is_consumed(p))
        {
            if(pool_is_connected(p) || queue->head == p)
            {
                list_remove(queue, p);
            }
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
    Arena* p = (Arena*)(a->c_slot.header);
    
    size_t count = (a->c_slot.offset - a->c_slot.start)/ a->c_slot.req_size;
    size_t cidx = a->c_slot.offset / a->c_slot.block_size;
    uint32_t block_count = a->c_slot.req_size/a->c_slot.block_size;
    uint64_t mask = (1ULL << (count*block_count) ) - 1;
    size_t sidx = cidx - (count*block_count);
    p->in_use |= (mask << sidx);
    
    if(block_count > 1)
    {
        // we need to assign our ranges.
        for(int32_t i = 0; i < count; i++)
        {
            size_t offset = sidx+(i*block_count);
            p->ranges |= apply_range(block_count, (uint32_t)offset);
        }
    }
    
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
            list_append(queue,p);
        }
    }
    a->c_slot.offset = 0;
    a->c_slot.end = 0;
    a->c_slot.header = 0;
    a->c_slot.start = 0;
}


static inline void allocator_release_slot(Allocator *a)
{
    
    if (a->c_slot.header) {
        switch(a->c_slot.type)
        {
            case SLOT_POOL:
                return allocator_release_pool_slot(a);
            case SLOT_ARENA:
                return allocator_release_arena_slot(a);
            default:
                break;
        }
    }
}

void * allocator_alloc_region(Allocator* alloc, int32_t partition_idx, int32_t num_regions, int32_t* region_idx, bool zero, bool active)
{
    if (partition_idx < 0) {
        return NULL;
    }
    
    return partition_allocator_get_free_region(partition_allocator, partition_idx, num_regions, region_idx, active);
}

internal_alloc allocator_load_region_slot(Allocator *a, const size_t s, bool zero, bool active)
{
    int32_t region_idx = -1;
    void* region = allocator_alloc_region(a, AT_FIXED_256, 1, &region_idx, zero, active);
    if(region)
    {
        return allocator_set_region_slot(a, (uintptr_t)region);
    }
    else
    {
        return allocator_slot_alloc_null;
    }
    
}

internal_alloc allocator_malloc_pool_find_fit(Allocator* alloc, const uint32_t pc)
{
    // check if there are any pools available.
    Queue *queue = &alloc->pools[pc];
    alloc_base* start = queue->head;
    
    while(start != NULL)
    {
        if(((Pool*)start)->block_idx != pc)
        {
            alloc_base* next = start->next;
            list_remove(queue, start);
            start = next;
            continue;
        }
        if(pool_is_unused((Pool*)start))
        {
            //list_remove(queue, start);
            pool_clear((Pool*)start);
            //pool_post_unused((Pool*)start, alloc);
            return allocator_set_pool_slot(alloc, (Pool *)start);
        }
        else if(!pool_is_consumed((Pool*)start))
        {
            //list_remove(queue, start);
            return allocator_set_pool_slot(alloc, (Pool *)start);
        }
        else if(pool_is_consumed((Pool*)start))
        {
            alloc_base* next = start->next;
            list_remove(queue, start);
            start = next;
        }
    }
    return allocator_slot_alloc_null;
}

static inline uintptr_t allocator_get_arena_blocks(Allocator* alloc, int32_t arena_idx,
                                                   int32_t min_free_blocks, uint8_t exp,
                                                   bool zero, int32_t* midx)
{
    Queue* aqueue = &alloc->arenas[arena_idx];
    alloc_base* start = aqueue->head;
    *midx = 0;
    
    // !active && in_use... in fully consumed but not in cache.
    // active && in_use... in cache and has memory handed out.
    // active && !in_use... in cache but no memory handed out.
    // !active && !in_use... completely free to use.
    
    while(start != NULL)
    {
        Arena* arena = (Arena*)start;
        alloc_base* next = start->next;
    
        uint64_t in_use = atomic_load(&arena->in_use);
        if(in_use != UINT64_MAX)
        {
            if((*midx = find_first_nzeros(in_use, min_free_blocks, exp)) != -1)
            {
                // in this case...
                break;
            }
        }
        
        start = next;
    }


    
    if(start == NULL)
    {
        int32_t region_idx = 0;
        start = allocator_alloc_region(alloc, arena_idx, 1,  &region_idx, zero, true);
        if(start != NULL)
        {
            *midx = 1<<exp;
            Arena* arena = (Arena*)start;
            arena->thread_id = alloc->thread_id;
            arena->partition_id = arena_idx;
            arena->idx = (region_idx << 4)| SLOT_ARENA;
            arena->in_use = 1;
            
            if(!arena_is_connected(arena) && aqueue->head != arena)
            {
                list_enqueue(aqueue, start);
            }
        }
        else
        {
            return 0;
        }
    }
    
    
    size_t block_size = ARENA_CHUNK_SIZE(alloc->c_back.partition_index);
    Arena* arena = (Arena*)start;
    uint64_t active = atomic_load(&arena->active);
    uintptr_t new_chunk = ((uintptr_t)start + (*midx * block_size));
    if((active & (1ULL <<  *midx)) != 0)
    {
        // if the memory is active, that means that it is found in a queue
        // at this level we only store active states for pools.
        Pool* new_pool = (Pool*)new_chunk;
        Queue *pqueue = &alloc->pools[new_pool->block_idx];
        if(pool_is_connected(new_pool) || pqueue->head == new_pool)
        {
            list_remove(pqueue, new_pool);
        }
    }
    else
    {
        arena_allocate_blocks(alloc, arena, *midx, min_free_blocks);
    }
    
    return new_chunk;
}

static bool is_multiple_of_power_in_range(uint32_t size, uint32_t *largest_power, uint32_t min, uint32_t max) {
    if (size < min || size > max) return false;
    
    // Count trailing zeros (number of times divisible by 2)
    unsigned int power = (size & -size); // Extracts the lowest set bit
    
    if (power >= min && power <= max) {
        *largest_power = power;
        return true;
    } else {
        return false;
    }
}

static inline void allocator_malloc_leq_32k_init(Allocator* alloc, const size_t size, const size_t alignment, const bool zero)
{
    uint8_t pc = size_to_pool(size);
    int32_t row = pc/8;
    uint8_t arena_idx = MIN(row, 5);
    alloc->c_back.min_size = pc == 0? 0 : (pool_sizes[pc-1] + 1);
    alloc->c_back.max_size = pool_sizes[pc];
    alloc->c_back.partition_index = arena_idx;
    alloc->c_back.exp = pc; // hijack this member for our pools
    alloc->c_slot.type = SLOT_POOL;
}

static inline void allocator_malloc_leq_4m_init(Allocator* alloc, const size_t size, const size_t alignment, const bool zero)
{
    bool power2 = POWER_OF_TWO(size);
    
    uint32_t idx = (31 - __builtin_clz((uint32_t)(size - 1) >> 15));
    uint32_t result_power = 1;
    alloc->c_back.num_blocks = 1;
    alloc->c_back.partition_index = idx;
    alloc->c_back.min_size = 1 << (15+idx);
    alloc->c_back.max_size = 1 << (15+idx+1);
    if(!power2)
    {
        if(is_multiple_of_power_in_range((uint32_t)size, &result_power, (1 << 15), (1ULL << 22))){
            alloc->c_back.num_blocks = (uint32_t)size/result_power;
            if(alloc->c_back.num_blocks > 32)
            {
                result_power = result_power << 1;
                alloc->c_back.num_blocks = (uint32_t)size/result_power;
                alloc->c_back.partition_index++;
            }
            power2 = true;
        }
    }
    
    const uint32_t as_exp = ARENA_CHUNK_SIZE_EXPONENT(alloc->c_back.partition_index);
    if(power2)
    {
        alloc->c_back.num_blocks = size >> as_exp;
        size_t delta = (alignment >> as_exp);
        alloc->c_back.exp = delta == 0? 0 : __builtin_ctzll(delta);
        alloc->c_slot.type = SLOT_ARENA;
    }
    else
    {
        if(alignment == sizeof(intptr_t))
        {
            alloc->c_slot.type = SLOT_IMPLICIT;
        }
        else
        {
            // We should not ever reach this.
            alloc->c_slot.type = SLOT_OS;
        }
    }
}
static inline void allocator_malloc_leq_256m_init(Allocator* alloc, const size_t _size, const size_t alignment, const bool zero)
{
    
    size_t size = _size;
    bool power2 = POWER_OF_TWO(size);
    if(!power2)
    {
        size = ALIGN_UP_2(size, os_page_size);
    }
    uint32_t result_power = 1;
    alloc->c_back.num_blocks = 1;
    bool use_arena = power2;
    if(!power2)
    {
        if(is_multiple_of_power_in_range((uint32_t)size, &result_power, (1 << 22), (1ULL << 28))){
            alloc->c_back.num_blocks = (uint32_t)size/result_power;
            if(alloc->c_back.num_blocks > 32)
            {
                result_power = result_power << 1;
                alloc->c_back.num_blocks = (uint32_t)size/result_power;
                alloc->c_back.partition_index++;
            }
            use_arena = true;
            alloc->c_back.partition_index = bitlength((uint32_t)result_power >> 23);
        }
    }
    
    if(use_arena)
    {
        if(power2)
        {
            alloc->c_back.partition_index = bitlength((uint32_t)size >> 23);
        }
        
        const uint32_t as_exp = ARENA_CHUNK_SIZE_EXPONENT(alloc->c_back.partition_index);
        
        size_t delta = (alignment >> as_exp);
        alloc->c_back.exp = delta == 0? 0 : __builtin_ctzll(delta);
        alloc->c_slot.type = SLOT_REGION;
        alloc->c_back.min_size = _size;
        alloc->c_back.max_size = size;
    }
    else
    {
        size_t idx = bitlength((uint32_t)(size - 1) >> 24);
        alloc->c_back.min_size = (1 << (22+idx*2));
        alloc->c_back.max_size = (1 << (24+idx*2));
        alloc->c_back.partition_index = idx + 6;
        alloc->c_slot.type = SLOT_IMPLICIT;
    }
}


static inline internal_alloc allocator_malloc_back(Allocator* alloc)
{
    if(alloc->c_slot.type == SLOT_POOL)
    {
        internal_alloc res = allocator_malloc_pool_find_fit(alloc,
                                                            alloc->c_back.exp);
        if(res == allocator_slot_alloc_null)
        {
            int32_t midx = 0;
            size_t block_size = ARENA_CHUNK_SIZE(alloc->c_back.partition_index);
            uintptr_t start = allocator_get_arena_blocks(alloc,
                                                 alloc->c_back.partition_index,
                                                 1,
                                                 0,
                                                 alloc->c_slot.is_zero,
                                                 &midx);
            if(start != 0)
            {
                alloc->c_back.header = ALIGN_DOWN_2(start, ARENA_SIZE(alloc->c_back.partition_index));
                alloc->c_back.index = midx;
                Pool* new_pool = (Pool*)start;
                pool_init(new_pool, midx, alloc->c_back.exp, (uint32_t)block_size);
                res = allocator_set_pool_slot(alloc, new_pool);
                Queue *aqueue = &alloc->pools[alloc->c_back.exp];
                if(!pool_is_connected(new_pool) && aqueue->head != new_pool)
                {
                    list_enqueue(aqueue, new_pool);
                }
            }
        }
        return res;
    }
    else if(alloc->c_slot.type == SLOT_ARENA)
    {
        int32_t midx = 0;
        size_t block_size = ARENA_CHUNK_SIZE(alloc->c_back.partition_index);
        uintptr_t start = allocator_get_arena_blocks(alloc,
                                                     (int32_t)alloc->c_back.partition_index,
                                                     (uint32_t)alloc->c_back.num_blocks,
                                                     (uint32_t)alloc->c_back.exp,
                                                     alloc->c_slot.is_zero,
                                                     &midx);
        // we allocate a single block
        // we create an arena slot
        if(start != 0)
        {
            alloc->c_back.header = ALIGN_DOWN_2(start, ARENA_SIZE(alloc->c_back.partition_index));
            alloc->c_back.index = midx;
            size_t area_size = block_size << 6;
            Arena *arena = (Arena *)((uintptr_t)start & ~(area_size - 1));
            return allocator_set_arena_slot(alloc, (void*)arena, (uint32_t)block_size,
                                            (uint32_t)alloc->c_back.exp,
                                            (uint32_t)alloc->c_back.num_blocks,
                                            midx);
        }
        return allocator_slot_alloc_null;
    }
    else if(alloc->c_slot.type == SLOT_IMPLICIT)
    {
        const uint32_t as_exp = ARENA_SIZE_EXPONENT(alloc->c_back.partition_index);
        int32_t region_idx = 0;
        uintptr_t start = (uintptr_t)allocator_alloc_region(alloc,
                                                            alloc->c_back.partition_index,
                                                            1,
                                                            &region_idx,
                                                            alloc->c_slot.is_zero,
                                                            true);
        alloc->c_back.header = ALIGN_DOWN_2(start, PARTITION_SECTION_SIZE(alloc->c_back.partition_index));
        alloc->c_back.index = region_idx;
        ImplicitList* new_bt = (ImplicitList*)start;
        implicitList_init(new_bt, region_idx, 1 << as_exp);
        new_bt->thread_id = alloc->thread_id;
        new_bt->partition_id = alloc->c_back.partition_index;
        new_bt->idx = (region_idx << 4)| SLOT_IMPLICIT;
        Queue *aqueue = &alloc->implicit[alloc->c_back.partition_index];
        if(!base_is_connected((alloc_base*)new_bt) && aqueue->head != new_bt)
        {
            list_enqueue(aqueue, new_bt);
        }
        // boundary tag allocator
        return allocator_set_implicit_slot(alloc, (uintptr_t)new_bt, as_exp);

    }
    else if(alloc->c_slot.type == SLOT_REGION)
    {
        int32_t region_idx = 0;
        uintptr_t start = (uintptr_t)allocator_alloc_region(alloc,
                                                            alloc->c_back.partition_index,
                                                            1,
                                                            &region_idx,
                                                            alloc->c_slot.is_zero,
                                                            false);
        return allocator_set_region_slot(alloc, start);
    }
    
    return allocator_slot_alloc_null;
}

static inline internal_alloc allocator_malloc_base(Allocator* alloc, size_t size, size_t alignment, const bool zero)
{
    // are we requesting similar blocks.
    if(alloc->c_back.header)
    {
        if(size >= alloc->c_back.min_size && size <= alloc->c_back.max_size)
        {
            if(alignment == alloc->c_slot.alignment)
            {
                return allocator_malloc_back(alloc);
            }
        }
    }
    
    memset(&alloc->c_back, 0, sizeof(alloc_slot_back));
    
    
    if(size <= (1 << 15)) // 8 <= n <= 32k
    {
        allocator_malloc_leq_32k_init(alloc, size, alignment, zero);
    }
    else if(size < (1ULL << 22)) // 32k < n <= 4m)
    {
        allocator_malloc_leq_4m_init(alloc, size, alignment, zero);
    }
    else if(size < (1ULL << 28)) // 4m < n <= 256m
    {
        allocator_malloc_leq_256m_init(alloc, size, alignment, zero);
    }
    else // n > 256m
    {
        alloc->c_slot.type = SLOT_OS;
    }

    return allocator_malloc_back(alloc);
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
    // Lets try to hand out memory from our current slot.
    if(a->prev_size != s)
    {
        // is it larger but still small enough to not need a new size class?
        if(s > a->prev_size)
        {
            if(s <= a->c_slot.block_size)
            {
                // lets make sure we still have blocks to hand out.
                int32_t offset = a->c_slot.offset + a->c_slot.req_size;
                if(offset <= a->c_slot.end)
                {
                    a->c_slot.offset += a->c_slot.req_size;
                    a->prev_size = s;
                    return res;
                }
            }
        }
        else
        {
            // If the memory is smaller
            // we need to check if it is still in our size class
            Pool* p = (Pool*)(a->c_slot.header);
            if(p->block_idx == size_to_pool(s))
            {
                int32_t offset = a->c_slot.offset + a->c_slot.req_size;
                if(offset <= a->c_slot.end)
                {
                    a->c_slot.offset += a->c_slot.req_size;
                    a->prev_size = s;
                    return res;
                }
            }
        }
    }
    return allocator_slot_alloc_pool(a, s);
}

static void *allocator_malloc_slot_arena(Allocator *a, size_t s, void* res)
{
    return NULL;
}

static void *allocator_malloc_slot(Allocator *a, size_t s, void* res)
{
    slot_type st = a->c_slot.type;
    switch(st)
    {
        case SLOT_POOL:
            return allocator_malloc_slot_pool(a, s, res);
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
    
    // if this is our main thread, we pick up our cozy global.
    if(prm->thread_id == main_thread_id)
    {
        a = main_instance;
    }
    else
    {
        // else we need to fetch the instance for this thread
        a = get_instance(prm->thread_id);
    }
    
    // Check our front-end contiguous cache
    if(a->c_slot.header)
    {
        switch(a->c_slot.type)
        {
            case SLOT_POOL:
            case SLOT_ARENA:
            {
                // Lets pre-load a contiguous address
                void* res = (void*)(uintptr_t)((a->c_slot.header) + a->c_slot.offset);
                // ensure that the size requested matches
                if(a->prev_size == s)
                {
                    // Does it match our alignment request
                    if(IS_ALIGNED(res, align))
                    {
                        // Have we gone passed the end of our block?
                        int32_t offset = a->c_slot.offset + a->c_slot.req_size;
                        if(offset <= a->c_slot.end)
                        {
                            // Still valid
                            a->c_slot.offset = offset;
                            return res;
                        }
                    }
                }
                // if the size is within our blocks margin
                // we can hand out more memory.
                // or we have exhausted our contiguous memory and we
                // need to see if anything is in our free lists.
                res = allocator_malloc_slot(a, s, res);
                if(res != NULL)
                {
                    return res;
                }
                break;
            }
            case SLOT_IMPLICIT:
            {
                
                //
                if(s >= a->c_back.min_size && s <= a->c_back.max_size)
                {
                    // Lets try to hand out memory from our current slot.
                    void* res = allocator_slot_alloc_implicit(a, s);
                    if(res != NULL)
                    {
                        return res;
                    }
                }
            
                
                break;
            }
            default:
                break;
        }
       
        
        
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
    return ialloc(a, s);
}

static inline __attribute__((always_inline)) void _allocator_free(Allocator *a, void *p)
{
    // always safe to free NULL
    if(p == NULL)
    {
        return;
    }
    
    // Lets compare the last memory handed out, to the freed memory
    void* res = (void*)(uintptr_t)(((a->c_slot.header) + a->c_slot.offset) - a->c_slot.req_size);
    if(res == p)
    {
        // we just returned the last memory allocated
        // so we just offset our slot.
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


void allocator_release_deferred(Allocator* a)
{
    // Walk over all of our pools and move deferred memory
    // to its home.
    for (int j = 0; j < POOL_BIN_COUNT; j++) {
        Pool* start = a->pools[j].head;
        while(start != NULL)
        {
            Pool* next = start->next;
            pool_move_deferred(start);
            if(pool_is_unused(start))
            {
                pool_set_unused(start, a);
            }
            start = next;
        }
    }
}

bool allocator_release_local_areas(Allocator *a)
{
    // release any cache data
    allocator_release_slot(a);
    deferred_release(a, NULL);
    bool result = false;
    
    // move all deferred back into the container
    allocator_release_deferred(a);
    bool was_released = true;
    // Try to release all our arena blocks back to the partition allocator
    for(int32_t i = 0; i < ARENA_BIN_COUNT; i++){
        Queue* queue = &a->arenas[i];
        Arena* start = queue->head;
        while (start) {
            Arena* next = start->next;
            
            if(start->in_use <= 1)
            {
                partition_allocator_free_blocks(partition_allocator, start, true);
            }
            else
            {
                was_released = false;
            }

            start = next;
        }
    }
    // If it was completely empty, we dump our cache lists.
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

