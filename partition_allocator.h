#ifndef PARTITION_ALLOCATOR_H
#define PARTITION_ALLOCATOR_H

#include "area.h"
#define PARTITION_ALLOCATOR_BASE_SIZE (1ULL << 13)

PartitionAllocator* partition_allocator_init_default(void);
PartitionAllocator *partition_allocator_aquire(size_t idx);
AtomicMessage *partition_allocator_get_last_message(PartitionAllocator *pa);
void partition_allocator_thread_free(PartitionAllocator *pa, void *p);
void partition_allocator_free_area(PartitionAllocator *pa, Area *area);
bool partition_allocator_release_local_areas(PartitionAllocator *pa);
void partition_allocator_release_deferred(PartitionAllocator *pa);
Partition *partition_allocator_get_free_area(PartitionAllocator *pa, size_t s, AreaType t, int32_t* area_idx);
AreaType get_area_type_for_heap(const size_t size);
Section *partition_allocator_alloc_section(PartitionAllocator *pa, const size_t size);

static inline uint32_t partition_allocator_get_partition_idx(PartitionAllocator* pa, Partition* queue)
{
    // sizeof(Partition) == 32
    uintptr_t delta = (uintptr_t)queue - (uintptr_t)&pa->area[0];
    return (uint32_t)(delta >> 5);
}

static inline uint32_t partition_allocator_get_arena_idx_from_queue(PartitionAllocator *pa, Arena *arena, Partition *queue)
{
    AreaType at = (AreaType)partition_allocator_get_partition_idx(pa, queue);
    size_t base_size = BASE_AREA_SIZE * 64 << (uint64_t)at;
    size_t offset = BASE_ADDR(at);
    size_t start_addr = (pa->idx)*base_size + offset;
    const ptrdiff_t diff = (uint8_t *)arena - (uint8_t *)start_addr;
    return (uint32_t)(((size_t)diff) >> arena->container_exponent);
}

static inline uint32_t partition_allocator_get_area_idx_from_queue(PartitionAllocator *pa, Area *area, Partition *queue)
{
    AreaType at = (AreaType)partition_allocator_get_partition_idx(pa, queue);
    size_t base_size = BASE_AREA_SIZE * 64 << (uint64_t)at;
    size_t offset = BASE_ADDR(at);
    size_t start_addr = (pa->idx)*base_size + offset;
    const ptrdiff_t diff = (uint8_t *)area - (uint8_t *)start_addr;
    return (uint32_t)(((size_t)diff) >> area_type_to_exponent[area_get_type(area)]);
}
static inline void *partition_allocator_area_at_idx(PartitionAllocator* pa, Partition* p, size_t idx)
{
    AreaType at = (AreaType)partition_allocator_get_partition_idx(pa, p);
    size_t base_size = BASE_AREA_SIZE * 64 << (uint64_t)at;
    size_t offset = BASE_ADDR(at);
    size_t start_addr = (pa->idx)*base_size + offset;
    size_t s = (1 << area_type_to_exponent[at]);
    return (void*)(start_addr + (s * idx));
}

static inline Partition *partition_from_addr(uintptr_t p)
{
    static const uint64_t masks[] = {~((AREA_SIZE_SMALL>>3) - 1),
                                    ~((AREA_SIZE_SMALL>>2) - 1),
                                    ~((AREA_SIZE_SMALL>>1) - 1),
                                    ~(AREA_SIZE_SMALL - 1),
                                    ~(AREA_SIZE_MEDIUM - 1),
                                    ~(AREA_SIZE_LARGE - 1),
                                    ~(AREA_SIZE_HUGE - 1),
                                    UINT64_MAX,
                                    UINT64_MAX,
                                    UINT64_MAX};

    const int8_t pidx = partition_id_from_addr(p);
    if (pidx < 0) {
        return NULL;
    }
    return (Partition *)(p & masks[pidx]);
}
#endif
