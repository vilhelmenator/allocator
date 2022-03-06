//
//  main.cpp
//  MemPoolTests
//
//  Created by Vilhelm Sævarsson on 25/02/2020.
//  Copyright © 2020 Vilhelm Sævarsson. All rights reserved.
//

#include <iostream>
#include <time.h>

#include "allocator.h"

/*

 mremap on systems that support it.
    - enables very fast realloc.
    - allows to move and copy memory very efficiently between thread areas.
    - only works on linux and windows.
    - macosx would have to fall back on traditional copy.
    - if mremamp avaialble
        - fast path to realloc
        - fast path to move thread memory

 each thread in the tread pool aquires a thread id for the memory to use after each run.
 enableing the system to move memory from one thread ownership over to another.
 allowing the system to copy a ptr network to another thread.
 allow one thread to inherit the memory of another.
    - but it can only free from that memory.
    - any re-allocatinos happens within its native space.

 Another random thought.
    - if a complicated structure is something that the allocator understands. Such as a tree, or a graph.
        : Can the allocator be used like a persistence util to traverse through the pointers and release them.
        : A user app could collect all pointers into an array and call free for the whole buffer.
        : Calling a destructor on an object would cause the system to defer free all the items until the destructor is done.
        : allot::destruct( obj ) -> any nested calls to destruct would cause a deferred free operation.
        : pass in a pointer to a struct. pass in a struct to describe the navigation path of a pointer tree.
        : traverse the network of pointers and pass them to their correct pools or pages.
        : destruct( ptr, schema ) -> no nested destruct calls.
        : construct( ptr, schema ) ->
 */

// [ ] test how things bleed into other partitions when one partition is filled.
// [ ] test when the allocator runs out of memory.
// [ ] test to see correct sizes being routed to correct pools and pages.
// [ ] measure the residiual leftovers of memory.
// [ ] how much of memory would be utilized by metadata.
//
//
// 49152*8 + 65536*8 = 344064 + 458752 = 802816 max 12.8gigs or 3.8gigs of pages.
// test exhausting all the pools on main thread. then on the last thread and various random threads.
// do the same for each size class.
//
// another test to test page allocations. small and large.
// allocate all the pages.
// test on various thread ids
//
// partition 1. 4 meg pages.
// 192 areas 32 megs. each has 8 sections. each section has 32 pools.
// sections = 192 * 8;  // 1536
// pools = sections*32; // 49152, 12288, 1536
//
// 64 areas * 32        // 2048
// pools = sections*32  // 65536, 16384, 2048
//
// pools can bleed into second partition
// pages can bleed into third partition.
//

//#include "include/mimalloc-override.h"  // redefines malloc etc.
const uint64_t NUMBER_OF_ITEMS = 420000L;
const uint64_t NUMBER_OF_ITERATIONS = 100UL;
const uint64_t OBJECT_SIZE = (1 << 3UL);

const uint64_t sz_kb = 1024;
const uint64_t sz_mb = sz_kb * sz_kb;
const uint64_t sz_gb = sz_kb * sz_mb;

const uint64_t section_size = 4 * sz_mb;
const uint64_t small_pool_size = 128 * sz_kb;
const uint64_t mid_pool_size = 512 * sz_kb;
const uint64_t large_pool_size = 4 * sz_mb;

const uint64_t small_page_size = 32 * sz_mb; // allocations <= 128k
const uint64_t mid_page_size = 128 * sz_mb; // allocations <= 32Mb
const uint64_t large_page_size = 256 * sz_mb; // allocations <= 128Mb

const uint64_t num_areas_part0 = (sz_gb * 2) / (32 * sz_mb);
const uint64_t num_areas_part1 = (sz_gb * 4) / (32 * sz_mb);
const uint64_t num_areas_part2 = (sz_gb * 8) / (128 * sz_mb);
const uint64_t num_areas_part3 = (sz_gb * 16) / (256 * sz_mb);

const uint64_t num_sections_part0 = (sz_gb * 2) / (4 * sz_mb);
const uint64_t num_sections_part1 = (sz_gb * 4) / (4 * sz_mb);
const uint64_t num_sections_part2 = (sz_gb * 8) / (4 * sz_mb);

const uint64_t max_small_size = 16 * sz_kb;
const uint64_t max_mid_size = 128 * sz_kb;
const uint64_t max_large_size = 2 * sz_mb;

