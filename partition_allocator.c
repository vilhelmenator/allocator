
#include "partition_allocator.h"
#include "os.h"
#include "pool.h"
#include "arena.h"

cache_align PartitionAllocator *partition_allocator = NULL;
#define PARTITION_ALLOCATOR_STATIC_SIZE (1024 * 1024) // 1MB
static uint8_t partition_allocator_static_buffer[PARTITION_ALLOCATOR_STATIC_SIZE] __attribute__((aligned(64)));

// Returns a pointer to the initialized allocator.
PartitionAllocator* partition_allocator__create(void) {
    if(partition_allocator != NULL)
    {
        return partition_allocator;
    }
    // Calculate total size required.
    size_t total_size = sizeof(PartitionAllocator);
    const size_t blockSizes[PARTITION_COUNT] = {
        256 * 1024 * 1024,              // 256MB
        512 * 1024 * 1024,              // 512MB
        1024 * 1024 * 1024,             // 1GB
        2ULL * 1024 * 1024 * 1024,      // 2GB
        4ULL * 1024 * 1024 * 1024,      // 4GB
        8ULL * 1024 * 1024 * 1024,      // 8GB
        16ULL * 1024 * 1024 * 1024,     // 16GB
        32ULL * 1024 * 1024 * 1024,     // 32GB
        64ULL * 1024 * 1024 * 1024,     // 64GB
    };
    size_t base_count = PARTITION_SIZE/blockSizes[0];
    // Add per-partition metadata (aligned to 64 bytes).
    const size_t partition_meta_sizes[PARTITION_COUNT] = { // 9 * 1TB blocks
        base_count * sizeof(PartitionMasks),  // Partition 0 (256MB blocks)
        base_count/2 * sizeof(PartitionMasks),  // Partition 1 (512MB blocks)
        base_count/4 * sizeof(PartitionMasks),  // Partition 2 (1024MB blocks)
        base_count/8 * sizeof(PartitionMasks),  // Partition 3 (2048MB blocks)
        base_count/16 * sizeof(PartitionMasks),  // Partition 4 (4096MB blocks)
        base_count/32 * sizeof(PartitionMasks),  // Partition 5 (8192MB blocks)
        base_count/64 * sizeof(PartitionMasks),  // Partition 6 (16384MB blocks)
        base_count/128 * sizeof(PartitionMasks),  // Partition 7 (32768MB blocks)
        base_count/256 * sizeof(PartitionMasks),  // Partition 8 (65536MB blocks)
    };

    for (int i = 0; i < PARTITION_COUNT; i++) {
        total_size = ALIGN_CACHE(total_size);
        total_size += partition_meta_sizes[i];
    }

    // Allocate the entire block.
    void* memory = partition_allocator_static_buffer;
    memset(memory, 0, total_size);

    // Set up the allocator.
    PartitionAllocator* allocator = (PartitionAllocator*)memory;
    allocator->totalMemory = PARTITION_COUNT * PARTITION_SIZE;

    //  Initialize partitions.
    uintptr_t current = (uintptr_t)memory + sizeof(PartitionAllocator);
    for (int i = 0; i < PARTITION_COUNT; i++) {
        current = ALIGN_CACHE(current);
        allocator->partitions[i].blocks = (PartitionMasks*)current;
        allocator->partitions[i].num_blocks = PARTITION_SIZE / blockSizes[i];
        allocator->partitions[i].blockSize = blockSizes[i];
        current += partition_meta_sizes[i];
    }

    return allocator;
}

