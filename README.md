# High-Performance General Purpose Allocator

## Overview
A two-tiered memory allocator designed for extreme performance, outperforming common allocators like `mi_malloc`. The system combines partition-based bulk memory management with thread-local caching for optimal allocation speed. It reduces OS level fragmentation by managing large virtual memory blocks by the backend, and effectively eliminates any fragmentation at the front-end with proper block management.

## Architecture

### 1. Backend: Partition Allocator
Manages large memory regions through size-partitioned blocks.

**Key Structures:**
```c
typedef struct {
    _Atomic(uint64_t) reserved;     // Track reserved parts
    _Atomic(uint64_t) committed;    // Track committed virtual memory
    _Atomic(uint64_t) ranges;       // Memory extents
    _Atomic(uint64_t) pending_release;
} PartitionMasks;

typedef struct {
    PartitionMasks* blocks;         // Array of managed blocks
    size_t num_blocks;              // Current block count
    size_t blockSize;               // Fixed block size for partition
} Partition;

typedef struct {
    Partition partitions[PARTITION_COUNT];  // 9 partitions total
    size_t totalMemory;                    // Total managed memory
} PartitionAllocator;
```

**Partition Characteristics:**
- 9 partitions, each managing 1TB address space
- Partition 0: 256MB regions (4MB sub-regions)
- Partition 1: 512MB regions (8MB sub-regions)
- ... doubling in size for each subsequent partition
- Uses 64-bit masks to track 64 subdivisions per region

### 2. Frontend: Thread-Local Allocator
Handles actual allocations with thread-specific caching.

**Key Structures:**
```c
typedef struct Allocator_t {
    _Atomic(uintptr_t) thread_id;   // Allocator thread id
    int64_t prev_size;              // Size tracking
    
    // Memory management queues
    Queue *pools;                   // Small allocation pools
    Queue *arenas;                  // Medium allocation arenas
    Queue *implicit;                // Special-case allocations
    
    // Performance optimization
    alloc_slot c_slot;              // Allocation cache
    deferred_free c_deferred;       // Release cache
} Allocator;
```

## Allocation Strategy

### Size-Based Routing:
1. **<32KB requests**: Handled by pool allocator
   - Multiple size classes to minimize waste
   - Optimized for alignment

2. **32KB-4MB requests**: 
   - Power-of-2 sizes: Allocated from arenas
   - Odd sizes: Boundary tag allocator

3. **4MB-256MB requests**:
   - Power-of-2: Direct from partition allocator
   - Odd sizes: Boundary tag allocator

4. **>256MB requests**:
   - Rounded to nearest power-of-2
   - Allocated directly from partition allocator

5. **>8GB requests**: Forwarded to OS

## Thread Handling

- Each arena/pool marked with owner thread_id
- Cross-thread frees handled via:
  - Counters for general case
  - Atomic lists for urgent reallocation
- Thread termination:
  - Orphaned arenas marked (thread_id = -1)
  - Completely unused arenas released
  - Others gradually adopted by allocating threads

## Performance Optimizations

1. **Slot Cache**:
   - Frontend static struct avoids pointer chasing
   - Uses simple offset arithmetic for contiguous blocks

2. **State Tracking**:
   - Pools/arenas marked as unused/in_use/consumed
   - Cached lists for fast access

3. **Deferred Freeing**:
   - Batched release operations
   - Memory returned to original owner when possible

## Memory Management

- Virtual memory committed on demand
- Large regions subdivided via bitmask tracking
- Boundary tag allocator for irregular sizes
- Gradual reclamation of unused resources

This design achieves high performance through:
- Size-specific allocation paths
- Thread-local caching
- Minimal atomic operations in hot paths
- Optimized memory layout for cache efficiency

Parts of the allocator that are still in developedment.
- Large allocations
- Orphaned thread arenas
- Purging cached structs back to OS.
- More tests and better metrics to compare against other allocators
---

## Build and run

  clang *c -o test -O3
  
  ./test

## mi_malloc test comparisons.

clang *c ../mimalloc-master/src/static.c -I ../mimalloc-master/include -O3 -DMI_DEBUG=0 -o test

