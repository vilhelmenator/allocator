//
//  main.cpp
//  MemPoolTests
//
//  Created by Vilhelm Sævarsson on 25/02/2020.
//  Copyright © 2020 Vilhelm Sævarsson. All rights reserved.
//

//
// at start we register our thread local stuff
//  set callbacks for when threads die
//  remove when atexit is called.
//  atexit(&process_done);
/*
// Called when the process is done (through `at_exit`)
static void mi_process_done(void) {
  // only shutdown if we were initialized
  if (!_mi_process_is_initialized) return;
  // ensure we are called once
  static bool process_done = false;
  if (process_done) return;
  process_done = true;

  #if defined(_WIN32) && !defined(MI_SHARED_LIB)
  FlsSetValue(mi_fls_key, NULL);  // don't call main-thread callback
  FlsFree(mi_fls_key);            // call thread-done on all threads to prevent dangling callback pointer if statically
linked with a DLL; Issue #208 #endif mi_collect(true);
}

*/
/*
int tls_create(tss_t *key, tss_dtor_t dtor)
{
    if (!key) return thrd_error;
    *key = TlsAlloc();
    if (dtor) {
        if (impl_tss_dtor_register(*key, dtor)) {
            TlsFree(*key);
            return thrd_error;
        }
    }
    return (*key != 0xFFFFFFFF) ? thrd_success : thrd_error;
}

// 7.25.6.2
void tss_delete(tss_t key)
{
    TlsFree(key);
}

// 7.25.6.3
void *tss_get(tss_t key)
{
    return TlsGetValue(key);
}

// 7.25.6.4
int tss_set(tss_t key, void *val)
{
    return TlsSetValue(key, val) ? thrd_success : thrd_error;
}
*/
#define CTEST_ENABLED
#include "../ctest/ctest.h"
#include "allocator.h"
//#include <iostream>

/*

 mremap on systems that support it.
    - enables very fast realloc.
    - allows to move and copy memory very efficiently between thread areas.
    - windows has its own unique api.
    - mach os has vm_remap functions that could be used.
    - if mremamp avaialble
        - fast path to realloc
        - fast path to move thread memory
        - very fast realloc of aligned memory.

 each thread in the tread pool aquires a thread id for the memory to use after
 each run. enableing the system to move memory from one thread ownership over to
 another. allowing the system to copy a ptr network to another thread. allow one
 thread to inherit the memory of another.
    - but it can only free from that memory.
    - any re-allocatinos happens within its native space.

 Another random thought.
    - if a complicated structure is something that the allocator understands.
 Such as a tree, or a graph. : Can the allocator be used like a persistence util
 to traverse through the pointers and release them. : A user app could collect
 all pointers into an array and call free for the whole buffer. : Calling a
 destructor on an object would cause the system to defer free all the items
 until the destructor is done. : allot::destruct( obj ) -> any nested calls to
 destruct would cause a deferred free operation. : pass in a pointer to a
 struct. pass in a struct to describe the navigation path of a pointer tree. :
 traverse the network of pointers and pass them to their correct pools or pages.
        : destruct( ptr, schema ) -> no nested destruct calls.
        : construct( ptr, schema ) ->
 */

//
//
// 49152*8 + 65536*8 = 344064 + 458752 = 802816 max 12.8gigs or 3.8gigs of
// pages. test exhausting all the pools on main thread. then on the last thread
// and various random threads. do the same for each size class.
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
const uint64_t NUMBER_OF_ITEMS = 800000L;
const uint64_t NUMBER_OF_ITERATIONS = 10UL;
const uint64_t OBJECT_SIZE = (1 << 3UL);

const uint64_t sz_kb = 1024;
const uint64_t sz_mb = sz_kb * sz_kb;
const uint64_t sz_gb = sz_kb * sz_mb;

const uint64_t section_size = 4 * sz_mb;
const uint64_t small_pool_size = 128 * sz_kb;
const uint64_t mid_pool_size = 512 * sz_kb;
const uint64_t large_pool_size = 4 * sz_mb;