/*
    Abandon blocks in a partition.  

    This will mark the blocks as abandoned
    and will allow the allocator to reclaim them later.
*/
bool partition_allocator_abandon_blocks(PartitionAllocator* palloc,
                                     void* addr) {
    PartitionLoc loc;
    if(get_partition_location(palloc, addr, &loc) == -1)
    {
        // error... this address is outside of our ranges.
        return false;
    }
    // Return the corresponding
    Partition* partition = &palloc->partitions[loc.partition];
    PartitionMasks* block = &partition->blocks[loc.block];
    uint32_t range = get_range(loc.region, block->ranges);
    
    // Update allocation masks
    uint64_t area_clear_mask = (range == 64)
        ? ~0ULL
        : ((1ULL << range) - 1) << loc.region;
    
    
    atomic_fetch_or_explicit(&block->abandoned,
                              area_clear_mask,
                              memory_order_release);
    
    
    return true;
}
bool partition_allocator_free_blocks(PartitionAllocator* palloc,
                                     void* addr,
                                     bool should_decommit) {
    PartitionLoc loc;
    if(get_partition_location(palloc, addr, &loc) == -1)
    {
        // error... this address is outside of our ranges.
        return false;
    }
    // Return the corresponding
    Partition* partition = &palloc->partitions[loc.partition];
    PartitionMasks* block = &partition->blocks[loc.block];
    uint32_t range = get_range(loc.region, block->ranges);
    
    // Update allocation masks
    uint64_t area_clear_mask = (range == 64)
        ? ~0ULL
        : ((1ULL << range) - 1) << loc.region;
    
    
    atomic_fetch_or_explicit(&block->pending_release,
                              area_clear_mask,
                              memory_order_release);
    
    
    if(should_decommit)
    {
        uint64_t free_mask = atomic_load(&block->pending_release);
        uint64_t new_mask = 0ULL;
        uint64_t region_size = partition->blockSize/64;
        uintptr_t base_addr = (uintptr_t)(BASE_ADDRESS + loc.partition*PARTITION_SIZE +
                                          (loc.block * partition->blockSize));
        uint64_t ranges = atomic_load(&block->ranges);
        
        if (atomic_compare_exchange_strong(&block->pending_release, &free_mask, new_mask)) {
            int32_t region_idx = get_next_mask_idx(free_mask, 0);
            // find free section.
            // detach all pools/pages/sections.
            while (region_idx != -1) {
                
                uintptr_t block_addr = base_addr + (region_idx * (region_size));
                uint32_t size_in_blocks = get_range((uint32_t)region_idx, ranges);
                uint64_t area_clear_mask =
                (size_in_blocks == 64 ? ~0ULL : ((1ULL << size_in_blocks) - 1)) << region_idx;
                // reserve our block.
                decommit_memory((void*)block_addr, region_size*size_in_blocks);
                atomic_fetch_and(&block->committed, ~area_clear_mask);
                
                // Clear range_mask bits if needed.
                if (size_in_blocks > 1) {
                    uint64_t range_clear_mask = (1ULL << region_idx) | (1ULL << (region_idx + size_in_blocks - 1));
                    atomic_fetch_and_explicit(&block->ranges,
                                              ~range_clear_mask,
                                              memory_order_relaxed);
                }
                region_idx = get_next_mask_idx(free_mask, region_idx + 1);
            }
        }
    }
    return true;
}

bool partition_allocator_claim_abandoned(PartitionAllocator* palloc,
                                                  void* addr
                                                  ) {
    // Validate indices
    if(palloc == NULL)
    {
        return false; // Invalid allocator
    }   
    PartitionLoc loc;
    if(get_partition_location(palloc, addr, &loc) == -1)
    {
        // error... this address is outside of our ranges.
        return false;
    }
    Partition* partition = &palloc->partitions[loc.partition];
    PartitionMasks* block = &partition->blocks[loc.block];
    
    // Update allocation masks
    uint64_t abandoned_mask = atomic_load(&block->abandoned);
    uint64_t region_mask = (1ULL << loc.region);
    if ((abandoned_mask & region_mask) != 0) {
        if(atomic_fetch_and_explicit(&block->abandoned, ~region_mask, memory_order_acquire))
        {
            return true; // Successfully claimed the abandoned region
        }
    }
    return false; // Region was not abandoned or already claimed
}
// Thread-safe allocation from a partition.
void* partition_allocator_allocate_from_partition(PartitionAllocator* allocator,
                                                  int32_t partition_idx,
                                                  int32_t num_regions,
                                                  int32_t* region_idx,
                                                  bool active) {
    Partition* partition = &allocator->partitions[partition_idx];
    // Find and reserve a free block (atomic CAS loop).
    for (int i = 0; i < partition->num_blocks; i++) {
        PartitionMasks* block = &partition->blocks[i];
        uint64_t free_mask = atomic_load(&block->reserved);
        uint64_t region_size = partition->blockSize/64;
        uintptr_t base_addr = (uintptr_t)(BASE_ADDRESS + partition_idx*PARTITION_SIZE +
                                  (i * partition->blockSize));
        if(free_mask == 0)
        {
            // Attempt to reserve the bit.
            uint64_t new_mask = ~0ULL;
            if (atomic_compare_exchange_strong(&block->reserved, &free_mask, new_mask)) {
                for (int ii = 0; ii < 64; ii++) {
                    
                    uintptr_t block_addr = base_addr + (ii * (region_size));
                    
                    // reserve our block.
                    void* result = alloc_memory((void*)block_addr, region_size, false);
                    if (result != (void*)block_addr) {
                        // revert our reservation.
                        atomic_fetch_and(&block->reserved, ~(1ULL << ii));
                        free_memory(result, region_size);
                    }
                }
            }
        }
        else
        {
            
            free_mask = atomic_load(&block->pending_release);
            uint64_t new_mask = 0ULL;
            if(free_mask != 0)
            {
                if (atomic_compare_exchange_strong(&block->pending_release, &free_mask, new_mask)) {
                    uint64_t ranges = atomic_load(&block->ranges);
                    int32_t region_idx = get_next_mask_idx(free_mask, 0);
                    while (region_idx != -1) {
                        
                        uintptr_t block_addr = base_addr + (region_idx * (region_size));
                        uint32_t size_in_blocks = get_range((uint32_t)region_idx, ranges);
                        uint64_t area_clear_mask =
                        (size_in_blocks == 64 ? ~0ULL : ((1ULL << size_in_blocks) - 1)) << region_idx;
                        decommit_memory((void*)block_addr, region_size*size_in_blocks);
                        atomic_fetch_and(&block->committed, ~area_clear_mask);
                        if (size_in_blocks > 1) {
                            uint64_t range_clear_mask = (1ULL << region_idx) | (1ULL << (region_idx + size_in_blocks - 1));
                            atomic_fetch_and_explicit(&block->ranges,
                                                      ~range_clear_mask,
                                                      memory_order_relaxed);
                        }
                        region_idx = get_next_mask_idx(free_mask, region_idx + 1);
                    }
                }
            }
        
        }
        free_mask = atomic_load(&block->committed);
        // Find first zero bit (free block).
        int bit = find_first_nzeros(free_mask, num_regions, 0);
        if (bit < 0) continue;  // No free blocks in this chunk.

        
        // Attempt to reserve the bit.
        uint64_t new_mask = free_mask | (1ULL << bit);
        if (atomic_compare_exchange_strong(&block->committed, &free_mask, new_mask)) {
            // Calculate the block's address.
            uintptr_t block_addr = base_addr + (bit * (region_size));

            // Commit memory (if requested).
            if (!commit_memory((void*)block_addr, region_size)) {
                // Failed to commit; revert the bitmask.
                atomic_fetch_and(&block->committed, ~(1ULL << bit));
                return NULL;
            }
            if(active)
            {
                atomic_fetch_or_explicit(&block->active,
                                         free_mask,
                                         memory_order_relaxed);
            }
            
            // Set range_mask bits if needed.
            if (num_regions > 1) {
                uint64_t range_add_mask = (1ULL << bit) | (1ULL << (bit + num_regions - 1));
                atomic_fetch_or_explicit(&block->ranges,
                                          range_add_mask,
                                          memory_order_relaxed);
            }
            // update commit mask.
            *region_idx = bit;
            return (void*)block_addr;
        }
        // CAS failed (another thread took the block); retry.
    }
    return NULL;  // No free blocks in the entire partition.
}