./test mi_malloc
```
Test with free -> size: [1,2,4,8,..8192], num items: 800000, num_iterations 10
[ TIME     ] mi_malloc (68 (milli sec))               [ TIME     ] cmalloc (57 (milli sec))   [ TIME     ] malloc (379 (milli sec))
[ TIME     ] mi_malloc (45 (milli sec))               [ TIME     ] cmalloc (40 (milli sec))   [ TIME     ] malloc (371 (milli sec))
[ TIME     ] mi_malloc (44 (milli sec))               [ TIME     ] cmalloc (39 (milli sec))   [ TIME     ] malloc (364 (milli sec))
[ TIME     ] mi_malloc (44 (milli sec))               [ TIME     ] cmalloc (38 (milli sec))   [ TIME     ] malloc (371 (milli sec))
[ TIME     ] mi_malloc (44 (milli sec))               [ TIME     ] cmalloc (40 (milli sec))   [ TIME     ] malloc (370 (milli sec))
[ TIME     ] mi_malloc (48 (milli sec))               [ TIME     ] cmalloc (40 (milli sec))   [ TIME     ] malloc (440 (milli sec))
[ TIME     ] mi_malloc (56 (milli sec))               [ TIME     ] cmalloc (45 (milli sec))   [ TIME     ] malloc (526 (milli sec))
[ TIME     ] mi_malloc (103 (milli sec))              [ TIME     ] cmalloc (63 (milli sec))   [ TIME     ] malloc (630 (milli sec))
[ TIME     ] mi_malloc (118 (milli sec))              [ TIME     ] cmalloc (75 (milli sec))   [ TIME     ] malloc (782 (milli sec))
[ TIME     ] mi_malloc (144 (milli sec))              [ TIME     ] cmalloc (96 (milli sec))   [ TIME     ] malloc (1 seconds : 12 (milli sec))
[ TIME     ] mi_malloc (179 (milli sec))              [ TIME     ] cmalloc (127 (milli sec))  [ TIME     ] malloc (326 (milli sec))
[ TIME     ] mi_malloc (265 (milli sec))              [ TIME     ] cmalloc (204 (milli sec))  [ TIME     ] malloc (341 (milli sec))
[ TIME     ] mi_malloc (373 (milli sec))              [ TIME     ] cmalloc (207 (milli sec))  [ TIME     ] malloc (365 (milli sec))
[ TIME     ] mi_malloc (632 (milli sec))              [ TIME     ] cmalloc (361 (milli sec))  [ TIME     ] malloc (417 (milli sec))
Committed physical pages post:
[ 403265   ]                                          [ 506912.  ]                            [ 3200.    ]

Test with immediate free -> size: [8,..8192], num items: 800000, num_iterations 10
[ TIME     ] mi_malloc (57 (milli sec))               [ TIME     ] cmalloc (35 (milli sec))
[ TIME     ] mi_malloc (56 (milli sec))               [ TIME     ] cmalloc (35 (milli sec))
[ TIME     ] mi_malloc (57 (milli sec))               [ TIME     ] cmalloc (35 (milli sec))
[ TIME     ] mi_malloc (57 (milli sec))               [ TIME     ] cmalloc (35 (milli sec))
[ TIME     ] mi_malloc (58 (milli sec))               [ TIME     ] cmalloc (35 (milli sec))
[ TIME     ] mi_malloc (59 (milli sec))               [ TIME     ] cmalloc (35 (milli sec))
[ TIME     ] mi_malloc (60 (milli sec))               [ TIME     ] cmalloc (35 (milli sec))
[ TIME     ] mi_malloc (66 (milli sec))               [ TIME     ] cmalloc (35 (milli sec))
[ TIME     ] mi_malloc (68 (milli sec))               [ TIME     ] cmalloc (35 (milli sec))
[ TIME     ] mi_malloc (75 (milli sec))               [ TIME     ] cmalloc (35 (milli sec))
[ TIME     ] mi_malloc (68 (milli sec))               [ TIME     ] cmalloc (35 (milli sec))
[ TIME     ] mi_malloc (103 (milli sec))              [ TIME     ] cmalloc (35 (milli sec))
[ TIME     ] mi_malloc (105 (milli sec))              [ TIME     ] cmalloc (35 (milli sec))
[ TIME     ] mi_malloc (92 (milli sec))               [ TIME     ] cmalloc (35 (milli sec))

Test with free -> size: [8192,..8], num items: 800000, num_iterations 10
[ TIME     ] mi_malloc (379 (milli sec))              [ TIME     ] cmalloc (104 (milli sec))
[ TIME     ] mi_malloc (249 (milli sec))              [ TIME     ] cmalloc (76 (milli sec))
[ TIME     ] mi_malloc (190 (milli sec))              [ TIME     ] cmalloc (67 (milli sec))
[ TIME     ] mi_malloc (151 (milli sec))              [ TIME     ] cmalloc (65 (milli sec))
[ TIME     ] mi_malloc (121 (milli sec))              [ TIME     ] cmalloc (69 (milli sec))
[ TIME     ] mi_malloc (113 (milli sec))              [ TIME     ] cmalloc (64 (milli sec))
[ TIME     ] mi_malloc (98 (milli sec))               [ TIME     ] cmalloc (59 (milli sec))
[ TIME     ] mi_malloc (52 (milli sec))               [ TIME     ] cmalloc (42 (milli sec))
[ TIME     ] mi_malloc (47 (milli sec))               [ TIME     ] cmalloc (40 (milli sec))
[ TIME     ] mi_malloc (44 (milli sec))               [ TIME     ] cmalloc (39 (milli sec))
[ TIME     ] mi_malloc (43 (milli sec))               [ TIME     ] cmalloc (38 (milli sec))
[ TIME     ] mi_malloc (43 (milli sec))               [ TIME     ] cmalloc (38 (milli sec))
[ TIME     ] mi_malloc (43 (milli sec))               [ TIME     ] cmalloc (38 (milli sec))
[ TIME     ] mi_malloc (43 (milli sec))               [ TIME     ] cmalloc (38 (milli sec))

Test scatter sizes([8,16,32],...[1024,2048,4196]) with free -> num items: 800000, num_iterations 10
[ TIME     ] mi_malloc (43 (milli sec))               [ TIME     ] cmalloc (44 (milli sec))
[ TIME     ] mi_malloc (44 (milli sec))               [ TIME     ] cmalloc (44 (milli sec))
[ TIME     ] mi_malloc (38 (milli sec))               [ TIME     ] cmalloc (44 (milli sec))
[ TIME     ] mi_malloc (34 (milli sec))               [ TIME     ] cmalloc (46 (milli sec))
[ TIME     ] mi_malloc (38 (milli sec))               [ TIME     ] cmalloc (53 (milli sec))
[ TIME     ] mi_malloc (56 (milli sec))               [ TIME     ] cmalloc (62 (milli sec))
[ TIME     ] mi_malloc (79 (milli sec))               [ TIME     ] cmalloc (66 (milli sec))
[ TIME     ] mi_malloc (100 (milli sec))              [ TIME     ] cmalloc (75 (milli sec))
[ TIME     ] mi_malloc (119 (milli sec))              [ TIME     ] cmalloc (77 (milli sec))
[ TIME     ] mi_malloc (150 (milli sec))              [ TIME     ] cmalloc (81 (milli sec))

Test scatter sizes([8,16,32],...[1024,2048,4196]) with free -> num items: 800000, num_iterations 10
[ TIME     ] mi_malloc (43 (milli sec))               [ TIME     ] cmalloc (44 (milli sec))
[ TIME     ] mi_malloc (43 (milli sec))               [ TIME     ] cmalloc (44 (milli sec))
[ TIME     ] mi_malloc (38 (milli sec))               [ TIME     ] cmalloc (45 (milli sec))
[ TIME     ] mi_malloc (34 (milli sec))               [ TIME     ] cmalloc (46 (milli sec))
[ TIME     ] mi_malloc (38 (milli sec))               [ TIME     ] cmalloc (52 (milli sec))
[ TIME     ] mi_malloc (53 (milli sec))               [ TIME     ] cmalloc (62 (milli sec))
[ TIME     ] mi_malloc (79 (milli sec))               [ TIME     ] cmalloc (66 (milli sec))
[ TIME     ] mi_malloc (100 (milli sec))              [ TIME     ] cmalloc (74 (milli sec))
[ TIME     ] mi_malloc (120 (milli sec))              [ TIME     ] cmalloc (72 (milli sec))
[ TIME     ] mi_malloc (150 (milli sec))              [ TIME     ] cmalloc (78 (milli sec))

Test sparse sizes ([8,16,32,...1024]) with free -> num items: 800000, num_iterations 10
[ TIME     ] mi_malloc (131 (milli sec))              [ TIME     ] cmalloc (109 (milli sec))

Test sparse sizes ([8,16,32,...1024]) with free reversed -> num items: 800000, num_iterations 10
[ TIME     ] mi_malloc (131 (milli sec))              [ TIME     ] cmalloc (42 (milli sec))
```