const uint64_t max_small_size_page = 128 * sz_kb;
const uint64_t max_mid_size_page = 32 * sz_mb;
const uint64_t max_large_size_page = 128 * sz_mb;

// const int64_t total_mem = NUMBER_OF_ITEMS*OBJECT_SIZE;
// how many 16k objects to exhaust all areas for small items.
// how many large items to exhaust all areas for large items.
//
thread_local size_t Allocator::_thread_id = 0;
thread_local int32_t Allocator::_thread_idx = 0;
#define min(x, y) ((x) < (y) ? (x) : (y))
Allocator alloc;

/*
1.
Area tests.
Pools.
 [x] Collect memory on NULL in malloc and try again.
[x] Allocate all puny areas avaialable.
 [x]  test exhausting promotions.
[x] Allocate all mid areas avaiailable. > 16k < 2Mb
 [x] test exhausting promotions.
[x] each ptr returned can't be less than the size from next sectin multiple.
Pages.
 [x] test small pages.
[x] Allocate 2 - 32 megs.
 - exhaust all areas.
[x] Allocate 32 - 256 megs.
    -- exhaust all areas.
[x] Allocate slabs.
     -- exhuast all areas.
[x] Test the order of the areas.
        - if I remove an area from the list, will it alocate from the empty spot.
 perf.
 [ ] test small page coalesce rules.
 [ ] test pools vs pages for small sizes

Section test.
 - write into memory from pool/page.

 2.
[ ] random allocations sizes.
   - within a pool size class.
   - within a partition size class.
   - mix pool size classes.
   - mix partition size classes.
   - when allocator is empty.
   - when allocator is getting half full and bleeding over partitions.
   - when allocator is getting full and running out of memory.

3.
[ ] test thread free performance from multiple threads.
[ ] random allocations sizes.
   - same as previous allocation test, but with multiple threads.
   - all memory in separate threads.
   - distribute memory among threads so that each thread is freeing memory into other threads and its own.
   - distribute memory among threads so that each thread is only freeing memory into other threads.
4.
[ ] improve page allocations. ordererd lists. double free tests.
[ ] memory API. alloc and string functions.

5.
[ ] add memory block objects and implicit list alocation support.
[ ] stats. leaks.
[ ] integrate with ALLOT
[ ] test with builder. DONE!
*/
bool test_pools(size_t pool_size, size_t allocation_size)
{
    auto pools_per_section = section_size / pool_size;
    auto max_count_per_pool = (pool_size - 64) / allocation_size;
    auto num_small_sections = num_sections_part0 + num_sections_part1;
    auto num_pools = num_small_sections * pools_per_section;
    auto num_small_allocations = num_pools * max_count_per_pool;

    auto expected_reserved_mem = os_page_size * num_small_allocations; // if all pools are touched
    auto actual_reserver_mem = allocation_size * num_small_allocations; // if all the owned pages would be touched

    char *variables[num_small_allocations];

    double readable_reserved = (double)expected_reserved_mem / (SZ_GB);
    // exhaust part 0 and 1
    for (uint32_t i = 0; i < num_small_allocations; i++) {
        if (342272 == i) {
            auto bbb = 0;
        }
        void *all = alloc.malloc(allocation_size);
        variables[i] = (char *)all;
        auto end = align_up((uintptr_t)variables[i], SECTION_SIZE);
        if ((end - (uintptr_t)variables[i]) < allocation_size) {
            return false;
        }
        if (variables[i] == NULL) {
            return false;
        }
    }
    for (uint32_t i = 0; i < num_small_allocations; i++) {
        alloc.free(variables[i]);
    }

    // release all the system resources
    alloc.release_local_areas();

    num_small_sections = num_sections_part0 + num_sections_part1 + num_sections_part2;
    num_pools = num_small_sections * pools_per_section;
    num_small_allocations = num_pools * max_count_per_pool;
    expected_reserved_mem = os_page_size * num_small_allocations; // if all pools are touched
    actual_reserver_mem = allocation_size * num_small_allocations; // if all the owned pages would be touched

    readable_reserved = (double)expected_reserved_mem / (SZ_GB);
    char **variables2 = (char **)malloc(num_small_allocations * sizeof(char **));
    // exhaust part 0, 1, and 2
    for (uint32_t i = 0; i < num_small_allocations; i++) {
        variables2[i] = (char *)alloc.malloc(allocation_size);
        auto end = align_up((uintptr_t)variables2[i], SECTION_SIZE);
        if ((end - (uintptr_t)variables2[i]) < allocation_size) {
            return false;
        }
        if (variables2[i] == NULL) {
            return false;
        }
    }
    // next allocation should be NULL;
    auto nll = alloc.malloc(allocation_size);
    if (nll != NULL) {
        return false;
    }
    for (uint32_t i = 0; i < num_small_allocations; i++) {
        alloc.free(variables2[i]);
    }
    free(variables2);
    // release all the system resources
    alloc.release_local_areas();
    return true;
}

