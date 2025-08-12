# High-Performance General Purpose Allocator

## Overview
A two-tiered memory allocator designed for extreme performance. The system combines partition-based bulk memory management with thread-local caching for optimal allocation speed. It reduces OS level fragmentation by managing large virtual memory blocks by the backend, and effectively eliminates any fragmentation at the front-end with proper block management.

## Architecture

### 1. Backend: Partition Allocator
Manages large memory regions through size-partitioned blocks.

**Key Structures:**
```c
typedef struct {
    _Atomic(uint64_t) reserved;     // Track reserved parts
    _Atomic(uint64_t) committed;    // Track committed virtual memory
    _Atomic(uint64_t) ranges;       // Memory extents
    _Atomic(uint64_t) active;       // which regions live in local caches
    _Atomic(uint64_t) abandoned;    // which regions have been abandoned by dead threads
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
   
4. **>256MB requests**: Forwarded to OS


## Thread Handling

- Each arena marked with owner thread_id
- Cross-thread frees handled via:
  - Counters for general case
  - Atomic lists for urgent reallocation
  - Atomic masks for the arenas
- Thread termination:
  - Orphaned arenas/implicit_lists marked (thread_id = -1)
  - Marked as abandoned in the partition allocator
  - Completely unused arenas released
  - Others gradually adopted by allocating threads when freed

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

Parts of the allocator that are still in development.
- Improve the build phase of the project so it can be easily tested by anyone.
- Better release strategy to reduce the resident pages
- More tests and better metrics to compare against other allocators
---

## Build and run

  clang *c -o test -O3
  
  ./test

## mi_malloc test comparisons.

clang -O3 -march=native -flto=thin -fomit-frame-pointer -fno-exceptions -fno-rtti -fvisibility=hidden   -mno-red-zone -ftls-model=initial-exec -O3 -W  -DNDEBUG -g0  -o test  *c ../mimalloc-master/src/static.c -I ../mimalloc-master/include -DMI_DEBUG=0

./test mi_malloc
```
 Test with free -> size: [1,..8192], num items: 800000, num_iterations 10              
[ TIME     ] cmalloc (51 (milli sec))      [ TIME     ] mi_malloc (56 (milli sec))          [ TIME     ] malloc (394 (milli sec))
[ TIME     ] cmalloc (40 (milli sec))      [ TIME     ] mi_malloc (46 (milli sec))          [ TIME     ] malloc (374 (milli sec))
[ TIME     ] cmalloc (42 (milli sec))      [ TIME     ] mi_malloc (51 (milli sec))          [ TIME     ] malloc (447 (milli sec))
[ TIME     ] cmalloc (48 (milli sec))      [ TIME     ] mi_malloc (52 (milli sec))          [ TIME     ] malloc (529 (milli sec))
[ TIME     ] cmalloc (60 (milli sec))      [ TIME     ] mi_malloc (99 (milli sec))          [ TIME     ] malloc (635 (milli sec))
[ TIME     ] cmalloc (92 (milli sec))      [ TIME     ] mi_malloc (115 (milli sec))         [ TIME     ] malloc (817 (milli sec))
[ TIME     ] cmalloc (92 (milli sec))      [ TIME     ] mi_malloc (132 (milli sec))         [ TIME     ] malloc (1 seconds : 35 (milli sec))
[ TIME     ] cmalloc (127 (milli sec))     [ TIME     ] mi_malloc (194 (milli sec))         [ TIME     ] malloc (327 (milli sec))
[ TIME     ] cmalloc (196 (milli sec))     [ TIME     ] mi_malloc (253 (milli sec))         [ TIME     ] malloc (352 (milli sec))
[ TIME     ] cmalloc (197 (milli sec))     [ TIME     ] mi_malloc (391 (milli sec))         [ TIME     ] malloc (380 (milli sec))
[ TIME     ] cmalloc (349 (milli sec))     [ TIME     ] mi_malloc (652 (milli sec))         [ TIME     ] malloc (441 (milli sec))
Committed pages post 500935      			Committed pages post 403280               				Committed pages post 3525

Test with free -> size: [32k,..512k], num items: 800000, num_iterations 10
[ TIME     ] cmalloc (62 (milli sec))      [ TIME     ] mi_malloc (50 (milli sec))          [ TIME     ] malloc (103 (milli sec))
[ TIME     ] cmalloc (70 (milli sec))      [ TIME     ] mi_malloc (130 (milli sec))         [ TIME     ] malloc (125 (milli sec))
[ TIME     ] cmalloc (85 (milli sec))      [ TIME     ] mi_malloc (372 (milli sec))         [ TIME     ] malloc (3 seconds : 939 (milli sec))
[ TIME     ] cmalloc (111 (milli sec))     [ TIME     ] mi_malloc (454 (milli sec))         [ TIME     ] malloc (11 seconds : 799 (milli sec))
Committed pages post 511071      			Committed pages post 517391               				Committed pages post 2029

