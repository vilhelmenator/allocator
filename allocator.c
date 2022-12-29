

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
        ImplicitList *heap = (ImplicitList *)((uint8_t *)section + sizeof(Section));
        uint32_t heapIdx = area_get_type((Area *)section);
        implicitList_free(heap, p, false);
        // if the free pools list is empty.
        if (!implicitList_is_connected(heap)) {
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
            ImplicitList *heap = (ImplicitList *)((uintptr_t)area + sizeof(Area));
            implicitList_free(heap, p, true);
            // if the pool is disconnected from the queue
            if (!implicitList_is_connected(heap)) {
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
    const uint32_t heap_size_cls = size_to_implicitList(s);
    Queue *queue = &a->part_alloc->heaps[heap_size_cls];
    ImplicitList *start = (ImplicitList *)queue->head;
    while (start != NULL) {
        ImplicitList *next = start->next;
        if (implicitList_has_room(start, s)) {
            return implicitList_get_block(start, (uint32_t)s);
        } else {
            if(heap_size_cls != 0)
            {
                Area *area = area_from_addr((uintptr_t)start);
                if(area_is_full(area))
                {
                    AreaType at = area_get_type(area);
                    Partition* partition = &a->part_alloc->area[at];
                    int32_t aidx = partition_allocator_get_area_idx_from_queue(a->part_alloc, area, partition);
                    partition->full_mask |= (1ULL << aidx);
                }
            }
            list_remove(queue, start);
        }
        start = next;
    }

    if (heap_size_cls == 0) {
        Section *new_section = allocator_get_free_section(a, s, ST_HEAP_4M);
        if (new_section != NULL) {
            const unsigned int coll_idx = section_reserve_next(new_section);
            int32_t psize = (1 << size_clss_to_exponent[ST_HEAP_4M]);
            start = (ImplicitList *)section_get_collection(new_section, coll_idx, psize);
            implicitList_init(start, coll_idx, heap_sizes[0]);
            section_claim_idx(new_section, coll_idx);
            list_enqueue(queue, start);
            return implicitList_get_block(start, (uint32_t)s);
        }
    }

    AreaType at = get_area_type_for_heap(s);
    int32_t area_idx = -1;
    Partition* partition = partition_allocator_get_free_area(a->part_alloc, s, at, &area_idx);
    if(partition == NULL)
    {
        return NULL;
    }
    Area* new_area = partition_allocator_area_at_idx(a->part_alloc, partition, area_idx);
    if (new_area == NULL) {
        return NULL;
    }
    
    if((partition->zero_mask & 1ULL << area_idx) == 0)
    {
        at = partition_allocator_get_partition_idx(a->part_alloc, partition);
        area_init(new_area, a->idx, at);
        partition->zero_mask |= (1ULL << area_idx);
    }
    uint32_t idx = partition_allocator_get_area_idx_from_queue(a->part_alloc, new_area, partition);
    uint32_t range = get_range(idx, partition->range_mask);
    uint64_t area_size = area_get_size(new_area)*range;
    area_set_container_type(new_area, CT_HEAP);
    area_reserve_all(new_area);
    start = (ImplicitList *)((uintptr_t)new_area + sizeof(Area));
    implicitList_init(start, 0, area_size);

    list_enqueue(queue, start);
    return implicitList_get_block(start, (uint32_t)s);
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
        partition->area_mask |= (1ULL << area_idx);
        partition->full_mask |= (1ULL << area_idx);
    }
    area_reserve_all(area);
    area_set_container_type(area, CT_SLAB);
    return (void *)((uintptr_t)area + sizeof(Area));
}

static inline int64_t size_to_arena(size_t s)
{
    int32_t lz = 63 - __builtin_clzll(s);
    uint8_t szidx = lz - 4;
    return szidx % 6;
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

    return header;
}

void *allocator_alloc_aligned(Allocator *a, const size_t s, const size_t alignment)
{
    return NULL;
}

size_t size_to_allocation_class(const size_t s)
{
    if(s < (1 << 11)) // < 2k
    {
        // pools
        return 0;
    }
    else
    {
        if(s < (1 << 25)) // < 32m
        {
            // arenas 0 - 5
            return 1;
        }
        else if(s < (1 << 27)) // < 256m
        {
            // arena 6
            return 2;
        }
        else if(s < (1ULL << 33)) // 256m - 8g
        {
            // partition
            return 3;
        }
        else
        {
            // os
            return 4;
        }
    }
}