bool test_pools_small()
{
    return test_pools(small_pool_size, max_small_size);
}
bool test_medium_pools()
{
    return test_pools(mid_pool_size, max_mid_size);
}

bool test_large_pools()
{
    return test_pools(large_pool_size, max_large_size);
}

bool test_pages(size_t page_size, size_t allocation_size)
{
    auto base_parts = num_areas_part0 + num_areas_part1;
    auto extended_parts = num_areas_part0 + num_areas_part1 + num_areas_part2;
    if (page_size != small_page_size) {
        base_parts = num_areas_part2;
        extended_parts = num_areas_part2 + num_areas_part3;
    }
    if (page_size == large_page_size) {
        extended_parts = 0;
    }
    auto max_count_per_page = (page_size - 64) / allocation_size;
    auto num_small_areas = base_parts;
    auto num_small_allocations = max_count_per_page * num_small_areas;

    auto expected_reserved_mem = os_page_size * num_small_allocations; // if all pools are touched
    auto actual_reserver_mem = allocation_size * num_small_allocations; // if all the owned pages would be touched

    char *variables[num_small_allocations];

    double readable_reserved = (double)expected_reserved_mem / (SZ_GB);
    // exhaust part 0 and 1
    for (uint32_t i = 0; i < num_small_allocations; i++) {
        variables[i] = (char *)alloc.malloc_page(allocation_size);
        auto end = align_up((uintptr_t)variables[i], page_size);
        if ((end - (uintptr_t)variables[i]) < allocation_size) {
            return false;
        }
        if (variables[i] == NULL) {
            return false;
        }
    }

    if (extended_parts != 0) {
        for (uint32_t i = 0; i < num_small_allocations; i++) {
            alloc.free(variables[i]);
        }
        // release all the system resources
        alloc.release_local_areas();
        max_count_per_page = (page_size - 64) / allocation_size;
        num_small_areas = extended_parts;
        num_small_allocations = max_count_per_page * num_small_areas;
        expected_reserved_mem = os_page_size * num_small_allocations; // if all pools are touched
        actual_reserver_mem = allocation_size * num_small_allocations; // if all the owned pages would be touched

        readable_reserved = (double)expected_reserved_mem / (SZ_GB);
        char **variables2 = (char **)malloc(num_small_allocations * sizeof(char **));
        // exhaust part 0, 1, and 2
        for (uint32_t i = 0; i < num_small_allocations; i++) {
            variables2[i] = (char *)alloc.malloc_page(allocation_size);
            auto end = align_up((uintptr_t)variables2[i], page_size);
            if ((end - (uintptr_t)variables2[i]) < allocation_size) {
                return false;
            }
            if (variables2[i] == NULL) {
                return false;
            }
        }
        // next allocation should be NULL;
        auto nll = alloc.malloc_page(allocation_size);
        if (nll != NULL) {
            return false;
        }
        for (uint32_t i = 0; i < num_small_allocations; i++) {
            alloc.free(variables2[i]);
        }
        free(variables2);
    } else {
        // next allocation should be NULL;
        auto nll = alloc.malloc_page(allocation_size);
        if (nll != NULL) {
            return false;
        }
        for (uint32_t i = 0; i < num_small_allocations; i++) {
            alloc.free(variables[i]);
        }
    }
    // release all the system resources
    alloc.release_local_areas();
    return true;
}

bool test_medium_pages()
{
    return test_pages(mid_page_size, max_mid_size_page);
}

bool test_large_pages()
{
    return test_pages(large_page_size, max_large_size_page);
}

