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
bool partition_allocator_allocate_blocks(PartitionMasks* block, uint8_t size_in_blocks);
int partition_allocator_alloc_subblock(PartitionMasks* block);
void* partition_allocator_allocate_from_partition(PartitionAllocator* allocator,
                                                  int32_t partition_idx, int32_t* region_idx, bool commit);
PartitionMasks* get_partition_masks(PartitionAllocator* allocator, void* addr, uint32_t* sub_idx);
void* partition_allocator_get_free_region(PartitionAllocator* allocator, int32_t partition_idx, int32_t* region_idx);

int32_t partition_allocator_free_blocks(PartitionAllocator* palloc,
                                     void* addr,
                                     bool should_decommit);
int32_t get_partition_location(PartitionAllocator* allocator, void* addr, PartitionLoc* loc);

#endif