uint8_t size_to_sub_class(uint32_t clss, const size_t s)
{
    if(clss == 0)
    {
        //
        return size_to_pool(s);
    }
    else
    {
        // 2k - 16m
        const uint8_t arena_index [] = {
            4, 5,
            0, 1, 2, 3, 4, 5,
            0, 1, 2, 3, 4, 5,
        };
        //
        int32_t ss = __builtin_clzll(s);
        int32_t high_index = 64 - ss;
        return arena_index[high_index - 11];
    }
}

uint8_t size_to_arena_level(uint32_t clss, const size_t s)
{
    // 4, 8, 16, 32, 64, 128
    // size of arena
    return 0;
};

uint8_t alignment_offset(uint32_t clss, const size_t s)
{
    // if partition ...
    // if arena ...
    return 0;
};

Arena* allocator_get_arena(const uint8_t clss, const bool zero)
{
    return NULL;
}

void* allocator_alloc_arena_high(Allocator *a, Arena* arena, uint32_t range, ArenaAllocation *result)
{
    int32_t midx;
    void* res = arena_alloc_high(arena, range, 0, &result->tidx);
    if(res != NULL)
    {
        result->arena = arena;
        result->midx = 1; // first available b block.
        return res;
    }
    else
    {
        // the idx of the arena.
        size_t aidx = arena_get_arena_index(arena);
        a->part_alloc->area[aidx].full_mask &= ~(1ULL << aidx);
        // allocate a new arena.
        // we can go down by at most 5 steps.
        // 1, 2, 4, 8, 16, 32
        
        Arena* new_arena = allocator_alloc_arena(a, 1ULL << arena->container_exponent);
        if(new_arena != NULL)
        {
            result->arena = new_arena;
            result->tidx = 0; // the arena is completely empty
            result->midx = 1; // first available b block.
            return new_arena;
        }
        return NULL;
    }
}

void* allocator_alloc_arena_mid(Allocator *a, Arena* arena, uint32_t range, uint32_t tidx, ArenaAllocation *result)
{
    result->bidx = 1;
    void* res = arena_alloc_mid(arena, range, 0, tidx, &result->midx);
    if(res != NULL)
    {
        result->arena = arena;
        result->tidx = tidx;
        return res;
    }
    else
    {
        //
        Partition* partition = &a->part_alloc->area[arena_get_arena_index(arena)];
        int32_t aidx = partition_allocator_get_arena_idx_from_queue(a->part_alloc, arena, partition);
        partition->full_mask |= (1ULL << aidx);
        void* high = allocator_alloc_arena_high(a, arena, 1, result);
        if(high != NULL)
        {
            void* res = arena_alloc_mid(result->arena, range, 0, result->tidx, &result->midx);
            if(res != NULL)
            {
                return res;
            }
        }
        return NULL;
    }
}

void* allocator_alloc_arena_low(Allocator *a, Arena* arena, uint32_t range, uint32_t tidx, uint32_t midx, ArenaAllocation *result)
{
    int32_t bidx;
    void* res = arena_alloc_low(arena, range, 0, tidx, midx, &result->bidx);
    if(res != NULL)
    {
        result->arena = arena;
        result->tidx = tidx;
        result->midx = midx;
        return res;
    }
    else
    {
        //
        void* mid = allocator_alloc_arena_mid(a, arena, 1, midx, result);
        if(mid != NULL)
        {
            void* res = arena_alloc_low(result->arena, range, 0, result->midx, result->bidx, &bidx);
            if(res != NULL)
            {
                return res;
            }
        }
        return NULL;
    }
}

void* allocator_malloc_pool_find_fit(Allocator* alloc, const uint32_t pc)
{
    // check if there are any pools available.
    Queue *queue = &alloc->part_alloc->pools[pc];
    Heap* start = queue->head;
    if(start != NULL)
    {
        list_remove(queue, start);
        void* res = pool_aquire_block((Pool *)start);
        allocator_set_cached_pool(alloc, (Pool *)start, true);
        return res;
    }
    return NULL;
}

void* allocator_malloc_arena_find_fit(Allocator* alloc, const size_t size,  const size_t alignment)
{
    size_t as = ALIGN_UP_2(size, 1 << 4);
    int32_t lz = 63 - __builtin_clzll(as);
    uint8_t szidx = lz - 4;
    for(int si = szidx; si >= 0; si--)
    {
        Queue * queue = &alloc->part_alloc->aligned_cls[si];
        uint8_t clevel = si/6;
        Heap* start = queue->head;
        if(start != NULL)
        {
            int32_t arena_index = si % 6;
            uint32_t range = arena_get_range(arena_index, size, clevel);
            void* res = arena_alloc_at((uintptr_t)start, range, 0, clevel);
            if(res != NULL)
            {
                list_remove(queue, start);
                allocator_set_cached_arena(alloc, (Arena*)start);
            }
            return res;
        }
    }
    return NULL;
}

