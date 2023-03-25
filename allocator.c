

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
extern uintptr_t main_thread_id;
extern Allocator *main_instance;
int8_t reserve_any_partition_set(void)
{
    spinlock_lock(&partition_lock);
    int32_t reserved_id = -1;
    for (int i = 0; i < MAX_THREADS; i++) {
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
    for (int i = 0; i < MAX_THREADS; i++) {
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
        Allocator *new_alloc = (Allocator *)ALIGN_DOWN_2((uintptr_t)part_alloc, DEFAULT_OS_PAGE_SIZE);
        allocator_list[idx] = new_alloc;
    }
    return allocator_list[idx];
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

void* allocator_slot_ilist_alloc(Allocator*a,  const size_t as)
{
    return implicitList_get_block((ImplicitList*)a->c_slot.header, (uint32_t)as);
}

void* allocator_slot_slab_alloc(Allocator*a,  const size_t as)
{
    return (void *)((uintptr_t)a->c_slot.header + sizeof(Area));
}

internal_alloc allocator_set_heap_slot(Allocator *a, ImplicitList *p)
{
    if(p == NULL)
    {
        return allocator_slot_alloc_null;
    }
    a->c_slot.header = (uintptr_t)p;
    a->c_slot.block_size = 0;
    a->c_slot.offset = 0;
    a->c_slot.end = 0;
    implicitList_reserve(p);

    return allocator_slot_ilist_alloc;
}

internal_alloc allocator_set_slab_slot(Allocator *a, Area *p)
{
    if(p == NULL)
    {
        return allocator_slot_alloc_null;
    }
    a->c_slot.header = (uintptr_t)p;
    a->c_slot.offset = 0;
    a->c_slot.end = 0;
    
    area_reserve_all(p);
    area_set_container_type(p, CT_SLAB);
    return allocator_slot_slab_alloc;
}

internal_alloc allocator_set_pool_slot(Allocator *a, Pool *p)
{
    if(p == NULL)
    {
        return allocator_slot_alloc_null;
    }
    a->c_slot.header = (uintptr_t)p | SLOT_POOL;
    a->c_slot.block_size = p->block_size;
    a->c_slot.alignment = p->alignment;
    a->c_slot.req_size = p->block_size;
    a->c_slot.pheader = 0;
    a->c_slot.pend = 0;
    int32_t rem_blocks = p->num_available - p->num_committed;
    pool_post_reserved(p, a);
    
    if(rem_blocks > 0)
    {
        a->c_slot.offset = (int32_t)((uintptr_t)pool_base_address(p) - (uintptr_t)p);
        a->c_slot.start = a->c_slot.offset;
        a->c_slot.end = (int32_t)((uintptr_t)pool_base_address(p) - (uintptr_t)p) + (p->num_available * p->block_size);
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

internal_alloc allocator_set_arena_slot(Allocator *a, void *p)
{
    uintptr_t start_addr = (uintptr_t)p;
    if (start_addr == a->c_slot.header) {
        return allocator_slot_alloc_null;
    }
    
    uint8_t pid = partition_id_from_addr((uintptr_t)p);
    size_t asize = area_size_from_partition_id(pid);
    /*
    Arena_L2* al2 =  (Arena_L2*)ALIGN_DOWN_2(p, asize);
    Arena *h = (Arena *)((uintptr_t)al2 + sizeof(Arena_L2));
    
    a->c_slot.header = start_addr | SLOT_ARENA;
    const arena_size_table *stable = arena_get_size_table(h);

    for (int i = 2; i >= 0; i--) {
        if (((uintptr_t)p & (stable->sizes[i] - 1)) == 0) {
            // aligned to base level
            uintptr_t pc = ((uintptr_t)p & ~(stable->sizes[i + 1] - 1));
            switch (i) {
            case 0:
                {
                    Arena_L0* al0 = (Arena_L0*)pc;
                    a->c_slot.alloc_range_mask = al0->L0_ranges;
                    a->c_slot.alloc_mask = al0->L0_allocations;
                    a->c_slot.block_size = (int32_t)stable->sizes[0];
                    a->c_slot.end = (int32_t)stable->sizes[1];
                }
                break;
            case 1:
                {
                    Arena_L1* al1 = (Arena_L1*)pc;
                    a->c_slot.alloc_range_mask = al1->L1_ranges;
                    a->c_slot.alloc_mask = al1->L1_allocations;
                    a->c_slot.block_size = (int32_t)stable->sizes[1];
                    a->c_slot.end = (int32_t)stable->sizes[2];

                }
                break;
            default:
                {
                    a->c_slot.alloc_range_mask = al2->L2_ranges;
                    a->c_slot.alloc_mask = al2->L2_allocations;
                    a->c_slot.block_size = (int32_t)stable->sizes[2];
                    a->c_slot.end = (int32_t)stable->sizes[3];
                }
                break;
            };

            break;
        }
    }
    */
    a->c_slot.alignment = a->c_slot.block_size;
    a->c_slot.req_size = a->c_slot.block_size;
    
    int32_t max_zeros = num_consecutive_zeros(a->c_slot.alloc_mask);
    int32_t offset = find_first_nzeros(a->c_slot.alloc_mask, max_zeros, 0);
    a->c_slot.offset = offset*a->c_slot.block_size;
    a->c_slot.end = a->c_slot.offset + max_zeros*a->c_slot.block_size;
    
    return allocator_slot_alloc;
}

static inline void allocator_release_pool_slot(Allocator *a)
{
    Pool* p = (Pool*)(a->c_slot.header & ~0x3);
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
                Queue *queue = &a->part_alloc->pools[p->block_idx];
                if(pool_is_connected(p) || queue->head == p)
                {
                    list_remove(queue, p);
                }
            }
            else
            {
                Queue *queue = &a->part_alloc->pools[p->block_idx];
                if(!pool_is_connected(p) && queue->head != p)
                {
                    list_enqueue(queue, p);
                }
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
    //Arena* p = (Arena*)(a->c_slot.header & ~0x3);

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
            default:
                break;
        }
    }
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
        pool_free_block(pool, p, a);
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

internal_alloc allocator_load_heap_slot(Allocator *a, const size_t s)
{
    const uint32_t heap_sizes[] = {1 << HT_4M, 1 << HT_32M, 1 << HT_64M, 1 << HT_128M, 1 << HT_256M};
    const uint32_t heap_size_cls = size_to_implicitList(s);
    Queue *queue = &a->part_alloc->heaps[heap_size_cls];
    ImplicitList *start = (ImplicitList *)queue->head;
    while (start != NULL) {
        ImplicitList *next = start->next;
        if (implicitList_has_room(start, s)) {
            return allocator_set_heap_slot(a, start);
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
            return allocator_set_heap_slot(a, start);
        }
    }

    AreaType at = get_area_type_for_heap(s);
    int32_t area_idx = -1;
    Partition* partition = partition_allocator_get_free_area(a->part_alloc, at, area_type_to_size[at], &area_idx, false);
    if(partition == NULL)
    {
        return allocator_slot_alloc_null;
    }
    Area* new_area = partition_allocator_area_at_idx(a->part_alloc, partition, area_idx);
    if (new_area == NULL) {
        return allocator_slot_alloc_null;
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
    return allocator_set_heap_slot(a, start);
}

internal_alloc allocator_load_slab_slot(Allocator *a, const size_t s)
{
    const size_t totalSize = sizeof(Area) + s;
    int32_t area_idx = -1;
    Partition *partition = partition_allocator_get_free_area(a->part_alloc, AT_FIXED_256, totalSize, &area_idx, false);
    if (partition == NULL) {
        return allocator_slot_alloc_null;
    }
    Area *area = partition_allocator_area_at_idx(a->part_alloc, partition, area_idx);
    if((partition->zero_mask & 1ULL << area_idx) == 0)
    {
        area_init(area, a->idx, AT_FIXED_256);
        partition->zero_mask |= (1ULL << area_idx);
        partition->area_mask |= (1ULL << area_idx);
        partition->full_mask |= (1ULL << area_idx);
    }
    return allocator_set_slab_slot(a, area);
}

void *allocator_alloc_slab(Allocator *a, const size_t s)
{
    const size_t totalSize = sizeof(Area) + s;
    int32_t area_idx = -1;
    Partition *partition = partition_allocator_get_free_area(a->part_alloc, AT_FIXED_256, totalSize, &area_idx, false);
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

void * allocator_alloc_arena(Allocator* alloc, int64_t partition_idx, bool zero)
{
    if (partition_idx < 0) {
        return NULL;
    }
    int32_t area_idx = -1;
    Partition* partition = partition_allocator_get_free_area(alloc->part_alloc, partition_idx, area_type_to_size[partition_idx], &area_idx, zero);
    if(partition == NULL)
    {
        return NULL;
    }
    void* arena = partition_allocator_area_at_idx(alloc->part_alloc, partition, area_idx);
    Arena *header = (Arena *)arena;
    AreaType at = partition_allocator_get_partition_idx(alloc->part_alloc, partition);
    
    if((partition->zero_mask & 1ULL << area_idx) == 0)
    {
        partition->zero_mask |= (1ULL << area_idx);
        header->container_exponent = area_type_to_exponent[at];
        header->partition_id = (uint32_t)alloc->part_alloc->idx;
        header->allocations = 1;
        header->ranges = 0;
        header->zero = 0;
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


internal_alloc allocator_malloc_pool_find_fit(Allocator* alloc, const uint32_t pc)
{
    // check if there are any pools available.
    Queue *queue = &alloc->part_alloc->pools[pc];
    Heap* start = queue->head;
    if(start != NULL)
    {
        list_remove(queue, start);
        return allocator_set_pool_slot(alloc, (Pool *)start);
    }
    return allocator_slot_alloc_null;
}


/*
int32_t allocator_find_arena(Allocator* alloc, ArenaAllocation *result, bool zero)
{
    size_t base_size = result->block_size;
    int32_t block_count = result->num_blocks;
    int32_t block_offset = result->block_exp;
    
    int32_t lz = 63UL - __builtin_clzll(base_size);
    int32_t base_offset = lz - 4;
    
    int32_t bcp = 32 - __builtin_clz(block_count);
    bcp = (1 << (bcp + 1));
    
    int32_t level_idx = base_offset / 6;
    int32_t arena_idx = base_offset % 6;
    
    int64_t partition_idx = size_to_arena(base_size);
    const arena_size_table *stable = arena_get_size_table_by_idx(partition_idx);
    
}

int32_t allocator_get_arena(Allocator* alloc, size_t size, const size_t alignment, bool zero, ArenaAllocation* result)
{

    
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
            result->block_size = (int32_t)base_size;
            result->num_blocks = (int32_t)asize;
            result->block_exp = 0;
            if(allocator_find_arena(alloc, result, zero))
            {
                return 1;
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
                result->block_size = (int32_t)base_size;
                result->num_blocks = (int32_t)asize;
                result->block_exp = (int32_t)block_offset < 2? 0:(int32_t)block_offset;
                if(allocator_find_arena(alloc, result, zero))
                {
                    return 1;
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
        result->block_size = (int32_t)asize;
        result->num_blocks = 1;
        result->block_exp = 0;

        if(allocator_find_arena(alloc, result, zero))
        {
            return 1;
        }
    }
    return 0;
}
*/

internal_alloc allocator_malloc_leq_32k(Allocator* alloc, const size_t size, const size_t alignment, const bool zero)
{
    int32_t row_map[] = {0,0,0,0,0,0,5,5,5,5};
    uint8_t pc = size_to_pool(size);
    internal_alloc res = allocator_malloc_pool_find_fit(alloc, pc);
    if(res == allocator_slot_alloc_null)
    {
        int32_t row = pc/8;
        
        uint8_t arena_idx = row_map[row];
        Heap* start = allocator_alloc_arena(alloc, arena_idx, zero);
        if(start != NULL)
        {
            Arena* arena = (Arena*)start;
            int32_t midx = find_first_nzeros(arena->allocations, 1, 0);
            
            int8_t pid = partition_id_from_addr((uintptr_t)arena);
            size_t area_size = area_size_from_partition_id(pid);
            size_t block_size = area_size >> 6;
            
            Pool* new_pool = (Pool*)((uintptr_t)arena + (midx * block_size));
            pool_init(new_pool, midx, pc, (uint32_t)block_size);
            res = allocator_set_pool_slot(alloc, new_pool);
        }
    }
    
    return res;
}

internal_alloc allocator_malloc_base(Allocator* alloc, size_t size, size_t alignment, const bool zero)
{
    internal_alloc res = allocator_slot_alloc_null;
    // if not zero, we align to the next multiple of wordwidth
    size = ALIGN(size);
    // alignment needs to be a power of 2.
    // the size needs to be a multiple of the alignment.
    if(!POWER_OF_TWO(alignment))
    {
        return res;
    }
    
    if(size < alignment)
    {
        size = alignment;
    }
    
    if(size <= (1 << 15)) // 8 <= n <= 32k 27
    {
        // map size to pool size.
        // <= 2k just use the previous mapping rules.
        // <= 32k just map to 256m arena, 4m block.
        // < 4m just map to arenas. 64k - 4m blocks.
        // < 256m allocate block ranges from arenas.
        // < 8gb allocate range of areas.
        
        res = allocator_malloc_leq_32k(alloc, size, alignment, zero);
    }
    else
    {
        // these large allocation do not go through this path
        if(size < (1ULL << 24)) // 32k < n <= 16m
        {
            // allocate a single arena block
        }
        else if(size < (1ULL << 28)) // 16m < n <= 256m
        {
            // allocate multiple arena blocks.
        }
        else if(size < (1ULL << 33)) // 256m < n < 8g
        {
            res = allocator_load_slab_slot(alloc, size);
        }
        else
        {
            res = allocator_slot_alloc_null;
        }
    }
    return res;
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
    pool_init(p, coll_idx, idx, psize);
    section_claim_idx(sfree_section, coll_idx);
    return p;
}

static inline void *allocator_fetch_pool(Allocator *a, const size_t s, const int32_t pool_idx, Section *prev_section)
{
    Queue *queue = &a->part_alloc->pools[pool_idx];
    Pool *start = queue->head;
    if (start != NULL) {
        // there are no empty pools in the queue.
        list_remove(queue, start);
    }
    else
    {
        start = allocator_alloc_pool(a, pool_idx, (uint32_t)s, prev_section);
        if (start == NULL) {
            return NULL;
        }
    }
    return start;
}

static inline void *allocator_alloc_from_pool_list(Allocator *a, const size_t s, const int32_t pool_idx, Section *prev_section)
{
    Pool* new_pool = allocator_fetch_pool(a, s, pool_idx, prev_section);
    return allocator_set_pool_slot(a, new_pool);
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
    int8_t pid = partition_id_from_addr((uintptr_t)p);
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
        allocator_release_slot(a);
        deferred_init(a, p);
    }
    else
    {
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

internal_alloc allocator_load_pool_slot(Allocator* a, size_t s)
{
    
    deferred_release(a, NULL);
    a->prev_size = (uint32_t)s;
    const size_t as = ALIGN(s);
    //
    const int32_t pool_idx = size_to_pool(as);
    Pool* new_pool = allocator_fetch_pool(a, as, pool_idx, NULL);
    if(new_pool == NULL)
    {
        return allocator_slot_alloc_null;
    }
    return allocator_set_pool_slot(a, new_pool);
}

static inline internal_alloc allocator_load_memory_slot(Allocator *a, size_t alignemt, size_t as, bool zero)
{
    allocator_release_slot(a);
#ifdef ARENA_PATH
    deferred_release(a, NULL);
    a->prev_size = (uint32_t)as;
    return allocator_malloc_base(a, ALIGN(as), alignemt, zero);
#else
    
    if (as <= LARGE_OBJECT_SIZE) {
        return allocator_load_pool_slot(a, as);
    } else if (as <= AREA_SIZE_LARGE) {
        // allocate form the large page
        return allocator_load_heap_slot(a, as);
    } else {
        return allocator_load_slab_slot(a, as);
    }
    
#endif
}

static inline internal_alloc allocator_malloc_fallback(Allocator *a, size_t alignment, size_t as, bool zero)
{
    // reset caching structs
    a->c_slot.header = 0;
    // try again by fetching a new partition set to use
    const int8_t new_partition_set_idx = reserve_any_partition_set_for((int32_t)a->idx);
    if (new_partition_set_idx != -1) {
        // flush our threaded pools.
        allocator_flush_thread_free(a);
        // move our default partition allocator to the new slot and try again.
        PartitionAllocator *part_alloc = partition_allocator_aquire(new_partition_set_idx);
        list_enqueue(&a->partition_allocators, part_alloc);
        a->part_alloc = part_alloc;
        return allocator_load_memory_slot(a, as, alignment, zero);
    }
    // we are out of options and the current thread can't get more memory
    return allocator_slot_alloc_null;
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
            // new size is smaller then the previous size.
            // what if the min size is smaller than the cache alignment supplies.
            // lets see if the size is small enough to justify creating a counter
            // alloc within the current slot
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
                if(POWER_OF_TWO(a->c_slot.block_size))
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
        
        }
    }
    
    return NULL;
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
        a->c_slot.offset += a->c_slot.req_size;
        if(a->c_slot.offset <= a->c_slot.end)
        {
            return res;
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
        // we will attemp only once to get a new partition set
        // if our current one is failing us.
        // if this fails, we have exhausted all options.
        ialloc = allocator_malloc_fallback(a, s, align, zero);
    }
    // commit our memory slot and return the address
    // the null allocator slot, just returns NULL.
    return ialloc(a, s);
}


bool allocator_release_local_areas(Allocator *a)
{
    allocator_release_slot(a);
    deferred_release(a, NULL);
    bool result = false;
    PartitionAllocator *palloc = a->partition_allocators.head;
    while (palloc != NULL) {
        PartitionAllocator *next = palloc->next;
        allocator_thread_dequeue_all(a, palloc->thread_free_queue);
        partition_allocator_release_deferred(palloc, a);
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
        if (partition_id_from_addr((uintptr_t)p) >= 0) {
            // it is within our address ranges.
            size_t area_size = area_size_from_addr((uintptr_t)p);
            Area *area = (Area *)((uintptr_t)p & ~(area_size - 1));
            const uint32_t part_id = area_get_id(area);
            if (part_id < MAX_THREADS) {
                // well we are able to read the contents of this memory address.
                // get the idx of the area within its partition.
                if (reserve_partition_set(part_id, MAX_THREADS)) {
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
    int8_t pid = partition_id_from_addr((uintptr_t)p);
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
