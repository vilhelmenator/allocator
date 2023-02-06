
#include "partition_allocator.h"
#include "os.h"
#include "pool.h"
#include "heap.h"

cache_align PartitionAllocator *partition_allocators[MAX_THREADS];
cache_align uint8_t* default_allocator_buffer = 0;
typedef void (*free_func)(void *);

PartitionAllocator *partition_allocator_init(size_t idx, uintptr_t thr_mem)
{
    // partition owners
    // the allocator is at 4k alignment
    Allocator *alloc = (Allocator *)thr_mem;
    alloc->idx = (int32_t)idx;
    alloc->prev_size = 0;
    thr_mem = ALIGN_CACHE(thr_mem + sizeof(Allocator));
    // next come the partition allocator structs.
    Queue *pool_queue = (Queue *)thr_mem;
    thr_mem = ALIGN_CACHE(thr_mem + sizeof(Queue) * POOL_BIN_COUNT);
    Queue *heap_queue = (Queue *)thr_mem;
    thr_mem = ALIGN_CACHE(thr_mem + sizeof(Queue) * HEAP_TYPE_COUNT);
    Queue *section_queue = (Queue *)thr_mem;
    thr_mem += CACHE_LINE;
    
    Queue *aligned_z = (Queue *)thr_mem;
    thr_mem = ALIGN_CACHE(thr_mem + sizeof(Queue) * ARENA_BIN_COUNT);
    Queue *aligned = (Queue *)thr_mem;
    thr_mem = ALIGN_CACHE(thr_mem + sizeof(Queue) * ARENA_BIN_COUNT);
    
    AtomicQueue *mqueue = (AtomicQueue *)thr_mem;
    mqueue->head = (uintptr_t)thr_mem;
    mqueue->tail = (uintptr_t)thr_mem;
    thr_mem = (uintptr_t)alloc + DEFAULT_OS_PAGE_SIZE;
    thr_mem -= ALIGN_CACHE(sizeof(PartitionAllocator));
    

    PartitionAllocator *palloc = (PartitionAllocator *)thr_mem;
    palloc->idx = idx;
    palloc->previous_partitions = UINT64_MAX;
    palloc->sections = section_queue;
    palloc->heaps = heap_queue;
    palloc->pools = pool_queue;
    palloc->aligned_cls = aligned;
    palloc->aligned_z_cls = aligned_z;
    palloc->thread_messages = NULL;
    palloc->thread_free_queue = mqueue;

    return palloc;
}

PartitionAllocator* partition_allocator_init_default(void)
{
    default_allocator_buffer = alloc_memory((void *)ADDR_START, MAX_THREADS*PARTITION_ALLOCATOR_BASE_SIZE, true);
    PartitionAllocator *part_alloc = partition_allocator_init(0, (uintptr_t)default_allocator_buffer);
    partition_allocators[0] = part_alloc;
    return part_alloc;
}

PartitionAllocator *partition_allocator_aquire(size_t idx)
{
    if (partition_allocators[idx] == NULL) {
        uintptr_t thread_mem = (uintptr_t)default_allocator_buffer + PARTITION_ALLOCATOR_BASE_SIZE*idx;
        PartitionAllocator *part_alloc = partition_allocator_init(idx, thread_mem);
        partition_allocators[idx] = part_alloc;
    }
    return partition_allocators[idx];
}

AtomicMessage *partition_allocator_get_last_message(PartitionAllocator *pa)
{
    AtomicMessage *msg = pa->thread_messages;
    if (msg == NULL) {
        return NULL;
    }
    while ((uintptr_t)msg->next != 0) {
        msg = (AtomicMessage *)(uintptr_t)msg->next;
    }
    return msg;
}

void partition_allocator_thread_free(PartitionAllocator *pa, void *p)
{
    AtomicMessage *new_free = (AtomicMessage *)p;
    new_free->next = (uintptr_t)pa->thread_messages;
    pa->thread_messages = new_free;
    pa->message_count++;
}

