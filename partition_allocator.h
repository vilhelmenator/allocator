#ifndef PARTITION_ALLOCATOR_H
#define PARTITION_ALLOCATOR_H

#include "area.h"

typedef void (*free_func)(void *);
Area *partition_allocator_alloc_area(Partition *area_queue, uint64_t area_size, uint64_t alignment);
PartitionAllocator *partition_allocator_aquire(size_t idx);
message *partition_allocator_get_last_message(PartitionAllocator *pa);
void partition_allocator_thread_free(PartitionAllocator *pa, void *p);
void partition_allocator_free_area(PartitionAllocator *pa, Area *area);
bool partition_allocator_release_local_areas(PartitionAllocator *pa);
Area *partition_allocator_get_free_area(PartitionAllocator *pa, size_t s, AreaType t);
AreaType get_area_type_for_heap(const size_t size);
Section *partition_allocator_alloc_section(PartitionAllocator *pa, const size_t size);

static inline uint32_t partition_allocator_claim_section(Area *area) { return area_claim_section(area); }
#endif
