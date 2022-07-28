#ifndef PARTITION_ALLOCATOR_H
#define PARTITION_ALLOCATOR_H

#include "callocator.inl"
#include "area.h"

typedef void (*free_func)(void *);
Area *partition_allocator_alloc_area(Partition *area_queue, uint64_t area_size, uint64_t alignment);
PartitionAllocator *partition_allocator_init(size_t idx);
PartitionAllocator *partition_allocator_aquire(size_t idx);
message *partition_allocator_get_last_message(PartitionAllocator *pa);

void partition_allocator_thread_free(PartitionAllocator *pa, void *p);
Area *partition_allocator_get_next_area(Partition *area_queue, uint64_t size, uint64_t alignment);
bool partition_allocator_try_release_containers(PartitionAllocator *pa, Area *area);
void partition_allocator_free_area_from_list(PartitionAllocator *pa, Area *a, Partition *list, size_t idx);

Partition *partition_allocator_get_area_list(PartitionAllocator *pa, Area *area);

uint32_t partition_allocator_get_area_idx_from_queue(PartitionAllocator *pa, Area *area, Partition *queue);

void partition_allocator_free_area(PartitionAllocator *pa, Area *area);

bool partition_allocator_try_free_area(PartitionAllocator *pa, Area *area, Partition *list);

int32_t area_list_get_next_area_idx(Partition *queue, uint32_t cidx);

Area *area_list_get_area(Partition *queue, uint32_t cidx);

bool partition_allocator_release_areas_from_queue(PartitionAllocator *pa, Partition *queue);

bool partition_allocator_release_single_area_from_queue(PartitionAllocator *pa, Partition *queue);

bool partition_allocator_release_local_areas(PartitionAllocator *pa);

Area *partition_allocator_get_free_area_from_queue(Partition *current_queue);
Partition *partition_allocator_get_current_queue(PartitionAllocator *pa, AreaType t, const size_t s, size_t *area_size,
                                                 size_t *alignement);

Partition *partition_allocator_promote_area(PartitionAllocator *pa, AreaType *t, size_t *area_size, size_t *alignement);

Area *partition_allocator_get_free_area(PartitionAllocator *pa, size_t s, AreaType t);
static inline uint32_t partition_allocator_claim_section(Area *area) { return area_claim_section(area); }
AreaType get_area_type_for_heap(const size_t size);

Section *partition_allocator_alloc_section(PartitionAllocator *pa, const size_t size);

Area *partition_allocator_alloc_area(Partition *area_queue, const uint64_t size, const uint64_t alignment);

#endif