int32_t partition_allocator_get_next_area(PartitionAllocator *pa, Partition *area_queue, uint64_t size, uint64_t alignment)
{
    AreaType at = (AreaType)partition_allocator_get_partition_idx(pa, area_queue);
    size_t base_size = BASE_AREA_SIZE * 64 << (uint64_t)at;
    size_t offset = BASE_ADDR(at);
    size_t start_addr = (pa->idx)*base_size + offset;
    size_t end_addr = start_addr + base_size;
    
    size_t type_exponent = area_type_to_exponent[at];
    size_t type_size = area_type_to_size[at];
    uint32_t range = (uint32_t)(size >> type_exponent);
    range += (size & (1 << type_exponent) - 1) ? 1 : 0;
    size = type_size * range;
    int32_t idx = find_first_nzeros(area_queue->area_mask, range, 0);
    if (idx == -1) {
        return -1; // no room.
    }
    uint64_t new_mask = (1ULL << range) - 1ULL;
    area_queue->area_mask |= (new_mask << idx);
    area_queue->range_mask |= apply_range(range, idx);
    uintptr_t aligned_addr = start_addr + (type_size * idx);
    alloc_memory_aligned((void *)aligned_addr, end_addr, size, alignment);
    return idx;
}

bool partition_allocator_try_release_containers(PartitionAllocator *pa, Area *area)
{
    if (area_is_free(area)) {

        // all sections should be free and very likely in the free sections
        // list.
        const int num_sections = area_get_section_count(area);
        const ContainerType root_ctype = area_get_container_type(area);
        if (root_ctype == CT_HEAP) {
            ImplicitList *heap = (ImplicitList *)((uintptr_t)area + sizeof(Area));
            uint32_t heapIdx = (area_get_type(area));
            Queue *queue = &pa->heaps[heapIdx];
            list_remove(queue, heap);
            return true;
        }

        for (int i = 0; i < num_sections; i++) {
            Section *section = (Section *)((uint8_t *)area + SECTION_SIZE * i);

            if (!area_is_claimed(area, i)) {
                continue;
            }
            int num_collections = section_get_collection_count(section);
            const SectionType st = section->type;
            int32_t psize = (1 << size_clss_to_exponent[st]);
            for (int j = 0; j < num_collections; j++) {
                if (!section_is_claimed(section, j)) {
                    continue;
                }
                void *collection = (void *)section_get_collection(section, j, psize);
                if (st != ST_HEAP_4M) {
                    Pool *pool = (Pool *)collection;
                    Queue *queue = &pa->pools[pool->block_idx];
                    list_remove(queue, pool);
                } else {
                    ImplicitList *heap = (ImplicitList *)collection;
                    Queue *queue = &pa->heaps[0];
                    list_remove(queue, heap);
                }
            }

            list_remove(pa->sections, section);
        }
        return true;
    }
    return false;
}

void partition_allocator_free_area_from_list(PartitionAllocator *pa, Area *a, Partition *list, size_t idx)
{
    AreaType at = (AreaType)partition_allocator_get_partition_idx(pa, list);
    uint64_t range = get_range((uint32_t)idx, list->range_mask);
    uint64_t new_mask = ((1ULL << range) - 1UL) << idx;
    list->area_mask = list->area_mask & ~new_mask;
    list->range_mask = list->range_mask & ~new_mask;
    list->zero_mask = list->zero_mask & ~new_mask;
    list->full_mask = list->full_mask & ~new_mask;
    int8_t previous_area = ((int8_t*)&pa->previous_partitions)[at];
    if ((idx == previous_area) || (list->area_mask == 0)) {
        ((int8_t*)&pa->previous_partitions)[at] = -1;
    }
    free_memory(a, area_get_size(a)*range);
}

static inline Partition *partition_allocator_get_area_list(PartitionAllocator *pa, Area *area)
{
    const AreaType at = area_get_type(area);
    return &pa->area[at];
}

void partition_allocator_free_area(PartitionAllocator *pa, Area *area)
{
    Partition *queue = partition_allocator_get_area_list(pa, area);
    int32_t idx = partition_allocator_get_area_idx_from_queue(pa, area, queue);
    partition_allocator_free_area_from_list(pa, area, queue, idx);
}

bool partition_allocator_try_free_area(PartitionAllocator *pa, Area *area, Partition *list)
{
    if (area_is_free(area)) {

        int32_t idx = partition_allocator_get_area_idx_from_queue(pa, area, list);
        partition_allocator_free_area_from_list(pa, area, list, idx);
        return true;
    }
    return false;
}