static inline uintptr_t allocator_arena_get_at(ArenaAllocation* alloc)
{
    const arena_size_table *stable = arena_get_size_table(alloc->arena);
    uintptr_t base = ((uintptr_t)alloc->arena & ~(os_page_size - 1));
    uintptr_t al1 = (uintptr_t)(base + (alloc->midx * stable->sizes[2]));
    return (al1 + (alloc->bidx * stable->sizes[1]));
}

void* getMemory(size_t base_size, size_t block_count, size_t block_offset, size_t refsize)
{
    
    //
    //
    //
    
    //
    // get size list.
    // get block count.
    //
    //
    return NULL;
}

void* allocator_base_allocation(size_t size, const size_t alignment)
{
    if(size == 0)
    {
        return NULL;
    }
    // if not zero, we align to the next multiple of wordwidth
    size = ALIGN(size);
    // alignment needs to be a power of 2.
    // the size needs to be a multiple of the alignment.
    if(!POWER_OF_TWO(alignment))
    {
        return NULL;
    }
    
    if(size < alignment)
    {
        size = alignment;
    }
    
    // if size is not a multiple of alignment.
    uint32_t highbit = MAX(63UL - __builtin_clzll(size), 5);
    uint32_t mask_offset = highbit - 5;
    size_t asize = 1;
    if(POWER_OF_TWO(size))
    {
        mask_offset += 5;
        for(uint32_t i = 0; i < 6; i++)
        {
            size_t base_size = (1 << (mask_offset - i));
            if(base_size < (1ULL << 4))
            {
                break;
            }
            void* res = getMemory(base_size, asize, 0, size);
            if(res != NULL)
            {
                return res;
            }
            asize *= 2;
        }
    }
    else
    {
        size_t block_offset = 1;
        uint64_t bmask = (1 << (mask_offset + 1)) - 1;
        asize = size >> mask_offset;
        if((size & bmask) != 0)
        {
            asize++; // round up if any low bit is set
        }
        for(uint32_t i = 0; i < 6; i++)
        {
            size_t base_size = (1 << (mask_offset + i));
            if((base_size >= (1ULL << 4)) && (asize < (1ULL << 6)))
            {
                block_offset = alignment >> (mask_offset + i);
                void* res = getMemory(base_size, asize, block_offset < 2? 0:block_offset, size);
                if(res != NULL)
                {
                    return res;
                }
            }
            size_t csize = asize >> 1;
            if((asize & 0x1) != 0)
            {
                csize++;
            }
            asize = csize;
        }
    }
    // next power of two
    // Desperate search is wasteful
    // seeks for 1 available block in the higher address ranges.
    for(uint32_t i = 0; i < 6; i++)
    {
        asize =  1ULL << (highbit + (i+1));
        void* res = getMemory(asize, 1, 0, size);
        if(res != NULL)
        {
            return res;
        }
    }
    return NULL;
}