const uint64_t small_page_size = 32 * sz_mb;  // allocations <= 128k
const uint64_t mid_page_size = 128 * sz_mb;   // allocations <= 32Mb
const uint64_t large_page_size = 256 * sz_mb; // allocations <= 128Mb

const uint64_t num_areas_part0 = (sz_gb * 2) / (32 * sz_mb);
const uint64_t num_areas_part1 = (sz_gb * 4) / (32 * sz_mb);
const uint64_t num_areas_part2 = (sz_gb * 8) / (128 * sz_mb);
const uint64_t num_areas_part3 = (sz_gb * 16) / (256 * sz_mb);

const uint64_t num_sections_part0 = (sz_gb * 2) / (4 * sz_mb);
const uint64_t num_sections_part1 = (sz_gb * 4) / (4 * sz_mb);
const uint64_t num_sections_part2 = (sz_gb * 8) / (4 * sz_mb);

const uint64_t max_small_size = 16 * sz_kb - 1;
const uint64_t max_mid_size = 128 * sz_kb - 1;
const uint64_t max_large_size = 2 * sz_mb - 1;

const uint64_t max_small_size_page = 128 * sz_kb;
const uint64_t max_mid_size_page = 32 * sz_mb;
const uint64_t max_large_size_page = 128 * sz_mb;

// const int64_t total_mem = NUMBER_OF_ITEMS*OBJECT_SIZE;
// how many 16k objects to exhaust all areas for small items.
// how many large items to exhaust all areas for large items.
//
// thread_init::thread_init() { allocator_main_index = reserve_any_partition_set(); }

// thread_init::~thread_init() { release_partition_set(allocator_main_index); }

// malloc calls init thread
//
// auto vv = allocator::malloc(2);
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
        - if I remove an area from the list, will it alocate from the empty
spot.


[ ] Allocation of an area fails.
[ ] Allocation can't inherit a new partition set
[ ] thread-free

 perf.
 [ ] test small page coalesce rules.
 [ ] test pools vs pages for small sizes


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
   - distribute memory among threads so that each thread is freeing memory into
other threads and its own.
   - distribute memory among threads so that each thread is only freeing memory
into other threads. 4. [ ] improve page allocations. ordererd lists. double free
tests. [ ] memory API. alloc and string functions.

5.
[ ] add memory block objects and implicit list alocation support.
[ ] stats. leaks.
[ ] integrate with ALLOT
[ ] test with builder. DONE!
*/
bool test_pools(size_t pool_size, size_t allocation_size)
{
    Allocator *alloc = allocator_get_thread_instance();
    uint64_t pools_per_section = section_size / pool_size;
    uint64_t max_count_per_pool = (pool_size - 64) / allocation_size;
    uint64_t num_small_sections = num_sections_part0 + num_sections_part1;
    uint64_t num_pools = num_small_sections * pools_per_section;
    uint64_t num_small_allocations = num_pools * max_count_per_pool;

    uint64_t expected_reserved_mem = os_page_size * num_small_allocations;  // if all pools are touched
    uint64_t actual_reserver_mem = allocation_size * num_small_allocations; // if all the owned pages would be touched

    uint64_t **variables = (uint64_t **)malloc(num_small_allocations * sizeof(uint64_t));

    double readable_reserved = (double)expected_reserved_mem / (SZ_GB);
    // exhaust part 0 and 1
    for (uint32_t i = 0; i < num_small_allocations; i++) {
        void *all = allocator_malloc(alloc, allocation_size);
        if (all == NULL) {
            return false;
        }
        variables[i] = (uint64_t *)all;
        uintptr_t end = align_up((uintptr_t)variables[i], SECTION_SIZE);
        if ((end - (uintptr_t)variables[i]) < allocation_size) {
            return false;
        }
        if (variables[i] == NULL) {
            return false;
        }
    }
    for (uint32_t i = 0; i < num_small_allocations; i++) {
        allocator_free(alloc, variables[i]);
    }

    // release all the system resources
    allocator_release_local_areas(alloc);
    free(variables);
    num_small_sections = num_sections_part0 + num_sections_part1 + num_sections_part2;
    num_pools = num_small_sections * pools_per_section;
    num_small_allocations = num_pools * max_count_per_pool;
    expected_reserved_mem = os_page_size * num_small_allocations;  // if all pools are touched
    actual_reserver_mem = allocation_size * num_small_allocations; // if all the owned pages would be touched

    readable_reserved = (double)expected_reserved_mem / (SZ_GB);
    uint64_t **variables2 = (uint64_t **)malloc(num_small_allocations * sizeof(uint64_t));
    // exhaust part 0, 1, and 2
    for (uint32_t i = 0; i < num_small_allocations; i++) {
        variables2[i] = (uint64_t *)allocator_malloc(alloc, allocation_size);
        uintptr_t end = align_up((uintptr_t)variables2[i], SECTION_SIZE);
        if ((end - (uintptr_t)variables2[i]) < allocation_size) {
            return false;
        }
        if (variables2[i] == NULL) {
            return false;
        }
    }
    // next allocation should be NULL;
    void *nll = allocator_malloc(alloc, allocation_size);
    if (nll != NULL) {
        return false;
    }
    for (uint32_t i = 0; i < num_small_allocations; i++) {
        allocator_free(alloc, variables2[i]);
    }
    free(variables2);
    // release all the system resources
    allocator_release_local_areas(alloc);
    return true;
}