static inline int32_t area_list_get_next_area_idx(Partition *queue, uint32_t cidx)
{
    return get_next_mask_idx(queue->area_mask, cidx);
}

Area *area_list_get_area(PartitionAllocator* pa, Partition *queue, uint32_t cidx)
{
    AreaType at = (AreaType)partition_allocator_get_partition_idx(pa, queue);
    size_t base_size = BASE_AREA_SIZE * 64 << (uint64_t)at;
    size_t offset = BASE_ADDR(at);
    size_t start_addr = (pa->idx)*base_size + offset;
    uintptr_t area_addr = start_addr + ((1 << at) * BASE_AREA_SIZE) * cidx;
    return (Area *)area_addr;
}

bool partition_allocator_release_areas_from_queue(PartitionAllocator *pa, Partition *queue)
{
    bool was_released = false;
    int32_t area_idx = area_list_get_next_area_idx(queue, 0);
    // find free section.
    // detach all pools/pages/sections.
    while (area_idx != -1) {
        Area *start = area_list_get_area(pa, queue, area_idx);
        was_released |= !partition_allocator_try_release_containers(pa, start);
        area_idx = area_list_get_next_area_idx(queue, area_idx + 1);
    }
    area_idx = area_list_get_next_area_idx(queue, 0);
    while (area_idx != -1) {
        Area *start = area_list_get_area(pa, queue, area_idx);
        was_released |= !partition_allocator_try_free_area(pa, start, queue);
        area_idx = area_list_get_next_area_idx(queue, area_idx + 1);
    }
    return !was_released;
}

bool partition_allocator_release_single_area_from_queue(PartitionAllocator *pa, Partition *queue)
{
    int32_t area_idx = area_list_get_next_area_idx(queue, 0);
    // find free section.
    // detach all pools/pages/sections.
    while (area_idx != -1) {
        Area *start = area_list_get_area(pa, queue, area_idx);
        if (partition_allocator_try_release_containers(pa, start)) {
            return partition_allocator_try_free_area(pa, start, queue);
        }
        area_idx = area_list_get_next_area_idx(queue, area_idx + 1);
    }
    return false;
}

bool partition_allocator_release_local_areas(PartitionAllocator *pa)
{
    bool was_released = false;
    for (size_t i = 0; i < 7; i++) {
        if (pa->area[i].area_mask != 0) {
            was_released |= !partition_allocator_release_areas_from_queue(pa, &pa->area[i]);
        }
    }
    return !was_released;
}
void partition_allocator_release_deferred(PartitionAllocator *pa)
{
    for (int j = 0; j < POOL_BIN_COUNT; j++) {
        Pool* start = pa->pools[j].head;
        while(start != NULL)
        {
            Pool* next = start->next;
            pool_move_deferred(start);
            if(start->num_used == 0)
            {
                pool_set_empty(start);
            }
            start = next;
        }
    }
    for (int j = 0; j < HEAP_TYPE_COUNT; j++) {
        ImplicitList* start = pa->heaps[j].head;
        while(start != NULL)
        {
            ImplicitList* next = start->next;
            implicitList_move_deferred(start);
            if(start->num_allocations == 0)
            {
                implicitList_freeAll(start);
            }
            start = next;
        }
    }
}

int8_t partition_allocator_get_free_area_from_queue(PartitionAllocator*pa, Partition *current_queue)
{
    // the areas are empty
    int8_t new_area = -1;
    //if((partition->free_mask & 1ULL << area_idx) == 0)
    uint32_t at = partition_allocator_get_partition_idx(pa, current_queue);
    Area *previous_area = NULL;
    int32_t previous_index = ((int8_t*)&pa->previous_partitions)[at];
    if (((int8_t*)&pa->previous_partitions)[at] != -1) {
        previous_area = partition_allocator_area_at_idx(pa, current_queue, previous_index);
        if((current_queue->full_mask & 1ULL << previous_index) == 0){
            new_area = previous_index;
        }
    }
    if (new_area == -1) {
        if ((current_queue->area_mask != 0) && (current_queue->full_mask != UINT64_MAX)) {
            int32_t area_idx = area_list_get_next_area_idx(current_queue, 0);
            while (area_idx != -1) {
                if((current_queue->full_mask & 1ULL << area_idx) == 0){
                    new_area = area_idx;
                    break;
                }
                area_idx = area_list_get_next_area_idx(current_queue, area_idx + 1);
            }
        }
    }
    return new_area;
}

