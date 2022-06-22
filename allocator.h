/*
 // Finish something!!!!
 // part 1
 [ ] - reallocate.

 // part 2
 [ ] - partition set testing.
 [ ] - thread alloc/free tests. Test queues.

 // part 3.
 [ ] - allocation benchmarks.
    * various benchmarks
    * missing API functions.
    * aligned allocations.
    * string functions.

 // part 4
 [ ] - Heap Allocation improvements. Sorted pools.
 [ ] - Remapping for 4k page allocations.
 [ ] - 32 bit support.
    * 64 thread_count
    * 3 gig range.
 [ ] - publish to github and wrap up.
 ------------------------------------------
 [ ] - Sorted Pool for resizing structures.
 [ ] - Additional API utils.
 */
#pragma once
#ifndef _allocator_h_
#define _allocator_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SZ_KB 1024ULL
#define SZ_MB (SZ_KB * SZ_KB)
#define SZ_GB (SZ_MB * SZ_KB)

#define DEFAULT_OS_PAGE_SIZE 4096ULL
#define SECTION_SIZE (1ULL << 22ULL)

#define BASE_AREA_SIZE (SECTION_SIZE * 8ULL) // 32Mb
#define AREA_SIZE_SMALL BASE_AREA_SIZE
#define AREA_SIZE_MEDIUM (SECTION_SIZE * 16ULL) // 64Mb
#define AREA_SIZE_LARGE (SECTION_SIZE * 32ULL)  // 128Mb
#define AREA_SIZE_HUGE (SECTION_SIZE * 64ULL)   // 256Mb
#define NUM_AREA_PARTITIONS 4

typedef enum AreaType_t {
    AT_FIXED_32 = 0,  //  small allocations, mostly pools.
    AT_FIXED_64 = 1,  //
    AT_FIXED_128 = 2, //  larger allocations, but can also contain pools and sections.
    AT_FIXED_256 = 3, //  heap allocations only
    AT_VARIABLE = 4   //  not a fixed size area. found in extended partitions.
} AreaType;

static const uintptr_t partitions_offsets[] = {
    ((uintptr_t)2 << 40), // allocations smaller than SECTION_MAX_MEMORY
    ((uintptr_t)4 << 40),
    ((uintptr_t)8 << 40),   // SECTION_MAX_MEMORY < x < AREA_MAX_MEMORY
    ((uintptr_t)16 << 40),  // AREA_MAX_MEMORY < x < 1GB
    ((uintptr_t)32 << 40),  // resource allocations.
    ((uintptr_t)64 << 40),  // Huge allocations
    ((uintptr_t)128 << 40), // end
};
static const uintptr_t area_type_to_size[] = {AREA_SIZE_SMALL, AREA_SIZE_MEDIUM, AREA_SIZE_LARGE, AREA_SIZE_HUGE,
                                              UINT64_MAX};
static inline uint64_t area_size_from_partition_id(uint8_t pid) { return area_type_to_size[pid]; }

static inline int8_t partition_from_addr(uintptr_t p)
{
    static const uint8_t partition_count = 7;
    const int lz = 22 - __builtin_clz(p >> 32);
    if (lz < 0 || lz > partition_count) {
        return -1;
    } else {
        return lz;
    }
}

static inline uint64_t area_size_from_addr(uintptr_t p) { return area_size_from_partition_id(partition_from_addr(p)); }

typedef enum ContainerType_e { CT_SECTION = 16, CT_HEAP = 32, CT_SLAB = 64 } ContainerType;
typedef enum SectionType_e { ST_HEAP_4M = 0, ST_POOL_128K = 1, ST_POOL_512K = 2, ST_POOL_4M = 3 } SectionType;
typedef enum HeapType_e { HT_4M = 22, HT_32M = 25, HT_64M = 26, HT_128M = 27, HT_256M = 28 } HeapType;

typedef union Bitmask_u
{
    uint64_t whole;
    uint32_t _w32[2];
} Bitmask;

typedef struct Area_t
{
    uint64_t partition_mask; // id, container type, area_type, num
    Bitmask constr_mask;     // containers that have been constructed.
    Bitmask active_mask;     // containers that cant be destructed.
} Area;

typedef struct Block_t
{
    struct Block_t *next;
} Block;

typedef struct Queue_t
{
    void *head;
    void *tail;
} Queue;

typedef struct Section_t
{
    uint64_t partition_mask; // area inherited values
    Bitmask constr_mask;     // which parts have been touched
    Bitmask active_mask;     // which parts are being used

    SectionType type;
    int32_t idx;

    // links to sections.
    struct Section_t *prev;
    struct Section_t *next;

    uint8_t collections[1];
} Section;

typedef struct Pool_t
{
    int32_t idx;        // index in the parent section
    uint32_t block_idx; // index into the pool queue. What size class do you belong to.
    uint32_t block_size;
    int32_t num_available;
    int32_t num_committed;
    int32_t num_used;
    Block *free;
    struct Pool_t *prev;
    struct Pool_t *next;
    uint8_t blocks[1];
} Pool;

typedef struct Heap_t
{
    int32_t idx;           // index into the parent section/if in a section.
    uint32_t total_memory; // how much do we have available in total
    uint32_t used_memory;  // how much have we used
    uint32_t min_block;    // what is the minum size block available;
    uint32_t max_block;    // what is the maximum size block available;
    uint32_t num_allocations;

    uint8_t *start;
    Queue free_nodes;

    struct Heap_t *prev;
    struct Heap_t *next;

    uint8_t blocks[1];
} Heap;

// DONE
void *cmalloc(size_t s);
void cfree(void *p);

bool callocator_release(void);
void *cmalloc_at(size_t s, uintptr_t vm_addr);
void *cmalloc_area(size_t s, size_t partition_idx);
void *cmalloc_os(size_t s);
void *cmalloc_from_heap(size_t s);

// NOT DONE
void *crealloc(void *p, size_t s);

void callocator_purge(void);
void *cmalloc_aligned(size_t s, size_t alignment);

void *cmalloc_from_pool(size_t s);
//
// basic API
//
// cmalloc(  )
// cfree(  )
// crealloc(  )
// ccmalloc(  )
// cmalloc_aligned(  )
//

//
// extended API.
//
// cmalloc_pool(  )
// cmalloc_heap(  )
// cmalloc_area(  )
// cmalloc_at(  )       -- allocate memory at a particular address within a certain range.
// cmalloc_bound(  )    -- bounds the memory allocated by read_only pages that will cause exceptions if touched.
// cpurge(  )   -- remove all memory reserved by thread.        Will release all allocations it got from the OS.
// crelease(  ) -- release all memory that is unused by thread. Will release all the allocations that are not in use.
// creset(  )  -- reset all memory used by current thread.     Will reset OS pages that it has touched. Without
// releasing allocation structures. creport(  )  -- log amount of used memory for particular thread.

#endif /* _allocator_h_ */