bool test_pools_small() { return test_pools(small_pool_size, max_small_size); }
bool test_medium_pools() { return test_pools(mid_pool_size, max_mid_size); }

bool test_large_pools() { return test_pools(large_pool_size, max_large_size); }

bool test_pages(size_t page_size, size_t allocation_size)
{
    Allocator *alloc = allocator_get_thread_instance();
    uint64_t base_parts = num_areas_part0 + num_areas_part1;
    uint64_t extended_parts = num_areas_part0 + num_areas_part1 + num_areas_part2;
    if (page_size != small_page_size) {
        base_parts = num_areas_part2;
        extended_parts = num_areas_part2 + num_areas_part3;
    }
    if (page_size == large_page_size) {
        extended_parts = 0;
    }
    uint64_t max_count_per_page = (page_size - 64) / allocation_size;
    uint64_t num_small_areas = base_parts;
    uint64_t num_small_allocations = max_count_per_page * num_small_areas;

    uint64_t expected_reserved_mem = os_page_size * num_small_allocations;  // if all pools are touched
    uint64_t actual_reserver_mem = allocation_size * num_small_allocations; // if all the owned pages would be touched

    uint64_t **variables = (uint64_t **)malloc(num_small_allocations * sizeof(uint64_t));

    double readable_reserved = (double)expected_reserved_mem / (SZ_GB);
    // exhaust part 0 and 1
    for (uint32_t i = 0; i < num_small_allocations; i++) {
        variables[i] = (uint64_t *)allocator_malloc_page(alloc, allocation_size);
        uintptr_t end = align_up((uintptr_t)variables[i], page_size);
        if ((end - (uintptr_t)variables[i]) < allocation_size) {
            return false;
        }
        if (variables[i] == NULL) {
            return false;
        }
    }

    if (extended_parts != 0) {
        for (uint32_t i = 0; i < num_small_allocations; i++) {
            allocator_free(alloc, variables[i]);
        }
        // release all the system resources
        allocator_release_local_areas(alloc);
        free(variables);
        max_count_per_page = (page_size - 64) / allocation_size;
        num_small_areas = extended_parts;
        num_small_allocations = max_count_per_page * num_small_areas;
        expected_reserved_mem = os_page_size * num_small_allocations;  // if all pools are touched
        actual_reserver_mem = allocation_size * num_small_allocations; // if all the owned pages would be touched

        readable_reserved = (double)expected_reserved_mem / (SZ_GB);
        uint64_t **variables2 = (uint64_t **)malloc(num_small_allocations * sizeof(uint64_t));
        // exhaust part 0, 1, and 2
        for (uint32_t i = 0; i < num_small_allocations; i++) {
            variables2[i] = (uint64_t *)allocator_malloc_page(alloc, allocation_size);
            uintptr_t end = align_up((uintptr_t)variables2[i], page_size);
            if ((end - (uintptr_t)variables2[i]) < allocation_size) {
                return false;
            }
            if (variables2[i] == NULL) {
                return false;
            }
        }
        // next allocation should be NULL;
        void *nll = allocator_malloc_page(alloc, allocation_size);
        if (nll != NULL) {
            return false;
        }
        for (uint32_t i = 0; i < num_small_allocations; i++) {
            allocator_free(alloc, variables2[i]);
        }
        free(variables2);
    } else {
        // next allocation should be NULL;
        void *nll = allocator_malloc_page(alloc, allocation_size);
        if (nll != NULL) {
            return false;
        }
        for (uint32_t i = 0; i < num_small_allocations; i++) {
            allocator_free(alloc, variables[i]);
        }
        free(variables);
    }
    // release all the system resources
    allocator_release_local_areas(alloc);
    return true;
}