static inline Partition *partition_allocator_get_current_queue(PartitionAllocator *pa, AreaType t, const size_t s, size_t *area_size,
                                                 size_t *alignement)
{
    if (t == AT_VARIABLE) {
        *area_size = s;
        *alignement = AREA_SIZE_HUGE;
        return &pa->area[AT_VARIABLE];
    } else {
        *area_size = area_type_to_size[t];
        *alignement = *area_size;
        return &pa->area[t];
    }
}

static inline Partition *partition_allocator_promote_area(PartitionAllocator *pa, AreaType *t, size_t *area_size, size_t *alignement)
{
    if (*t > AT_FIXED_128) {
        return NULL;
    } else {
        (*t)++;
        *area_size = area_type_to_size[*t];
        *alignement = *area_size;
        return &pa->area[*t];
    }
}

Partition *partition_allocator_get_free_area(PartitionAllocator *pa, size_t s, AreaType t, int32_t* new_area_idx)
{
    size_t area_size = AREA_SIZE_SMALL;
    size_t alignment = area_size;
    Partition *current_queue = partition_allocator_get_current_queue(pa, t, s, &area_size, &alignment);
    *new_area_idx = partition_allocator_get_free_area_from_queue(pa, current_queue);
    while (*new_area_idx == -1 && (UINT64_MAX == current_queue->area_mask)) {
        // try releasing an area first.
        bool was_released = partition_allocator_release_single_area_from_queue(pa, current_queue);
        if (was_released) {
            *new_area_idx = partition_allocator_get_free_area_from_queue(pa, current_queue);
            if (new_area_idx >= 0) {
                break;
            }
        }
        // try promotion
        current_queue = partition_allocator_promote_area(pa, &t, &area_size, &alignment);
        if (current_queue == NULL) {
            return NULL;
        }
        *new_area_idx = partition_allocator_get_free_area_from_queue(pa, current_queue);
    }

    if (*new_area_idx == -1) {
        if (s < os_page_size) {
            s = os_page_size;
        }
        
        *new_area_idx = partition_allocator_get_next_area(pa, current_queue, s, alignment);
        if (*new_area_idx == -1) {
            return NULL;
        }
    }
    AreaType at = (AreaType)partition_allocator_get_partition_idx(pa, current_queue);
    ((int8_t*)&pa->previous_partitions)[at] = *new_area_idx;
    return current_queue;
}

AreaType get_area_type_for_heap(const size_t size)
{
    AreaType at = AT_FIXED_32;
    if (size > MEDIUM_OBJECT_SIZE) {
        if (size <= LARGE_OBJECT_SIZE) {
            at = AT_FIXED_64;
        } else if (size <= HUGE_OBJECT_SIZE) {
            at = AT_FIXED_128;
        } else {
            at = AT_FIXED_256;
        }
    }
    return at;
}

Section *partition_allocator_alloc_section(PartitionAllocator *pa, const size_t size)
{
    int32_t area_idx = -1;
    Partition* partition = partition_allocator_get_free_area(pa, size, AT_FIXED_32, &area_idx);
    if (partition == NULL) {
        return NULL;
    }
    Area *new_area = partition_allocator_area_at_idx(pa, partition, area_idx);
    AreaType at = (AreaType)partition_allocator_get_partition_idx(pa, partition);
    if((partition->zero_mask & 1ULL << area_idx) == 0)
    {
        area_init(new_area, pa->idx, at);
        partition->zero_mask |= (1ULL << area_idx);
    }
    
    size_t area_size = area_type_to_size[at];
    if (area_size > AREA_SIZE_LARGE) {
        // sections are not supported in areas larger than 128 megs
        if (area_is_free(new_area)) {

            partition_allocator_free_area(pa, new_area);
        }
        return NULL;
    }
    const int32_t section_idx = area_claim_section(new_area);
    area_set_container_type(new_area, CT_SECTION);
    Section *section = (Section *)((uint8_t *)new_area + SECTION_SIZE * section_idx);
    section->constr_mask._w32[0] = 0;
    section->active_mask._w32[0] = 0;
    section->idx = section_idx;
    section->partition_mask = new_area->partition_mask;
    return section;
}


