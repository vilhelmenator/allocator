/*
TODO
 [ ] - partition set testing.
 [ ] - thread alloc/free tests. Test queues.
 [ ] - allocation benchmarks.
    * various benchmarks
    * missing API functions.
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
#ifndef _alloc_h_
#define _alloc_h_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SZ_KB 1024ULL
#define SZ_MB (SZ_KB * SZ_KB)
#define SZ_GB (SZ_MB * SZ_KB)

#define DEFAULT_OS_PAGE_SIZE 4096ULL
#define SECTION_SIZE (1ULL << 22ULL)

static const uintptr_t partitions_offsets[] = {
    ((uintptr_t)2 << 40), // allocations smaller than SECTION_MAX_MEMORY
    ((uintptr_t)4 << 40),
    ((uintptr_t)8 << 40),   // SECTION_MAX_MEMORY < x < AREA_MAX_MEMORY
    ((uintptr_t)16 << 40),  // AREA_MAX_MEMORY < x < 1GB
    ((uintptr_t)32 << 40),  // resource allocations.
    ((uintptr_t)64 << 40),  // Huge allocations
    ((uintptr_t)128 << 40), // end
};

typedef enum AreaType_t {
    AT_FIXED_32 = 0,  //  small allocations, mostly pools.
    AT_FIXED_64 = 1,  //
    AT_FIXED_128 = 2, //  larger allocations, but can also contain pools and sections.
    AT_FIXED_256 = 3, //  heap allocations only
    AT_VARIABLE = 4   //  not a fixed size area. found in extended partitions.
} AreaType;

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

struct Allocator_t;
typedef struct Allocator_t Allocator;

int32_t get_heap_size_class(size_t s);
int8_t partition_from_addr(uintptr_t p);
uint64_t area_size_from_addr(uintptr_t p);
void *alloc_memory_aligned(void *base, uintptr_t end, size_t size, size_t alignment);
bool free_memory(void *ptr, size_t size);

void *allocator_malloc(Allocator *a, size_t s);
void *allocator_malloc_heap(Allocator *a, size_t s);
void allocator_free(Allocator *a, void *p);
bool allocator_release_local_areas(Allocator *a);

Allocator *allocator_get_thread_instance(void);

void *cmalloc(size_t s);
void cfree(void *p);

#endif /* Malloc_h */