bool test_medium_pages() { return test_pages(mid_page_size, max_mid_size_page); }

bool test_large_pages() { return test_pages(large_page_size, max_large_size_page); }

bool test_small_pages() { return test_pages(small_page_size, max_small_size_page); }

bool test_slabs()
{
    Allocator *alloc = allocator_get_thread_instance();
    uint64_t allocation_size = 129 * sz_mb;
    uint64_t base_parts = num_areas_part3;
    uint64_t max_count_per_page = 1;
    uint64_t num_small_areas = base_parts;
    uint64_t num_small_allocations = max_count_per_page * num_small_areas;

    uint64_t **variables = (uint64_t **)malloc(num_small_allocations * sizeof(uint64_t));

    // exhaust part 0 and 1
    for (uint32_t i = 0; i < num_small_allocations; i++) {
        variables[i] = (uint64_t *)allocator_malloc_page(alloc, allocation_size);
        uintptr_t end = align_up((uintptr_t)variables[i], 256 * sz_mb);
        if ((end - (uintptr_t)variables[i]) < allocation_size) {
            return false;
        }
        if (variables[i] == NULL) {
            return false;
        }
    }
    void *nll = allocator_malloc_page(alloc, allocation_size);
    if (nll != NULL) {
        return false;
    }
    for (uint32_t i = 0; i < num_small_allocations; i++) {
        allocator_free(alloc, variables[i]);
    }
    free(variables);
    return true;
}

bool test_areas()
{
    Allocator *alloc = allocator_get_thread_instance();
    uint64_t allocation_size = 129 * sz_mb;
    uint64_t base_parts = num_areas_part3;
    uint64_t num_alloc = base_parts;

    uint64_t **variables = (uint64_t **)malloc(num_alloc * sizeof(uint64_t));
    for (uint32_t i = 0; i < num_alloc; i++) {
        variables[i] = (uint64_t *)allocator_malloc(alloc, allocation_size);
    }
    void *nll = allocator_malloc_page(alloc, allocation_size);
    if (nll != NULL) {
        return false;
    }

    uintptr_t end_addr = (uintptr_t)variables[num_alloc - 2];
    uintptr_t next_addr = (uintptr_t)variables[num_alloc - 3];
    allocator_free(alloc, variables[num_alloc - 2]);
    allocator_free(alloc, variables[num_alloc - 3]);
    uintptr_t new_addr = (uintptr_t)allocator_malloc(alloc, allocation_size);
    variables[num_alloc - 3] = (uint64_t *)new_addr;
    if (new_addr != next_addr && new_addr != end_addr) {
        return false;
    }
    new_addr = (uintptr_t)allocator_malloc(alloc, allocation_size);
    if (new_addr != end_addr) {
        return false;
    }
    variables[num_alloc - 2] = (uint64_t *)new_addr;

    next_addr = (uintptr_t)variables[num_alloc - 5];
    // remove four and try to allocate 780 megs;
    allocator_free(alloc, variables[num_alloc - 2]);
    allocator_free(alloc, variables[num_alloc - 3]);
    allocator_free(alloc, variables[num_alloc - 4]);
    allocator_free(alloc, variables[num_alloc - 5]);
    new_addr = (uintptr_t)allocator_malloc(alloc, 780 * sz_mb);
    if (new_addr != next_addr) {
        return false;
    }
    allocator_free(alloc, (void *)new_addr);
    variables[num_alloc - 2] = (uint64_t *)allocator_malloc(alloc, allocation_size);
    variables[num_alloc - 3] = (uint64_t *)allocator_malloc(alloc, allocation_size);
    variables[num_alloc - 4] = (uint64_t *)allocator_malloc(alloc, allocation_size);
    variables[num_alloc - 5] = (uint64_t *)allocator_malloc(alloc, allocation_size);
    allocator_free(alloc, variables[num_alloc - 2]);
    if (allocator_malloc(alloc, 256 * sz_mb) != NULL) {
        return false;
    }
    variables[num_alloc - 2] = (uint64_t *)allocator_malloc(alloc, allocation_size);
    for (uint32_t i = 0; i < num_alloc; i++) {
        allocator_free(alloc, variables[i]);
    }
    free(variables);
    return true;
}