PartitionMasks* get_partition_masks(PartitionAllocator* allocator, void* addr,
                                    uint32_t* sub_idx) {
    // Convert address to integer and validate range
    uintptr_t p = (uintptr_t)addr;
    if (p < (uintptr_t)BASE_ADDRESS ||
        p >= (uintptr_t)BASE_ADDRESS + allocator->totalMemory) {
        return NULL;
    }

    // Calculate partition index (large partitions)
    uintptr_t offset = p - (uintptr_t)BASE_ADDRESS;
    int32_t partition_id = (int32_t)(offset / PARTITION_SIZE);
    
    if (partition_id < 0 || partition_id >= PARTITION_COUNT) {
        return NULL;
    }

    // Get the specific partition
    Partition* partition = &allocator->partitions[partition_id];
    
    // Calculate large block index within partition
    uintptr_t partition_offset = offset % PARTITION_SIZE;
    uint32_t block_index = (uint32_t)(partition_offset / partition->blockSize);
    
    if (block_index >= partition->num_blocks) {
        return NULL;
    }
    
    uintptr_t base_address = (uintptr_t)(BASE_ADDRESS +
                                         partition_id * PARTITION_SIZE +
                                         block_index*partition->blockSize);
    
    const ptrdiff_t diff = (uint8_t *)addr - (uint8_t *)base_address;
    *sub_idx =  (uint32_t)((size_t)diff >> (22+partition_id));
    
    // Return the corresponding
    return &partition->blocks[block_index];
}

int32_t get_partition_location(PartitionAllocator* allocator, void* addr, PartitionLoc* loc) {
    // Convert address to integer and validate range
    uintptr_t p = (uintptr_t)addr;
    if (p < (uintptr_t)BASE_ADDRESS ||
        p >= (uintptr_t)BASE_ADDRESS + allocator->totalMemory) {
        return -1;
    }
    
    // Calculate partition index (large partitions)
    uintptr_t offset = p - (uintptr_t)BASE_ADDRESS;
    int32_t partition_id = (int32_t)(offset / PARTITION_SIZE);
    
    if (partition_id < 0 || partition_id >= PARTITION_COUNT) {
        return -1;
    }
    
    // Get the specific partition
    Partition* partition = &allocator->partitions[partition_id];
    
    // Calculate large block index within partition
    uintptr_t partition_offset = offset % PARTITION_SIZE;
    uint32_t block_index = (uint32_t)(partition_offset / partition->blockSize);
    
    if (block_index >= partition->num_blocks) {
        return -1;
    }
    
    uintptr_t base_address = (uintptr_t)(BASE_ADDRESS +
                                         partition_id * PARTITION_SIZE +
                                         block_index*partition->blockSize);
    
    const ptrdiff_t diff = (uint8_t *)addr - (uint8_t *)base_address;
    
    loc->region =  (uint32_t)((size_t)diff >> (22+partition_id));
    loc->partition = partition_id;
    loc->block = block_index;
    
    return 0;
}

void* partition_allocator_get_free_region(PartitionAllocator* allocator,
                                          int32_t partition_idx,
                                          int32_t num_regions,
                                          int32_t* region_idx,
                                          bool active )
{
    return partition_allocator_allocate_from_partition(allocator, partition_idx, num_regions, region_idx, active);
}