Test with immediate free -> size: [8,..8192], num items: 800000, num_iterations 10 
[ TIME     ] cmalloc (35 (milli sec))      [ TIME     ] mi_malloc (43 (milli sec))          [ TIME     ] malloc (2 (milli sec))
[ TIME     ] cmalloc (35 (milli sec))      [ TIME     ] mi_malloc (43 (milli sec))          [ TIME     ] malloc (2 (milli sec))
[ TIME     ] cmalloc (35 (milli sec))      [ TIME     ] mi_malloc (43 (milli sec))          [ TIME     ] malloc (2 (milli sec))
[ TIME     ] cmalloc (35 (milli sec))      [ TIME     ] mi_malloc (43 (milli sec))          [ TIME     ] malloc (2 (milli sec))
[ TIME     ] cmalloc (35 (milli sec))      [ TIME     ] mi_malloc (44 (milli sec))          [ TIME     ] malloc (2 (milli sec))
[ TIME     ] cmalloc (35 (milli sec))      [ TIME     ] mi_malloc (45 (milli sec))          [ TIME     ] malloc (2 (milli sec))
[ TIME     ] cmalloc (35 (milli sec))      [ TIME     ] mi_malloc (45 (milli sec))          [ TIME     ] malloc (2 (milli sec))
[ TIME     ] cmalloc (35 (milli sec))      [ TIME     ] mi_malloc (51 (milli sec))          [ TIME     ] malloc (2 (milli sec))
[ TIME     ] cmalloc (36 (milli sec))      [ TIME     ] mi_malloc (55 (milli sec))          [ TIME     ] malloc (2 (milli sec))
[ TIME     ] cmalloc (35 (milli sec))      [ TIME     ] mi_malloc (52 (milli sec))          [ TIME     ] malloc (2 (milli sec))
[ TIME     ] cmalloc (35 (milli sec))      [ TIME     ] mi_malloc (56 (milli sec))          [ TIME     ] malloc (2 (milli sec))
[ TIME     ] cmalloc (35 (milli sec))      [ TIME     ] mi_malloc (93 (milli sec))          [ TIME     ] malloc (2 (milli sec))
[ TIME     ] cmalloc (35 (milli sec))      [ TIME     ] mi_malloc (93 (milli sec))          [ TIME     ] malloc (2 (milli sec))
[ TIME     ] cmalloc (35 (milli sec))      [ TIME     ] mi_malloc (92 (milli sec))          [ TIME     ] malloc (2 (milli sec))

Test with free -> size: [8192,..8], num items: 800000, num_iterations 10  
[ TIME     ] cmalloc (92 (milli sec))      [ TIME     ] mi_malloc (388 (milli sec))         [ TIME     ] malloc (430 (milli sec))
[ TIME     ] cmalloc (72 (milli sec))      [ TIME     ] mi_malloc (243 (milli sec))         [ TIME     ] malloc (368 (milli sec))
[ TIME     ] cmalloc (62 (milli sec))      [ TIME     ] mi_malloc (190 (milli sec))         [ TIME     ] malloc (341 (milli sec))
[ TIME     ] cmalloc (59 (milli sec))      [ TIME     ] mi_malloc (141 (milli sec))         [ TIME     ] malloc (333 (milli sec))
[ TIME     ] cmalloc (61 (milli sec))      [ TIME     ] mi_malloc (116 (milli sec))         [ TIME     ] malloc (1 seconds : 45 (milli sec))
[ TIME     ] cmalloc (55 (milli sec))      [ TIME     ] mi_malloc (109 (milli sec))         [ TIME     ] malloc (800 (milli sec))
[ TIME     ] cmalloc (52 (milli sec))      [ TIME     ] mi_malloc (89 (milli sec))          [ TIME     ] malloc (615 (milli sec))
[ TIME     ] cmalloc (42 (milli sec))      [ TIME     ] mi_malloc (50 (milli sec))          [ TIME     ] malloc (528 (milli sec))
[ TIME     ] cmalloc (42 (milli sec))      [ TIME     ] mi_malloc (46 (milli sec))          [ TIME     ] malloc (441 (milli sec))
[ TIME     ] cmalloc (39 (milli sec))      [ TIME     ] mi_malloc (44 (milli sec))          [ TIME     ] malloc (384 (milli sec))
[ TIME     ] cmalloc (38 (milli sec))      [ TIME     ] mi_malloc (44 (milli sec))          [ TIME     ] malloc (382 (milli sec))
[ TIME     ] cmalloc (39 (milli sec))      [ TIME     ] mi_malloc (43 (milli sec))          [ TIME     ] malloc (378 (milli sec))
[ TIME     ] cmalloc (38 (milli sec))      [ TIME     ] mi_malloc (43 (milli sec))          [ TIME     ] malloc (386 (milli sec))
[ TIME     ] cmalloc (38 (milli sec))      [ TIME     ] mi_malloc (43 (milli sec))          [ TIME     ] malloc (390 (milli sec))

