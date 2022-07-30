
#include "partition_allocator.h"
#include "os.h"
#include "section.h"

PartitionAllocator **partition_allocators = NULL;

typedef void (*free_func)(void *);
PartitionAllocator *partition_allocator_init(size_t idx)
{
    // partition owners
    // message sentinels
    size_t parts[] = {ALIGN_CACHE(sizeof(Allocator)),
                      ALIGN_CACHE(sizeof(Queue) * (POOL_BIN_COUNT + HEAP_TYPE_COUNT + 1)),
                      ALIGN_CACHE(sizeof(message_queue)), ALIGN_CACHE(sizeof(PartitionAllocator))};
    size_t size = 0;
    for (int i = 0; i < sizeof(parts) / sizeof(size_t); i++) {
        size += parts[i];
    }
    uintptr_t thr_mem = (uintptr_t)cmalloc_os(size);
    // the allocator is at 4k alignment
    Allocator *alloc = (Allocator *)thr_mem;
    alloc->idx = (int32_t)idx;
    thr_mem = ALIGN_CACHE(thr_mem + sizeof(Allocator));
    // next come the partition allocator structs.
    Queue *pool_queue = (Queue *)thr_mem;
    thr_mem = ALIGN_CACHE(thr_mem + sizeof(Queue) * POOL_BIN_COUNT);
    Queue *heap_queue = (Queue *)thr_mem;
    thr_mem = ALIGN_CACHE(thr_mem + sizeof(Queue) * HEAP_TYPE_COUNT);
    Queue *section_queue = (Queue *)thr_mem;
    thr_mem += CACHE_LINE;
    message_queue *mqueue = (message_queue *)thr_mem;
    mqueue->head = (uintptr_t)thr_mem;
    mqueue->tail = (uintptr_t)thr_mem;
    thr_mem += CACHE_LINE;

    PartitionAllocator *palloc = (PartitionAllocator *)thr_mem;
    size = (SZ_GB * 2);
    size_t offset = ((size_t)2 << 40);
    uint32_t area_type = 0;
    for (size_t j = 0; j < 4; j++) {
        palloc->area[j].partition_id = (uint32_t)idx;
        palloc->area[j].start_addr = (idx)*size + offset;
        palloc->area[j].end_addr = palloc->area[j].start_addr + size;
        palloc->area[j].type = area_type;
        palloc->area[j].area_mask = 0;
        palloc->area[j].previous_area = NULL;
        size *= 2;
        offset *= 2;
        area_type++;
    }
    palloc->idx = idx;
    palloc->sections = section_queue;
    palloc->heaps = heap_queue;
    palloc->pools = pool_queue;
    palloc->thread_messages = NULL;
    palloc->message_count = 0;
    palloc->thread_free_queue = mqueue;

    return palloc;
}

PartitionAllocator *partition_allocator_aquire(size_t idx)
{
    if (partition_allocators[idx] == NULL) {
        PartitionAllocator *part_alloc = partition_allocator_init(idx);
        partition_allocators[idx] = part_alloc;
    }
    return partition_allocators[idx];
}

message *partition_allocator_get_last_message(PartitionAllocator *pa)
{
    message *msg = pa->thread_messages;
    if (msg == NULL) {
        return NULL;
    }
    while ((uintptr_t)msg->next != 0) {
        msg = (message *)(uintptr_t)msg->next;
    }
    return msg;
}

void partition_allocator_thread_free(PartitionAllocator *pa, void *p)
{
    message *new_free = (message *)p;
    new_free->next = (uintptr_t)pa->thread_messages;
    pa->thread_messages = new_free;
    pa->message_count++;
}

Area *partition_allocator_get_next_area(Partition *area_queue, uint64_t size, uint64_t alignment)
{
    size_t type_exponent = area_type_to_exponent[area_queue->type];
    size_t type_size = area_type_to_size[area_queue->type];
    size_t range = size >> type_exponent;
    range += (size & (1 << type_exponent) - 1) ? 1 : 0;
    size = type_size * range;
    int32_t idx = find_first_nzeros(area_queue->area_mask, range);
    if (idx == -1) {
        return NULL; // no room.
    }
    uint64_t new_mask = (1UL << range) - 1UL;
    area_queue->area_mask |= (new_mask << idx);
    idx = 63 - idx;
    uintptr_t aligned_addr = area_queue->start_addr + (type_size * idx);

    Area *new_area = (Area *)alloc_memory_aligned((void *)aligned_addr, area_queue->end_addr, size, alignment);
    if (new_area == NULL) {
        return NULL;
    }
    area_init(new_area, area_queue->partition_id, area_queue->type, range);
    return new_area;
}