bool test_small_pages()
{
    return test_pages(small_page_size, max_small_size_page);
}

bool test_slabs()
{
    auto allocation_size = 129 * sz_mb;
    auto base_parts = num_areas_part3;
    auto max_count_per_page = 1;
    auto num_small_areas = base_parts;
    auto num_small_allocations = max_count_per_page * num_small_areas;

    auto expected_reserved_mem = os_page_size * num_small_allocations;
    auto actual_reserver_mem = allocation_size * num_small_allocations;

    char *variables[num_small_allocations];

    double readable_reserved = (double)expected_reserved_mem / (SZ_GB);
    // exhaust part 0 and 1
    for (uint32_t i = 0; i < num_small_allocations; i++) {
        variables[i] = (char *)alloc.malloc_page(allocation_size);
        auto end = align_up((uintptr_t)variables[i], 256 * sz_mb);
        if ((end - (uintptr_t)variables[i]) < allocation_size) {
            return false;
        }
        if (variables[i] == NULL) {
            return false;
        }
    }
    auto nll = alloc.malloc_page(allocation_size);
    if (nll != NULL) {
        return false;
    }
    for (uint32_t i = 0; i < num_small_allocations; i++) {
        alloc.free(variables[i]);
    }
    return true;
}

bool test_areas()
{
    auto allocation_size = 129 * sz_mb;
    auto base_parts = num_areas_part3;
    auto num_alloc = base_parts;

    char *variables[num_alloc];
    for (uint32_t i = 0; i < num_alloc; i++) {
        variables[i] = (char *)alloc.malloc(allocation_size);
    }
    auto nll = alloc.malloc_page(allocation_size);
    if (nll != NULL) {
        return false;
    }

    uintptr_t end_addr = (uintptr_t)variables[num_alloc - 2];
    uintptr_t next_addr = (uintptr_t)variables[num_alloc - 3];
    alloc.free(variables[num_alloc - 2]);
    alloc.free(variables[num_alloc - 3]);
    uintptr_t new_addr = (uintptr_t)alloc.malloc(allocation_size);
    variables[num_alloc - 3] = (char *)new_addr;
    if (new_addr != next_addr) {
        return false;
    }
    new_addr = (uintptr_t)alloc.malloc(allocation_size);
    if (new_addr != end_addr) {
        return false;
    }
    variables[num_alloc - 2] = (char *)new_addr;

    next_addr = (uintptr_t)variables[num_alloc - 5];
    // remove four and try to allocate 780 megs;
    alloc.free(variables[num_alloc - 2]);
    alloc.free(variables[num_alloc - 3]);
    alloc.free(variables[num_alloc - 4]);
    alloc.free(variables[num_alloc - 5]);
    new_addr = (uintptr_t)alloc.malloc(780 * sz_mb);
    if (new_addr != next_addr) {
        return false;
    }
    alloc.free((void *)new_addr);
    variables[num_alloc - 2] = (char *)alloc.malloc(allocation_size);
    variables[num_alloc - 3] = (char *)alloc.malloc(allocation_size);
    variables[num_alloc - 4] = (char *)alloc.malloc(allocation_size);
    variables[num_alloc - 5] = (char *)alloc.malloc(allocation_size);
    alloc.free(variables[num_alloc - 2]);
    if (alloc.malloc(256 * sz_mb) != NULL) {
        return false;
    }
    variables[num_alloc - 2] = (char *)alloc.malloc(allocation_size);
    for (uint32_t i = 0; i < num_alloc; i++) {
        alloc.free(variables[i]);
    }

    return true;
}

bool test_huge_alloc()
{
    auto gb = (uint64_t *)alloc.malloc(15 * sz_gb);
    if (gb == NULL) {
        return false;
    }
    alloc.free(gb);
    gb = (uint64_t *)alloc.malloc(16 * sz_gb);
    if (gb != NULL) {
        return false;
    }
    return true;
}
/*
ALMOST THERE. Lets get this done, today!!!
 // section.
 [ ] verify that you can write into all the memory that is resturned form a pool.
 [ ] verify that you can write into all the memory returned from a section.
 [ ] verify that you can write into all the memory returned from a page.
 [ ] verify that the last allocated part of section,pool or page does not have more space. that we are fitting as much as possible.

 // maybe complicated.
 [ ] run tests from other than the main thread.
 [ ] run tests from multiple threads.
 [ ] add thread free for pools.
 [ ] add thread free for pages.

*/