bool test_huge_alloc()
{
    Allocator *alloc = allocator_get_thread_instance();
    void *gb = (uint64_t *)allocator_malloc(alloc, 15 * sz_gb);
    if (gb == NULL) {
        return false;
    }
    allocator_free(alloc, gb);
    gb = (uint64_t *)allocator_malloc(alloc, 16 * sz_gb);
    if (gb != NULL) {
        return false;
    }
    return true;
}

bool fillAPool()
{
    Allocator *alloc = allocator_get_thread_instance();
    const int num_allocs = 16378;
    uint64_t **allocs = (uint64_t **)malloc(num_allocs * sizeof(uint64_t));
    for (int i = 0; i < num_allocs; i++) {
        allocs[i] = (uint64_t *)allocator_malloc(alloc, 8);
        *allocs[i] = (uint64_t)allocs[i];
    }

    for (int i = 0; i < num_allocs; i++) {
        if (*allocs[i] != (uint64_t)allocs[i]) {
            free(allocs);
            return false;
        }
        allocator_free(alloc, allocs[i]);
    }
    // allocate 8 byte parts to fill 128bytes.
    // what is the addres of the last allocation in the pool.
    // is there room for one more?
    free(allocs);
    return true;
}

bool fillASection()
{
    Allocator *alloc = allocator_get_thread_instance();
    const int num_pools = 32;
    const int num_allocs = 16378;
    uint64_t **allocs = (uint64_t **)malloc(num_allocs * num_pools * sizeof(uint64_t **));
    for (int s = 0; s < num_pools; s++) {
        for (int i = 0; i < num_allocs; i++) {
            allocs[i + (num_allocs * s)] = (uint64_t *)allocator_malloc(alloc, 8);
            *allocs[i + (num_allocs * s)] = (uint64_t)allocs[i + (num_allocs * s)];
        }
    }

    for (int s = 0; s < num_pools; s++) {
        for (int i = 0; i < num_allocs; i++) {
            if (*allocs[i + (num_allocs * s)] != (uint64_t)allocs[i + (num_allocs * s)]) {
                return false;
            }
            allocator_free(alloc, allocs[i + (num_allocs * s)]);
        }
    }
    free(allocs);
    // fill all 8 pools of one section.
    return true;
}

bool fillAnArea()
{
    Allocator *alloc = allocator_get_thread_instance();
    const int num_sections = 8;
    const int num_pools = 32;
    const int num_allocs = 16378;
    const int total_allocs = num_sections * num_pools * num_allocs;
    uint64_t **allocs = (uint64_t **)malloc(total_allocs * sizeof(uint64_t **));

    for (int s = 0; s < num_pools * num_sections; s++) {
        for (int i = 0; i < num_allocs; i++) {
            int index = i + (num_allocs * s);
            allocs[index] = (uint64_t *)allocator_malloc(alloc, 8);
            *allocs[index] = (uint64_t)allocs[index];
        }
    }

    for (int s = 0; s < num_pools * num_sections; s++) {
        for (int i = 0; i < num_allocs; i++) {
            int index = i + (num_allocs * s);
            if (*allocs[index] != (uint64_t)allocs[index]) {
                return false;
            }
            allocator_free(alloc, allocs[index]);
        }
    }

    free(allocs);

    // fill all 8 pools of one section.
    return true;
}