void* allocator_malloc_aligned_lt32m(Allocator* alloc, const size_t size,  const size_t alignment, const bool zero)
{
    int32_t ss = __builtin_clzll(size);
    int32_t high_index = 63UL - ss;
    int32_t base_offset = high_index - 4;
    int32_t align_idx = base_offset;
    
    // check what is available at all levels.
    int32_t level_idx = align_idx / 6;
    int32_t arena_index = align_idx % 6;
    uint32_t range = arena_get_range(arena_index, size, level_idx);
    //
    // we start by searching the bottom and working our way
    // to a higher level within the same arena type
    Queue *queues[] = {
        &alloc->part_alloc->aligned_cls[align_idx],
        &alloc->part_alloc->aligned_cls[align_idx+6],
        &alloc->part_alloc->aligned_cls[align_idx+12],
        &alloc->part_alloc->aligned_z_cls[align_idx],
        &alloc->part_alloc->aligned_z_cls[align_idx+6],
        &alloc->part_alloc->aligned_z_cls[align_idx+12]};
    //
    Heap* start = NULL;
    // if zero is set, we skip over the nzero lists.
    int32_t start_level_idx = level_idx + (zero ? 3: 0);
    for(int start_level_idx = level_idx; start_level_idx < 6; start_level_idx++)
    {
        if(queues[start_level_idx]->head != NULL)
        {
            start = queues[start_level_idx]->head;
            break;
        }
    }
    //
    start_level_idx %= 3;
    ArenaAllocation result;
    void* block = NULL;
    uint32_t midx = -1;
    uint32_t bidx = -1;
    if(start != NULL)
    {
        Arena *arena = arena_get_header((uintptr_t)start);
        uintptr_t base = arena_get_parent_block(arena, (uintptr_t)start, AL_HIGH);
        if(level_idx == 0)
        {
            midx = arena_get_local_idx(arena, (uintptr_t)start, base, AL_HIGH);
            
            void* block = NULL;
            if(start_level_idx < 2)
            {
                uintptr_t base_l = arena_get_parent_block(arena, (uintptr_t)start, AL_MID);
                bidx = arena_get_local_idx(arena, (uintptr_t)start, base_l, AL_MID);
                block = allocator_alloc_arena_low(alloc, arena, range, midx, bidx, &result);
            }
            else
            {
                allocator_alloc_arena_mid(alloc, arena, 1, midx, &result);
                block = allocator_alloc_arena_low(alloc, result.arena, range, result.midx, result.bidx, &result);
            }
        }
        else if(level_idx == 1)
        {
            midx = arena_get_local_idx(arena, (uintptr_t)start, base, AL_HIGH);
            block = allocator_alloc_arena_mid(alloc, arena, range, midx, &result);
        }
        else
        {
            block = allocator_alloc_arena_high(alloc, arena, range, &result);
        }
        
        Heap* current_source = (Heap*)allocator_arena_get_at(&result);
        if(current_source != start)
        {
            list_remove(queues[start_level_idx], start);
            list_enqueue(queues[start_level_idx], current_source);
        }
    }
    else
    {
        Arena* new_arena = allocator_alloc_arena(alloc, size);
        block = allocator_alloc_arena_high(alloc, new_arena, range, &result);
        Queue *q = &alloc->part_alloc->aligned_z_cls[align_idx];
        list_enqueue(q, (Heap*)allocator_arena_get_at(&result));
    }
    
    if(block != NULL)
    {
        
        midx = result.midx;
        bidx = result.bidx;
        //
        // add blocks to alignment lists.
        //  are they zero blocks or not.
        //      if the grand parent is not full, just add that to the alignment lists.
        //      if the parent is not full add that to the alignement lists.
        //      if the bottom arena is the only thing that is not full, add that to the alignment lists.
        // add arena to cache struct.
        //
        return block;
    }
    else
    {
        return NULL;
    }
}

void* allocator_malloc_lt2k(Allocator* alloc, const size_t size)
{
    uint8_t pc = size_to_pool(size);
    void* res = allocator_malloc_pool_find_fit(alloc, pc);
    if(res == NULL)
    {
        res = allocator_malloc_arena_find_fit(alloc, sizeof(void*), size);
        if(res != NULL)
        {
            return res;
        }
    }
    
    uint8_t arena_idx = pc >> 3;
    const arena_size_table *stable = arena_get_size_table_by_idx(arena_idx);

    // get size of alignment
    Pool* new_pool = allocator_malloc_aligned_lt32m(alloc, stable->sizes[2], sizeof(void*), false);
    if (new_pool == NULL) {
        return NULL;
    }
    pool_init(new_pool, 0, pc, (uint32_t)stable->sizes[2], size);
    res = pool_get_free_block(new_pool);
    allocator_set_cached_pool(alloc, new_pool, true);
    return res;
}