int main()
{

    /*
    if(!test_pools_small())
    {
        printf("FFFF");
    }
    if(!test_medium_pools())
    {
        printf("FFFF");
    }
    if(!test_large_pools())
    {
        printf("FFFF");
    }


    if(!test_small_pages())
    {
        printf("FFFF");
    }
    if(!test_medium_pages())
    {
        printf("FFFF");
    }
    if(!test_large_pages())
    {
        printf("FFFF");
    }

    if(!test_slabs())
    {
        printf("FFFF");
    }
    if(!test_areas())
    {
        printf("FFFF");
    }
     */
    if (!test_huge_alloc()) {
        printf("FFFF");
    }
    //[ ] test allocating too much memory.
    //    [ ] too big allocations.

    /*
    //std::cout << "total mem: " << total_mem << std::endl;
    char*t = (char*)alloc.malloc(OBJECT_SIZE);
    alloc.free(t);
    t = (char*)malloc(OBJECT_SIZE);
    free(t);
    t = (char*)mi_malloc(OBJECT_SIZE);
    mi_free(t);

    auto a = rdtsc();
    t = (char*)alloc.malloc(OBJECT_SIZE);
    auto b = rdtsc() - a;
    std::cout << "alloc.malloc Cycles : " << b << std::endl << std::endl;
    a = rdtsc();
    alloc.free(t);
    b = rdtsc() - a;
    std::cout << "alloc.free Cycles : " << b << std::endl << std::endl;


    a = rdtsc();
    t = (char*)mi_malloc(OBJECT_SIZE);
    b = rdtsc() - a;
    std::cout << "mi_malloc Cycles : " << b << std::endl << std::endl;
    a = rdtsc();
    mi_free(t);
    b = rdtsc() - a;
    std::cout << "mi_free Cycles : " << b << std::endl << std::endl;

    a = rdtsc();
    t = (char*)malloc(OBJECT_SIZE);
    b = rdtsc() - a;
    std::cout << "malloc Cycles : " << b << std::endl << std::endl;
    a = rdtsc();
    free(t);
    b = rdtsc() - a;
    std::cout << "free Cycles : " << b << std::endl << std::endl;

    char* variables[NUMBER_OF_ITEMS];
    auto t1 = std::chrono::high_resolution_clock::now();

    for(auto j=0;j<NUMBER_OF_ITERATIONS; j++)
    {
        for(auto i=0; i<NUMBER_OF_ITEMS; i++)
            variables[i] = (char*)alloc.malloc(OBJECT_SIZE);
        for(auto i=0; i<NUMBER_OF_ITEMS; i++)
            alloc.free( variables[i] );

    }
    alloc.release_local_areas();
    auto t2 = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>( t2 - t1 ).count();
    std::cout << "Time spent in alloc: " << duration << " milliseconds" << std::endl << std::endl;

    t1 = std::chrono::high_resolution_clock::now();
    for(int j=0;j<NUMBER_OF_ITERATIONS; j++)
    {
       for(int i=0; i<NUMBER_OF_ITEMS; i++)
           variables[i] = (char*)mi_malloc(OBJECT_SIZE);

        for(int i=0; i<NUMBER_OF_ITEMS; i++)
            mi_free( variables[i] );
    }
    t2 = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast<std::chrono::milliseconds>( t2 - t1 ).count();
    std::cout << "Time spent in mi_malloc: " << duration << " milliseconds" << std::endl << std::endl;

    t1 = std::chrono::high_resolution_clock::now();
    for(auto j=0;j<NUMBER_OF_ITERATIONS; j++)
    {
       for(auto i=0; i<NUMBER_OF_ITEMS; i++)
           variables[i] = (char*)malloc(OBJECT_SIZE);

        for(auto i=0; i<NUMBER_OF_ITEMS; i++)
            free( variables[i] );
    }
    t2 = std::chrono::high_resolution_clock::now();
    duration = std::chrono::duration_cast<std::chrono::milliseconds>( t2 - t1 ).count();
    std::cout << "Time spent in system malloc: " << duration << " milliseconds" << std::endl << std::endl;
    */
    return 0;
}