bool fillAPage()
{
    Allocator *alloc = allocator_get_thread_instance();
    const int num_allocs = 1398094;
    uint64_t **allocs = (uint64_t **)malloc(num_allocs * sizeof(uint64_t **));

    for (int i = 0; i < num_allocs; i++) {
        uint64_t *addr = (uint64_t *)allocator_malloc_page(alloc, 8);
        allocs[i] = addr;
        *allocs[i] = (uint64_t)allocs[i];
    }

    for (int i = 0; i < num_allocs; i++) {

        if (*allocs[i] != (uint64_t)allocs[i]) {
            return false;
        }
        allocator_free(alloc, allocs[i]);
    }

    free(allocs);

    // fill all 8 pools of one section.
    return true;
}

void run_tests()
{

    Allocator *alloc = allocator_get_thread_instance();
    START_TEST(Allocator, {});
    TEST(Allocator, pools_small, { EXPECT(test_pools_small()); });
    TEST(Allocator, medium_pools, { EXPECT(test_medium_pools()); });
    TEST(Allocator, large_pools, { EXPECT(test_large_pools()); });
    TEST(Allocator, small_pages, { EXPECT(test_small_pages()); });
    TEST(Allocator, medium_pages, { EXPECT(test_medium_pages()); });
    TEST(Allocator, large_pages, { EXPECT(test_large_pages()); });
    TEST(Allocator, slabs, { EXPECT(test_slabs()); });
    TEST(Allocator, huge_alloc, { EXPECT(test_huge_alloc()); });

    TEST(Allocator, areas, { EXPECT(test_areas()); });
    TEST(Allocator, fillAPool, { EXPECT(fillAPool()); });
    TEST(Allocator, fillASection, { EXPECT(fillASection()); });
    TEST(Allocator, fillAnArea, { EXPECT(fillAnArea()); });
    TEST(Allocator, fillAPage, { EXPECT(fillAPage()); });
    END_TEST(Allocator, {});
    allocator_release_local_areas(alloc);
}

bool testAreaFail()
{
    Allocator *alloc = allocator_get_thread_instance();
    // Why am I stalling this!
    // this is sort of the last fallback step and allows you allocate all the
    // ranges with a single thread. if allocations fail, it can move into other
    // partition sets. then I can move into the thread free part. Which should
    // be a lot simpler.
    //   then it is just testing and cleaning things up..
    void *m = alloc_memory_aligned((void *)partitions_offsets[0], 48 * sz_mb, 32 * sz_mb, true, partitions_offsets[1]);
    //

    // two areas should be marked as bad.
    // see if those areas are marked as bad.
    // how should we mark those areas as bad.
    // if the size of an area is -1, then it is bad.
    void *bb = allocator_malloc(alloc, 24);
    //
    // next allocate most of the first partition. forcing the next allocation to
    // be from another partition set. if that is reserved already. then promote
    // to the next partition.
    //
    //
    // allocate some memory at where the initial thread assumes is free
    //  - verify that the next area it returns is correct.
    // allocate some memory that overlaps the rangeo of two partition sets.
    //  -- verify that the nex tarea is in the next partition set
    //  ensure that the areas that are invalid are all marked as invalid.
    //  ensure that the partition-set is allocated only if it is available.
    //
    allocator_free(alloc, bb);
    free_memory(m, 48 * sz_mb);
    allocator_release_local_areas(alloc);
    bb = allocator_malloc(alloc, 24);
    allocator_free(alloc, bb);
    allocator_release_local_areas(alloc);
    return false;
}

