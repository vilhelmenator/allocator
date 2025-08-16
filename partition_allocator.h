#ifndef PARTITION_ALLOCATOR_H
#define PARTITION_ALLOCATOR_H

#include "callocator.inl"
typedef struct
{
    uint32_t partition;
    uint32_t block;
    uint32_t region;
} PartitionLoc;

PartitionAllocator* partition_allocator__create(void);
void* partition_allocator_allocate_from_partition(PartitionAllocator* allocator,
                                                  int32_t partition_idx,
                                                  uint32_t num_regions,
                                                  int32_t* region_idx,
                                                  bool active);
PartitionMasks* get_partition_masks(PartitionAllocator* allocator, void* addr, uint32_t* sub_idx);
void* partition_allocator_get_free_region(PartitionAllocator* allocator,
                                          int32_t partition_idx,
                                          uint32_t num_regions,
                                          int32_t* region_idx, bool active);

bool partition_allocator_free_blocks(PartitionAllocator* palloc,
                                     void* addr,
                                     bool should_decommit);
int32_t get_partition_location(PartitionAllocator* allocator, void* addr, PartitionLoc* loc);
bool partition_allocator_decommit_pending(PartitionAllocator* palloc);
bool partition_allocator_claim_abandoned(PartitionAllocator* palloc,
                                                  void* addr);
bool partition_allocator_abandon_blocks(PartitionAllocator* palloc,
                                     void* addr);
bool partition_allocator_rest_blocks(PartitionAllocator* palloc,
                                     void* addr);
#endif
