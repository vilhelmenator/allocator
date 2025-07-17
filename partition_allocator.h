#ifndef PARTITION_ALLOCATOR_H
#define PARTITION_ALLOCATOR_H

#include "callocator.inl"
#define PARTITION_ALLOCATOR_BASE_SIZE (1ULL << 13)


PartitionAllocator* partition_allocator_init_default(void);
PartitionAllocator *partition_allocator_aquire(size_t idx);
AtomicMessage *partition_allocator_get_last_message(PartitionAllocator *pa);
void partition_allocator_thread_free(PartitionAllocator *pa, void *p);
void partition_allocator_free_area(PartitionAllocator *pa, uintptr_t area);
bool partition_allocator_release_local_areas(PartitionAllocator *pa, Allocator* a);
void partition_allocator_release_deferred(PartitionAllocator *pa, Allocator* a);
Partition *partition_allocator_get_free_area(Allocator* a, uint8_t partition_id, size_t s, int32_t* area_idx, bool zero);


static inline uint32_t partition_allocator_get_area_idx_from_partition(PartitionAllocator *pa, uintptr_t area, Partition *partition)
{
    AreaType at = (AreaType)partition_allocator_get_partition_idx(pa, partition);
    size_t base_size = BASE_AREA_SIZE * 64 << (uint64_t)at;
    size_t offset = BASE_ADDR(at);
    size_t start_addr = (pa->idx)*base_size + offset;
    const ptrdiff_t diff = (uint8_t *)area - (uint8_t *)start_addr;
    return (uint32_t)(((size_t)diff) >> area_type_to_exponent[at]);
}
static inline uintptr_t partition_allocator_area_at_idx(PartitionAllocator* pa, Partition* p, size_t idx)
{
    AreaType at = (AreaType)partition_allocator_get_partition_idx(pa, p);
    size_t base_size = BASE_AREA_SIZE * 64 << (uint64_t)at;
    size_t offset = BASE_ADDR(at);
    size_t start_addr = (pa->idx)*base_size + offset;
    size_t s = (1 << area_type_to_exponent[at]);
    return (uintptr_t)(start_addr + (s * idx));
}

#endif