static inline uint8_t sizeToPool2(size_t as)
{
    static const int bmask = ~0x7f;
    if ((bmask & as) == 0) {
        // the first 2 rows
        return (as >> 3);
    } else {
        const uint32_t top_mask = 0xffffffff;
        const int tz = __builtin_clz(as);
        const uint64_t bottom_mask = (top_mask >> (tz + 4));
        const uint64_t incr = (bottom_mask & as) > 0;
        const size_t row = (26 - tz) * 8;
        return (row + ((as >> (28 - tz)) & 0x7)) + incr;
    }
}

int test_size_to_pool()
{

    static const int32_t pool_sizes[] = {
        0,       8,       16,      24,      32,      40,      48,      56,      64,      72,      80,      88,
        96,      104,     112,     120,     128,     144,     160,     176,     192,     208,     224,     240,
        256,     288,     320,     352,     384,     416,     448,     480,     512,     576,     640,     704,
        768,     832,     896,     960,     1024,    1152,    1280,    1408,    1536,    1664,    1792,    1920,
        2048,    2304,    2560,    2816,    3072,    3328,    3584,    3840,    4096,    4608,    5120,    5632,
        6144,    6656,    7168,    7680,    8192,    9216,    10240,   11264,   12288,   13312,   14336,   15360,
        16384,   18432,   20480,   22528,   24576,   26624,   28672,   30720,   32768,   36864,   40960,   45056,
        49152,   53248,   57344,   61440,   65536,   73728,   81920,   90112,   98304,   106496,  114688,  122880,
        131072,  147456,  163840,  180224,  196608,  212992,  229376,  245760,  262144,  294912,  327680,  360448,
        393216,  425984,  458752,  491520,  524288,  589824,  655360,  720896,  786432,  851968,  917504,  983040,
        1048576, 1179648, 1310720, 1441792, 1572864, 1703936, 1835008, 1966080, 2097152, 2359296, 2621440, 2883584,
        3145728, 3407872, 3670016, 3932160, 4194304, 4718592};

    for (int ii = 0; ii <= 4718592; ii++) {
        int i = ALIGN(ii);

        uint32_t np = sizeToPool2(i);
        if (np == 0) {
            if (i > 8) {
                printf("does not fita %d %d", pool_sizes[np], i);
                return -1;
            }

        } else {
            if (pool_sizes[np] < i) {
                printf("does not fit %d %d", pool_sizes[np], i);
                return -1;
            }
            if (pool_sizes[np - 1] >= i) {
                printf("not best fit %d %d", pool_sizes[np - 1], i);
                return -1;
            }
        }
    }
    return 1;
}

void test_size_iter(uint32_t alloc_size)
{

    START_TEST(allocator, {});
    char **variables = (char **)malloc(NUMBER_OF_ITEMS * sizeof(char *));

    MEASURE_TIME(allocator, alloc, {
        for (uint64_t j = 0; j < NUMBER_OF_ITERATIONS; j++) {
            for (uint64_t i = 0; i < NUMBER_OF_ITEMS; i++) {
                Allocator *al = allocator_get_thread_instance();
                variables[i] = (char *)allocator_malloc(al, alloc_size);
            }

            for (uint64_t i = 0; i < NUMBER_OF_ITEMS; i++) {
                Allocator *al = allocator_get_thread_instance();
                allocator_free(al, variables[i]);
            }
        }
    });
    // allocator_release_local_areas(alloc);
    /*

    MEASURE_TIME(Allocator, malloc, {
        for (uint64_t j = 0; j < NUMBER_OF_ITERATIONS; j++) {
            for (uint64_t i = 0; i < NUMBER_OF_ITEMS; i++)
                variables[i] = (char *)mi_malloc(alloc_size);

            for (uint64_t i = 0; i < NUMBER_OF_ITEMS; i++)
                mi_free(variables[i]);
        }
    });
    mi_collect(true);
    */
    END_TEST(allocator, {});
    free(variables);
}

int main()
{
    // int32_t ii = test_size_to_pool();

    // run_tests();
    for (int i = 0; i < 12; i++) {
        test_size_iter(1 << i);
    }
    return 0;
}
