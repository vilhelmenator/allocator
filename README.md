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

