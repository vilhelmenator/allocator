#ifndef PARTITION_ALLOCATOR_H
#define PARTITION_ALLOCATOR_H

#include "area.h"
#define PARTITION_ALLOCATOR_BASE_SIZE \
ALIGN_CACHE(sizeof(Allocator))+\
ALIGN_CACHE(sizeof(Queue) * (POOL_BIN_COUNT + HEAP_TYPE_COUNT + 1))+\
ALIGN_CACHE(sizeof(message_queue))+\
ALIGN_CACHE(sizeof(PartitionAllocator))

typedef void (*free_func)(void *);
PartitionAllocator* partition_allocator_init_default(void);
PartitionAllocator *partition_allocator_aquire(size_t idx);
message *partition_allocator_get_last_message(PartitionAllocator *pa);
void partition_allocator_thread_free(PartitionAllocator *pa, void *p);
void partition_allocator_free_area(PartitionAllocator *pa, Area *area);
bool partition_allocator_release_local_areas(PartitionAllocator *pa);
Area *partition_allocator_get_free_area(PartitionAllocator *pa, size_t s, AreaType t);
AreaType get_area_type_for_heap(const size_t size);
Section *partition_allocator_alloc_section(PartitionAllocator *pa, const size_t size);


static inline uint32_t partition_allocator_get_partition_idx(PartitionAllocator* pa, Partition* queue)
{
    uintptr_t delta = (uintptr_t)queue - (uintptr_t)&pa->area[0];
    return (uint32_t)(delta / sizeof(Partition));
}
static inline uint32_t partition_allocator_get_area_idx_from_queue(PartitionAllocator *pa, Area *area, Partition *queue)
{
    AreaType at = (AreaType)partition_allocator_get_partition_idx(pa, queue);
    size_t base_size = BASE_AREA_SIZE * 64 << (uint64_t)at;
    size_t offset = ((size_t)1 << 40) << (uint64_t)at;
    size_t start_addr = (pa->idx)*base_size + offset;
    const ptrdiff_t diff = (uint8_t *)area - (uint8_t *)start_addr;
    return (uint32_t)(((size_t)diff) >> area_type_to_exponent[area_get_type(area)]);
}
#endif
