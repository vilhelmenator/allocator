
#include "partition_allocator.h"
#include "os.h"
#include "pool.h"
#include "arena.h"

cache_align PartitionAllocator *partition_allocators[MAX_THREADS];
cache_align uint8_t* default_allocator_buffer = 0;
PartitionAllocator *partition_allocator_init(size_t idx, uintptr_t thr_mem)
{
    // partition owners
    // the allocator is at 4k alignment
    // 
    Allocator *alloc = (Allocator *)thr_mem;
    alloc->idx = (int32_t)idx;
    alloc->prev_size = -1;
    thr_mem = ALIGN_CACHE(thr_mem + sizeof(Allocator));
    mutex_t * lock_mutex = (mutex_t *)thr_mem;
    thr_mem = ALIGN_CACHE(thr_mem + sizeof(mutex_t));
    // next come the partition allocator structs.
    Queue *pool_queue = (Queue *)thr_mem;
    thr_mem = ALIGN_CACHE(thr_mem + sizeof(Queue) * POOL_BIN_COUNT);
    Queue *arena_queue = (Queue *)thr_mem;
    thr_mem = (uintptr_t)alloc + DEFAULT_OS_PAGE_SIZE;
    thr_mem -= ALIGN_CACHE(sizeof(PartitionAllocator));
    
    mutex_init(lock_mutex);
    
    PartitionAllocator *palloc = (PartitionAllocator *)thr_mem;
    palloc->idx = idx;
    palloc->part_lock = (struct mutex_t*)lock_mutex;
    alloc->pools = pool_queue;
    alloc->arenas = arena_queue;
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

int32_t partition_allocator_get_next_area(PartitionAllocator *pa, Partition *partition, uint64_t size, uint64_t alignment)
{
    AreaType at = (AreaType)partition_allocator_get_partition_idx(pa, partition);
    size_t base_size = BASE_AREA_SIZE * 64 << (uint64_t)at;
    size_t offset = BASE_ADDR(at);
    size_t start_addr = (pa->idx)*base_size + offset;
    size_t type_exponent = area_type_to_exponent[at];
    size_t type_size = area_type_to_size[at];
    uint32_t range = (uint32_t)(size >> type_exponent);
    range += (size & (1 << type_exponent) - 1) ? 1 : 0;
    size = type_size * range;

    int32_t idx = find_first_nzeros(partition->commit_mask, range, 0);
    if (idx == -1) {
        return -1; // no room.
    }
    
    uintptr_t aligned_addr = start_addr + (type_size * idx);
    
    if(commit_memory((void *)aligned_addr, size))
    {
        uint64_t new_mask = (1ULL << range) - 1ULL;
        partition->commit_mask |= (new_mask << idx);
        partition->range_mask |= apply_range(range, idx);
        return idx;
    }
    return -1;
}

int32_t partition_allocator_reserve_areas(PartitionAllocator *pa, Partition *partition)
{
    AreaType at = (AreaType)partition_allocator_get_partition_idx(pa, partition);
    size_t base_size = BASE_AREA_SIZE * 64 << (uint64_t)at;
    size_t offset = BASE_ADDR(at);
    size_t start_addr = (pa->idx)*base_size + offset;
    size_t type_size = area_type_to_size[at];
    for(uint8_t i = 0; i < 64; i++ )
    {
        uintptr_t aligned_addr = start_addr + (type_size * i);
        void *ptr = alloc_memory((void*)aligned_addr, type_size, false);
        commit_memory(ptr, type_size);
        if((uintptr_t)ptr == aligned_addr)
        {
            uint64_t new_mask = (1ULL << 1) - 1ULL;
            partition->area_mask |= (new_mask << i);
        }
        else
        {
            free_memory(ptr, type_size);
        }
    }
    
    
    return -1;
}

static inline Partition *partition_allocator_get_area_list(PartitionAllocator *pa, uintptr_t area)
{
    AreaType at = (AreaType)partition_id_from_addr(area);
    return &pa->area[at];
}

static inline int32_t area_list_get_next_area_idx(Partition *partition, uint32_t cidx)
{
    return get_next_mask_idx(partition->commit_mask, cidx);
}

bool partition_allocator_try_release_containers(PartitionAllocator *pa, Allocator* alloc, uintptr_t area)
{
    Arena* arena = (Arena*)area;
    if (arena->allocations <= 1) {
        
        return true;
    }
    return false;
}

void partition_allocator_free_area_from_list(PartitionAllocator *pa, uintptr_t a, Partition *list, size_t idx)
{
    uint64_t range = get_range((uint32_t)idx, list->range_mask);
    uint64_t new_mask = ((1ULL << range) - 1UL) << idx;
    list->range_mask = list->range_mask & ~new_mask;
    list->zero_mask = list->zero_mask & ~new_mask;
    list->full_mask = list->full_mask & ~new_mask;
    list->commit_mask = list->commit_mask & ~new_mask;
    AreaType at = (AreaType)partition_allocator_get_partition_idx(pa, list);
    decommit_memory((void*)a, area_type_to_size[at]*range);

}


void partition_allocator_free_area(PartitionAllocator *pa, uintptr_t area)
{
    Partition *partition = partition_allocator_get_area_list(pa, area);
    int32_t idx = partition_allocator_get_area_idx_from_partition(pa, area, partition);
    partition_allocator_free_area_from_list(pa, area, partition, idx);
}

bool partition_allocator_try_free_area(PartitionAllocator *pa, uintptr_t area, Partition *list)
{
    Arena* arena = (Arena*)area;
    if (arena->allocations <= 1) {
        int32_t idx = partition_allocator_get_arena_idx_from_queue(pa, arena, list);
        partition_allocator_free_area_from_list(pa, area, list, idx);
        return true;
    }
    return false;
}

uintptr_t area_list_get_area(PartitionAllocator* pa, Partition *partition, uint32_t cidx)
{
    AreaType at = (AreaType)partition_allocator_get_partition_idx(pa, partition);
    size_t base_size = BASE_AREA_SIZE * 64 << (uint64_t)at;
    size_t offset = BASE_ADDR(at);
    size_t start_addr = (pa->idx)*base_size + offset;
    uintptr_t area_addr = start_addr + ((1 << at) * BASE_AREA_SIZE) * cidx;
    return area_addr;
}

bool partition_allocator_release_areas_from_queue(PartitionAllocator *pa, Allocator* alloc, Partition *partition)
{
    bool was_released = false;
    int32_t area_idx = area_list_get_next_area_idx(partition, 0);
    // find free section.
    // detach all pools/pages/sections.
    while (area_idx != -1) {
        uintptr_t start = area_list_get_area(pa, partition, area_idx);
        if((partition->full_mask & 1ULL << area_idx) != 0)
        {
            was_released |= !partition_allocator_try_release_containers(pa, alloc, start);
        }
        area_idx = area_list_get_next_area_idx(partition, area_idx + 1);
    }
    area_idx = area_list_get_next_area_idx(partition, 0);
    while (area_idx != -1) {
        uintptr_t start = area_list_get_area(pa, partition, area_idx);
        if((partition->full_mask & 1ULL << area_idx) != 0)
        {
            was_released |= !partition_allocator_try_free_area(pa, start, partition);
        }
        area_idx = area_list_get_next_area_idx(partition, area_idx + 1);
    }
    return !was_released;
}

bool partition_allocator_release_single_area_from_queue(PartitionAllocator *pa, Allocator* alloc, Partition *partition)
{
    int32_t area_idx = area_list_get_next_area_idx(partition, 0);
    // find free section.
    // detach all pools/pages/sections.
    while (area_idx != -1) {
        uintptr_t start = area_list_get_area(pa, partition, area_idx);
        if (partition_allocator_try_release_containers(pa, alloc, start)) {
            return partition_allocator_try_free_area(pa, start, partition);
        }
        area_idx = area_list_get_next_area_idx(partition, area_idx + 1);
    }
    return false;
}

bool partition_allocator_release_local_areas(PartitionAllocator *pa, Allocator* alloc)
{
    bool was_released = false;
    for (size_t i = 0; i < 7; i++) {
        if (pa->area[i].area_mask != 0) {
            was_released |= !partition_allocator_release_areas_from_queue(pa, alloc, &pa->area[i]);
        }
    }
    return !was_released;
}
void partition_allocator_release_deferred(PartitionAllocator *pa, Allocator* a)
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

int32_t partition_allocator_get_free_area_from_partitino(PartitionAllocator*pa, Partition *current_partition, uint8_t num_blocks, bool zero)
{
    // the areas are empty
    int32_t new_area = -1;
    if (current_partition->area_mask != current_partition->full_mask) {
        int32_t area_idx = area_list_get_next_area_idx(current_partition, 0);
        while (area_idx != -1) {
            if(((current_partition->commit_mask & 1ULL << area_idx) != 0) && ((current_partition->full_mask & 1ULL << area_idx) == 0)){
                if(zero)
                {
                    if((current_partition->zero_mask & (1ULL << area_idx)) == 0)
                    {
                        new_area = area_idx;
                    }
                }
                else
                {
                    new_area = area_idx;
                }

                break;
            }
            area_idx = area_list_get_next_area_idx(current_partition, area_idx + 1);
        }
    }

    return new_area;
}

Partition *partition_allocator_get_free_area(Allocator *a, uint8_t partition_idx, size_t s, int32_t* new_area_idx, bool zero)
{
    PartitionAllocator* pa = a->part_alloc;
    size_t area_size = area_type_to_size[partition_idx];
    size_t alignment = area_size;
   
    Partition *current_partition = &pa->area[partition_idx];
    if (s < os_page_size) {
        s = os_page_size;
    }
    
    if(current_partition->area_mask == 0)
    {
        partition_allocator_reserve_areas(pa, current_partition);
    }
    else
    {
        partition_allocator_release_local_areas(pa, a);
    }
    
    *new_area_idx = partition_allocator_get_next_area(pa, current_partition, s, alignment);
    if (*new_area_idx == -1) {
        return NULL;
    }
    
    return current_partition;
}