Test scatter sizes([8,16,32],...[1024,2048,4196]) with free -> num items: 800000, num_iterations 10  
[ TIME     ] cmalloc (57 (milli sec))      [ TIME     ] mi_malloc (44 (milli sec))          [ TIME     ] malloc (383 (milli sec))
[ TIME     ] cmalloc (58 (milli sec))      [ TIME     ] mi_malloc (44 (milli sec))          [ TIME     ] malloc (378 (milli sec))
[ TIME     ] cmalloc (79 (milli sec))      [ TIME     ] mi_malloc (38 (milli sec))          [ TIME     ] malloc (387 (milli sec))
[ TIME     ] cmalloc (73 (milli sec))      [ TIME     ] mi_malloc (31 (milli sec))          [ TIME     ] malloc (420 (milli sec))
[ TIME     ] cmalloc (88 (milli sec))      [ TIME     ] mi_malloc (35 (milli sec))          [ TIME     ] malloc (453 (milli sec))
[ TIME     ] cmalloc (74 (milli sec))      [ TIME     ] mi_malloc (50 (milli sec))          [ TIME     ] malloc (554 (milli sec))
[ TIME     ] cmalloc (86 (milli sec))      [ TIME     ] mi_malloc (77 (milli sec))          [ TIME     ] malloc (631 (milli sec))
[ TIME     ] cmalloc (73 (milli sec))      [ TIME     ] mi_malloc (94 (milli sec))          [ TIME     ] malloc (730 (milli sec))
[ TIME     ] cmalloc (86 (milli sec))      [ TIME     ] mi_malloc (115 (milli sec))         [ TIME     ] malloc (783 (milli sec))
[ TIME     ] cmalloc (74 (milli sec))      [ TIME     ] mi_malloc (150 (milli sec))         [ TIME     ] malloc (568 (milli sec))

Test scatter sizes([8,16,32],...[1024,2048,4196]) with free -> num items: 800000, num_iterations 10  
[ TIME     ] cmalloc (60 (milli sec))      [ TIME     ] mi_malloc (44 (milli sec))          [ TIME     ] malloc (381 (milli sec))
[ TIME     ] cmalloc (60 (milli sec))      [ TIME     ] mi_malloc (44 (milli sec))          [ TIME     ] malloc (381 (milli sec))
[ TIME     ] cmalloc (80 (milli sec))      [ TIME     ] mi_malloc (38 (milli sec))          [ TIME     ] malloc (364 (milli sec))
[ TIME     ] cmalloc (73 (milli sec))      [ TIME     ] mi_malloc (30 (milli sec))          [ TIME     ] malloc (413 (milli sec))
[ TIME     ] cmalloc (85 (milli sec))      [ TIME     ] mi_malloc (35 (milli sec))          [ TIME     ] malloc (459 (milli sec))
[ TIME     ] cmalloc (74 (milli sec))      [ TIME     ] mi_malloc (50 (milli sec))          [ TIME     ] malloc (545 (milli sec))
[ TIME     ] cmalloc (85 (milli sec))      [ TIME     ] mi_malloc (74 (milli sec))          [ TIME     ] malloc (626 (milli sec))
[ TIME     ] cmalloc (76 (milli sec))      [ TIME     ] mi_malloc (93 (milli sec))          [ TIME     ] malloc (733 (milli sec))
[ TIME     ] cmalloc (86 (milli sec))      [ TIME     ] mi_malloc (127 (milli sec))         [ TIME     ] malloc (785 (milli sec))
[ TIME     ] cmalloc (73 (milli sec))      [ TIME     ] mi_malloc (150 (milli sec))         [ TIME     ] malloc (565 (milli sec))

Test sparse sizes ([rand()%1024]) with free -> num items: 800000, num_iterations 10
[ TIME     ] cmalloc (130 (milli sec))     [ TIME     ] mi_malloc (162 (milli sec))         [ TIME     ] malloc (978 (milli sec))
Test sparse sizes ([rand()%1024]) with free reversed -> num items: 800000, num_iterations 10 
[ TIME     ] cmalloc (108 (milli sec))     [ TIME     ] mi_malloc (161 (milli sec))         [ TIME     ] malloc (977 (milli sec))
Test sparse sizes ([rand()%1024*1024]) with free reversed -> num items: 80000, num_iterations 10 
[ TIME     ] cmalloc (13 (milli sec))      [ TIME     ] mi_malloc (1 seconds : 164 (milli sec)) [ TIME     ] malloc (11 seconds : 472 (milli sec))
```