void* allocator_malloc_base(Allocator* alloc, const size_t size, const size_t alignment, const bool zero)
{
    //
    // first we find an area that services a type of size.
    // then we look at the alignment requirements.
    // because larger number of two are multiples of lower multiples.
    // an alignment of 32, will also be true on 64, 128, ... and up.
    // the only thing is in the higher arena, is that you mighe have to
    // require an allocation to be of certain sections.
    // (4m,x64) (8m,32) (16,16) (32,8) (64,4) (128,2)
    // likeweise for when allocating from a partition.
    // (256m,x64), (512m,x32), (1m,x16), (2,8), (4,4), (8,2), (16,1)
    //
    
    //
    // 0 - 5, Arena
    // 6 - largest arena
    // 7 - areas. from largest arena
    // 8 - high address slabs.
    //
    
    if((size < (1 << 11)) && (alignment == sizeof(void*)) && !zero) // 8 <= n < 2k
    {
        // the common allocation path.
        // no alignment requirements.
        // no zero requirements.
        // and smaller than 2k in size.
        return allocator_malloc_lt2k(alloc, size);
    }
    else
    {
        if(size < (1 << 25)) // 2k <= n < 32m
        {
            return allocator_malloc_aligned_lt32m(alloc, size, alignment, zero);
        }
        else if(size < (1 << 27)) // 32m <= n < 256m
        {
            return allocator_malloc_arena_find_fit(alloc, size, alignment);
        }
        else if(size < (1ULL << 33)) // 256m <= n < 8g
        {
            return allocator_alloc_slab(alloc, size);
        }
        else // 8g <= n < inf
        {
            //
            // allocate from 32T and up directly from the OS.
            //
            
            //
            // 16 - inf.
            // high area address from OS.
            //
            
            //
            // 1, 2,  4,  8, 16,  32, 64,
            // 4, 8, 16, 32, 64, 128, 256
            // 4*64
            // 256, 512, 1G, 2G, 4G, 8G, 16G
            // 1    1.256, 1.
            //
            
            // what are safe addresses
            // 256 - 512  1.256 - 1.512
            // 512 - 1024 1.512 - 2
            // 1024 - 2048 2 - 3
            //             3 - 5
            //             5 - 9
            //             9 - 15
            //            15 - 31
            //            31 - 32 // reserved
            //            32 - inf // high address large allocations. 16G and up. rediculous requests.
            
            //
            // largest area/arena. 256,512,1G. 64, 32, 16 arenas.
            //
            
            //
            // 32 bits.
            // 1 Arena per thread.
            // 16 partition_sets
            //
            
            // 508 megs per partition_set.
            // 2*16 =   32
            // 4*16 =   64
            // 8*16 =  128
            // 16*16 =  256
            // 32*16 =  512M
            // 64*16 = 1G
            // 128*16 = 2G
            
            // 256, 512,
            //
            return NULL;
        }
    }
}

static inline Pool *allocator_alloc_pool(Allocator *a, const uint32_t idx, const uint32_t s, Section *prev_section)
{
    Section *sfree_section = NULL;
    if(prev_section != NULL && !section_is_full(prev_section))
    {
        sfree_section = prev_section;
    }
    else
    {
        sfree_section = allocator_get_free_section(a, s, get_pool_size_class(s));
        if (sfree_section == NULL) {
            return NULL;
        }
    }

    const unsigned int coll_idx = section_reserve_next(sfree_section);
    int32_t psize = (1 << size_clss_to_exponent[sfree_section->type]);
    Pool *p = (Pool *)section_get_collection(sfree_section, coll_idx, psize);
    pool_init(p, coll_idx, idx, psize, s);
    section_claim_idx(sfree_section, coll_idx);
    return p;
}

static inline void *allocator_alloc_from_pool_list(Allocator *a, const size_t s, const int32_t pool_idx, Section *prev_section)
{
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
        start = allocator_alloc_pool(a, pool_idx, (uint32_t)s, prev_section);
        if (start == NULL) {
            return NULL;
        }
        res = pool_get_free_block(start);
    }
    allocator_set_cached_pool(a, start, true);
    return res;
}

void *allocator_alloc_from_pool(Allocator *a, const size_t s)
{
    const int32_t pool_idx = size_to_pool(s);
    return allocator_alloc_from_pool_list(a, s, pool_idx, NULL);
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
            if(a->c_deferred.num > 255)
            {
                int bb = 0;
            }
        }
    }
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
    return allocator_malloc_base(a, as, as, false);
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
            if(!pool_is_empty(p))
            {
                return pool_aquire_block(p);
            }
            allocator_release_cached_pool(a);
            deferred_cache_release(a, NULL);
            Section *section = (Section *)((uintptr_t)p & ~(SECTION_SIZE - 1));
            return allocator_alloc_from_pool_list(a, p->block_size, p->block_idx, section);
        }
    }
    return NULL;
}
void *allocator_malloc(Allocator *a, size_t s)
{
    // load memory struct
    // get memory
    //
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
                ImplicitList *heap = (ImplicitList *)((uint8_t *)section + sizeof(Section));
                return implicitList_get_block_size(heap, p);
            }
        }
        case CT_HEAP: {
            ImplicitList *heap = (ImplicitList *)((uintptr_t)area + sizeof(Area));
            return implicitList_get_block_size(heap, p);
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
