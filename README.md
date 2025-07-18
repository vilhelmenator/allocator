# Efficient Allocator for Geometry Tools

This project aims to develop an efficient memory allocator tailored for geometry tools. The memory is organized into partitions, with each partition reserved for a specific thread, making it ideal for thread-pools with a fixed number of threads.

## Memory Architecture

Each thread's memory area is divided into 8 partitions, and each partition is dedicated to specific size ranges. Further, each partition contains 64 arenas, and each arena is segmented into 64 segments. These segments can either house a pool of smaller allocations or allocate entire segments for parts.

## Allocation Strategy

Memory allocation within each arena is driven by size and alignment requirements:

- Default pool sizes are allocated to arenas of corresponding sizes.
- If a pool's block size is a power of two, it aligns to that specific size.

### Size Classes and Allocation Details

| Size Classes                                                     | Part Size | Arena Size |
| ---------------------------------------------------------------- | --------- | ---------- |
| 8, 16, 24, 32, 40, 48, 56, 64                                   | 64k       | 4MB        |
| 72, 80, 88, 96, 104, 112, 120, 128                               | 128k      | 8MB        |
| 144, 160, 176, 192, 208, 224, 240, 256                           | 256k      | 16MB       |
| 288, 320, 352, 384, 416, 448, 480, 512                           | 512k      | 32MB       |
| 576, 640, 704, 768, 832, 896, 960, 1024                          | 1MB       | 64MB       |
| 1152, 1280, 1408, 1536, 1664, 1792, 1920, 2048                   | 2MB       | 128MB      |
| 2304 --- 32,768                                                  | 4MB       | 256MB      |
| Reserved for special allocations                                 | 8MB       | 512MB      |

- Allocations larger than 32k are handled by assigning specific arena parts.
- Allocations larger than 2MB are allocated in whole arenas.
- For allocations exceeding 4GB, memory is obtained from higher address ranges.
- When a thread exhausts its current partition, a new one is allocated.
- Address ranges and alignments identify the thread partition and sub-partition associated with each memory address.
- Parent containers are accessed by aligning addresses down to their size class.
- Freed addresses are added to a deferred list for processing.

## Additional Features

- Pools compute relative offsets for addresses within free lists and remove entries with out-of-scope addresses.
- Functionality for managing larger allocations is in development.

## Build and run

  clang *c -o test -O3
  
  ./test

## mi_malloc test comparisons.

clang *c ../mimalloc-master/src/static.c -I ../mimalloc-master/include -O3 -DMI_DEBUG=0 -o test

./test mi_malloc
```
| Test with free -> size: [8,..8192], num items: 800000, num_iterations 10                  |
[ TIME     ] mi_malloc (68 (milli sec))               [ TIME     ] cmalloc (57 (milli sec))
[ TIME     ] mi_malloc (45 (milli sec))               [ TIME     ] cmalloc (40 (milli sec))
[ TIME     ] mi_malloc (44 (milli sec))               [ TIME     ] cmalloc (39 (milli sec))
[ TIME     ] mi_malloc (44 (milli sec))               [ TIME     ] cmalloc (38 (milli sec))
[ TIME     ] mi_malloc (44 (milli sec))               [ TIME     ] cmalloc (40 (milli sec))
[ TIME     ] mi_malloc (48 (milli sec))               [ TIME     ] cmalloc (40 (milli sec))
[ TIME     ] mi_malloc (56 (milli sec))               [ TIME     ] cmalloc (45 (milli sec))
[ TIME     ] mi_malloc (103 (milli sec))              [ TIME     ] cmalloc (63 (milli sec))
[ TIME     ] mi_malloc (118 (milli sec))              [ TIME     ] cmalloc (75 (milli sec))
[ TIME     ] mi_malloc (144 (milli sec))              [ TIME     ] cmalloc (96 (milli sec))
[ TIME     ] mi_malloc (179 (milli sec))              [ TIME     ] cmalloc (127 (milli sec))
[ TIME     ] mi_malloc (265 (milli sec))              [ TIME     ] cmalloc (204 (milli sec))
[ TIME     ] mi_malloc (373 (milli sec))              [ TIME     ] cmalloc (207 (milli sec))
[ TIME     ] mi_malloc (632 (milli sec))              [ TIME     ] cmalloc (361 (milli sec))

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
[ TIME     ] mi_malloc (131 (milli sec))              [ TIME     ] cmalloc (499 (milli sec))

Test sparse sizes ([8,16,32,...1024]) with free reversed -> num items: 800000, num_iterations 10
[ TIME     ] mi_malloc (131 (milli sec))              [ TIME     ] cmalloc (380 (milli sec))
```