bool partition_allocator_try_release_containers(PartitionAllocator *pa, Area *area)
{
    if (area_is_free(area)) {

        // all sections should be free and very likely in the free sections
        // list.
        const int num_sections = area_get_section_count(area);
        const ContainerType root_ctype = area_get_container_type(area);
        if (root_ctype == CT_HEAP) {
            Heap *heap = (Heap *)((uintptr_t)area + sizeof(Area));
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
                    Heap *heap = (Heap *)collection;
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
    size_t range = area_get_range(a);
    uint64_t new_mask = ((1UL << range) - 1UL) << (64UL - idx - range);
    list->area_mask = list->area_mask & ~new_mask;
    if ((a == list->previous_area) || (list->area_mask == 0)) {
        list->previous_area = NULL;
    }
    free_memory(a, area_get_size(a));
}

Partition *partition_allocator_get_area_list(PartitionAllocator *pa, Area *area)
{
    const AreaType at = area_get_type(area);
    return &pa->area[at];
}

uint32_t partition_allocator_get_area_idx_from_queue(PartitionAllocator *pa, Area *area, Partition *queue)
{
    const ptrdiff_t diff = (uint8_t *)area - (uint8_t *)queue->start_addr;
    return (uint32_t)(((size_t)diff) >> area_type_to_exponent[area_get_type(area)]);
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

int32_t area_list_get_next_area_idx(Partition *queue, uint32_t cidx)
{
    return get_next_mask_idx(queue->area_mask, cidx);
}

Area *area_list_get_area(Partition *queue, uint32_t cidx)
{
    uintptr_t area_addr = queue->start_addr + ((1 << queue->type) * BASE_AREA_SIZE) * cidx;
    return (Area *)area_addr;
}

bool partition_allocator_release_areas_from_queue(PartitionAllocator *pa, Partition *queue)
{
    bool was_released = false;
    int32_t area_idx = area_list_get_next_area_idx(queue, 0);
    // find free section.
    // detach all pools/pages/sections.
    while (area_idx != -1) {
        Area *start = area_list_get_area(queue, area_idx);
        was_released |= !partition_allocator_try_release_containers(pa, start);
        area_idx = area_list_get_next_area_idx(queue, area_idx + 1);
    }
    area_idx = area_list_get_next_area_idx(queue, 0);
    while (area_idx != -1) {
        Area *start = area_list_get_area(queue, area_idx);
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
        Area *start = area_list_get_area(queue, area_idx);
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
    for (size_t i = 0; i < 4; i++) {
        if (pa->area[i].area_mask != 0) {
            was_released |= !partition_allocator_release_areas_from_queue(pa, &pa->area[i]);
        }
    }
    return !was_released;
}

Area *partition_allocator_get_free_area_from_queue(Partition *current_queue)
{
    // the areas are empty
    Area *new_area = NULL;
    Area *previous_area = current_queue->previous_area;
    if (previous_area != NULL) {
        if (!area_is_full(previous_area)) {
            new_area = previous_area;
        }
    }
    if (new_area == NULL) {
        if (current_queue->area_mask != 0) {
            int32_t area_idx = area_list_get_next_area_idx(current_queue, 0);
            while (area_idx != -1) {
                Area *start = area_list_get_area(current_queue, area_idx);
                if (!area_is_full(start)) {
                    new_area = start;
                    break;
                }
                area_idx = area_list_get_next_area_idx(current_queue, area_idx + 1);
            }
        }
    }
    return new_area;
}

Partition *partition_allocator_get_current_queue(PartitionAllocator *pa, AreaType t, const size_t s, size_t *area_size,
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

Partition *partition_allocator_promote_area(PartitionAllocator *pa, AreaType *t, size_t *area_size, size_t *alignement)
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

Area *partition_allocator_get_free_area(PartitionAllocator *pa, size_t s, AreaType t)
{
    size_t area_size = AREA_SIZE_SMALL;
    size_t alignment = area_size;
    Partition *current_queue = partition_allocator_get_current_queue(pa, t, s, &area_size, &alignment);
    Area *new_area = partition_allocator_get_free_area_from_queue(current_queue);
    while (new_area == NULL && (UINT64_MAX == current_queue->area_mask)) {
        // try releasing an area first.
        bool was_released = partition_allocator_release_single_area_from_queue(pa, current_queue);
        if (was_released) {
            new_area = partition_allocator_get_free_area_from_queue(current_queue);
            if (new_area) {
                break;
            }
        }
        // try promotion
        current_queue = partition_allocator_promote_area(pa, &t, &area_size, &alignment);
        if (current_queue == NULL) {
            return NULL;
        }
        new_area = partition_allocator_get_free_area_from_queue(current_queue);
    }

    if (new_area == NULL) {
        if (s < os_page_size) {
            s = os_page_size;
        }
        new_area = partition_allocator_alloc_area(current_queue, s, alignment);
        if (new_area == NULL) {
            return NULL;
        }
    }

    return new_area;
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
    Area *new_area = partition_allocator_get_free_area(pa, size, AT_FIXED_32);
    if (new_area == NULL) {
        return NULL;
    }
    size_t area_size = area_type_to_size[area_get_type(new_area)];
    if (area_size > AREA_SIZE_LARGE) {
        // sections are not supported in areas larger than 128 megs
        if (area_is_free(new_area)) {

            partition_allocator_free_area(pa, new_area);
        }
        return NULL;
    }
    const int32_t section_idx = partition_allocator_claim_section(new_area);
    area_set_container_type(new_area, CT_SECTION);
    Section *section = (Section *)((uint8_t *)new_area + SECTION_SIZE * section_idx);
    section->constr_mask._w32[0] = 0;
    section->active_mask._w32[0] = 0;
    section->idx = section_idx;
    section->partition_mask = new_area->partition_mask;
    return section;
}

Area *partition_allocator_alloc_area(Partition *area_queue, const uint64_t size, const uint64_t alignment)
{
    Area *new_area = partition_allocator_get_next_area(area_queue, size, alignment);
    if (new_area == NULL) {
        return NULL;
    }
    area_queue->previous_area = new_area;
    return new_area;
}
