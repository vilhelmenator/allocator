
#define CTEST_ENABLED
#include "../ctest/ctest.h"
#include "allocator.h"
//#include "cthread.h"
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

const uint64_t puny_page_size = 4 * sz_mb;    // allocations <= 16k
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

const uint64_t max_small_size = 16 * sz_kb;
const uint64_t max_mid_size = 128 * sz_kb;
const uint64_t max_large_size = 2 * sz_mb;

const uint64_t max_puny_size_page = 16 * sz_kb;
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
    bool result = true;
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
    int8_t pid = 0;
    for (uint32_t i = 0; i < num_small_allocations; i++) {
        void *all = allocator_malloc(alloc, allocation_size);
        pid = partition_id_from_addr((uintptr_t)all);
        if (all == NULL) {
            result = false;
            goto end;
        }
        variables[i] = (uint64_t *)all;
        uintptr_t end = align_up((uintptr_t)variables[i], SECTION_SIZE);
        if ((end - (uintptr_t)variables[i]) < allocation_size) {
            result = false;
            goto end;
        }
        if (variables[i] == NULL) {
            result = false;
            goto end;
        }
    }
    for (uint32_t i = 0; i < num_small_allocations; i++) {
        allocator_free(alloc, variables[i]);
    }
    free(variables);
    // release all the system resources
    if (!allocator_release_local_areas(alloc)) {
        return false;
    }

    num_small_sections = num_sections_part0 + num_sections_part1 + num_sections_part2;
    num_pools = num_small_sections * pools_per_section;
    num_small_allocations = num_pools * max_count_per_pool;
    expected_reserved_mem = os_page_size * num_small_allocations;  // if all pools are touched
    actual_reserver_mem = allocation_size * num_small_allocations; // if all the owned pages would be touched

    readable_reserved = (double)expected_reserved_mem / (SZ_GB);
    variables = (uint64_t **)malloc(num_small_allocations * sizeof(uint64_t));
    // exhaust part 0, 1, and 2
    for (uint32_t i = 0; i < num_small_allocations; i++) {
        variables[i] = (uint64_t *)allocator_malloc(alloc, allocation_size);
        pid = partition_id_from_addr((uintptr_t)variables[i]);
        uintptr_t end = align_up((uintptr_t)variables[i], SECTION_SIZE);
        if ((end - (uintptr_t)variables[i]) < allocation_size) {
            result = false;
            goto end;
        }
        if (variables[i] == NULL) {
            result = false;
            goto end;
        }
    }
    // next allocation should be from a different partition id
    void *nll = allocator_malloc(alloc, allocation_size);
    int8_t npid = partition_id_from_addr((uintptr_t)nll);
    if (npid == pid) {
        result = false;
    }
    allocator_free(alloc, nll);
end:
    for (uint32_t i = 0; i < num_small_allocations; i++) {
        allocator_free(alloc, variables[i]);
    }
    free(variables);
    // release all the system resources

    return allocator_release_local_areas(alloc);
    ;
}

bool test_pools_small(void) { return test_pools(small_pool_size, max_small_size); }
bool test_medium_pools(void) { return test_pools(mid_pool_size, max_mid_size); }

bool test_large_pools(void) { return test_pools(large_pool_size, max_large_size); }

bool test_pages(size_t page_size, size_t promotion_size, size_t allocation_size)
{
    bool result = true;
    Allocator *alloc = allocator_get_thread_instance();
    uint64_t base_parts = num_areas_part0 + num_areas_part1;
    uint64_t extended_parts = num_areas_part2;
    if (page_size > small_page_size) {
        base_parts = num_areas_part2;
        extended_parts = num_areas_part3;
    }
    if (page_size == large_page_size) {
        extended_parts = 0;
    }
    uint64_t max_count_per_page = (page_size - 64) / allocation_size;
    uint64_t max_count_extended_page = (promotion_size - 64) / allocation_size;
    uint64_t num_allocations = max_count_per_page * base_parts;
    uint64_t num_extended_allocations = max_count_per_page * base_parts + max_count_extended_page * extended_parts;
    uint64_t expected_reserved_mem = os_page_size * num_allocations;  // if all pools are touched
    uint64_t actual_reserver_mem = allocation_size * num_allocations; // if all the owned pages would be touched

    uint64_t **variables = (uint64_t **)malloc(num_allocations * sizeof(uint64_t));

    double readable_reserved = (double)expected_reserved_mem / (SZ_GB);
    // exhaust part 0 and 1
    for (uint32_t i = 0; i < num_allocations; i++) {
        uint64_t *new_addr = (uint64_t *)allocator_malloc_heap(alloc, allocation_size);
        variables[i] = new_addr;
        uintptr_t end = align_up((uintptr_t)variables[i], area_size_from_addr((uintptr_t)new_addr));
        if ((end - (uintptr_t)variables[i]) < allocation_size) {
            result = false;
            goto end;
        }
        if (variables[i] == NULL) {
            result = false;
            goto end;
        }
    }
    if (extended_parts == 0) {
        // next allocation should be NULL;
        void *nll = allocator_malloc_heap(alloc, allocation_size);
        if (nll != NULL) {
            result = false;
        }
    }
    for (uint32_t i = 0; i < num_allocations; i++) {
        allocator_free(alloc, variables[i]);
    }
    free(variables);

    // release all the system resources
    if (!allocator_release_local_areas(alloc)) {
        return false;
    }

    if (extended_parts == 0) {
        return result;
    }

    expected_reserved_mem = os_page_size * num_extended_allocations;  // if all pools are touched
    actual_reserver_mem = allocation_size * num_extended_allocations; // if all the owned pages would be touched

    readable_reserved = (double)expected_reserved_mem / (SZ_GB);
    num_allocations = num_extended_allocations;
    variables = (uint64_t **)malloc(num_allocations * sizeof(uint64_t));
    // exhaust part 0, 1, and 2
    for (uint32_t i = 0; i < num_allocations; i++) {
        uint64_t *new_addr = (uint64_t *)allocator_malloc_heap(alloc, allocation_size);
        variables[i] = new_addr;
        uintptr_t end = align_up((uintptr_t)variables[i], area_size_from_addr((uintptr_t)new_addr));
        if ((end - (uintptr_t)variables[i]) < allocation_size) {
            result = false;
            goto end;
        }
        if (variables[i] == NULL) {
            result = false;
            goto end;
        }
    }
    // next allocation should be NULL;
    void *nll = allocator_malloc_heap(alloc, allocation_size);
    if (nll != NULL) {
        result = false;
    }

end:
    for (uint32_t i = 0; i < num_allocations; i++) {
        allocator_free(alloc, variables[i]);
    }
    free(variables);
    // release all the system resources

    return allocator_release_local_areas(alloc);
}

bool test_medium_pages(void) { return test_pages(mid_page_size, large_page_size, max_mid_size_page); }

bool test_large_pages(void) { return test_pages(large_page_size, large_page_size, max_large_size_page); }

bool test_small_pages(void) { return test_pages(small_page_size, mid_page_size, max_small_size_page); }

bool test_puny_pages(void) { return test_pages(puny_page_size, puny_page_size, max_puny_size_page); }

bool test_slabs(void)
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
        variables[i] = (uint64_t *)allocator_malloc_heap(alloc, allocation_size);
        uintptr_t end = align_up((uintptr_t)variables[i], 256 * sz_mb);
        if ((end - (uintptr_t)variables[i]) < allocation_size) {
            return false;
        }
        if (variables[i] == NULL) {
            return false;
        }
    }
    void *nll = allocator_malloc_heap(alloc, allocation_size);
    if (nll != NULL) {
        return false;
    }
    for (uint32_t i = 0; i < num_small_allocations; i++) {
        allocator_free(alloc, variables[i]);
    }
    free(variables);
    return true;
}

bool test_areas(void)
{
    Allocator *alloc = allocator_get_thread_instance();
    uint64_t allocation_size = 129 * sz_mb;
    uint64_t base_parts = num_areas_part3;
    uint64_t num_alloc = base_parts;

    uint64_t **variables = (uint64_t **)malloc(num_alloc * sizeof(uint64_t));
    for (uint32_t i = 0; i < num_alloc; i++) {
        variables[i] = (uint64_t *)allocator_malloc(alloc, allocation_size);
    }
    void *nll = allocator_malloc_heap(alloc, allocation_size);
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

bool test_huge_alloc(void)
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

bool fillAPool(void)
{
    Allocator *alloc = allocator_get_thread_instance();
    const int num_allocs = 16378;
    uint64_t **allocs = (uint64_t **)malloc(num_allocs * sizeof(uint64_t **));
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
    free(allocs);
    // allocate 8 byte parts to fill 128bytes.
    // what is the addres of the last allocation in the pool.
    // is there room for one more?
    return true;
}

bool fillASection(void)
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

bool fillAnArea(void)
{
    Allocator *alloc = allocator_get_thread_instance();
    const int num_sections = 8;
    const int num_pools = 32;
    const int num_allocs = 16378;
    const int total_allocs = num_sections * num_pools * num_allocs;
    uint64_t **allocs = (uint64_t **)malloc(total_allocs * sizeof(uint64_t **));

    int index = 0;
    for (int s = 0; s < num_pools * num_sections; s++) {
        for (int i = 0; i < num_allocs; i++) {
            index = i + (num_allocs * s);
            allocs[index] = (uint64_t *)allocator_malloc(alloc, 8);
            *allocs[index] = (uint64_t)allocs[index];
        }
    }
    /*uint64_t * start = allocs[0];
    uint64_t * end = allocs[index];
    uint64_t *diff = end - start;
     */
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

bool fillAPage(void)
{
    Allocator *alloc = allocator_get_thread_instance();
    const int num_allocs = 1398094;
    uint64_t **allocs = (uint64_t **)malloc(num_allocs * sizeof(uint64_t **));

    for (int i = 0; i < num_allocs; i++) {
        uint64_t *addr = (uint64_t *)allocator_malloc_heap(alloc, 8);
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

void run_tests(void)
{

    Allocator *alloc = allocator_get_thread_instance();
    START_TEST(Allocator, {});
    TEST(Allocator, pools_small, { EXPECT(test_pools_small()); });
    TEST(Allocator, medium_pools, { EXPECT(test_medium_pools()); });
    TEST(Allocator, large_pools, { EXPECT(test_large_pools()); });
    TEST(Allocator, puny_pages, { EXPECT(test_puny_pages()); });
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

bool testAreaFail(void)
{
    Allocator *alloc = allocator_get_thread_instance();
    // Why am I stalling this!
    // this is sort of the last fallback step and allows you allocate all the
    // ranges with a single thread. if allocations fail, it can move into other
    // partition sets. then I can move into the thread free part. Which should
    // be a lot simpler.
    //   then it is just testing and cleaning things up..
    void *m = alloc_memory_aligned((void *)PARTITION_0, PARTITION_1, 48 * sz_mb, 32 * sz_mb);
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

void test_size_iter(uint32_t alloc_size, size_t num_items, size_t num_loops)
{

    START_TEST(allocator, {});
    char **variables = (char **)malloc(num_items * sizeof(char *));

    MEASURE_TIME(allocator, alloc, {
        for (uint64_t j = 0; j < num_loops; j++) {
            for (uint64_t i = 0; i < num_items; i++) {
                variables[i] = (char *)cmalloc(alloc_size);
            }
            for (uint64_t i = 0; i < num_items; i++) {

                cfree(variables[i]);
            }
        }
    });
    // allocator_release_local_areas(alloc);
    /*
    MEASURE_TIME(Allocator, malloc, {
        for (uint64_t j = 0; j < num_loops; j++) {
            for (uint64_t i = 0; i < num_items; i++)
                variables[i] = (char *)mi_malloc(alloc_size);

            for (uint64_t i = 0; i < num_items; i++)
                mi_free(variables[i]);
        }
    });
    //mi_collect(true);
    */
    END_TEST(allocator, {});
    free(variables);
}
/*
int test(void*)
{
    char* test = (char *)cmalloc(16);
    cfree(test);
    return 1;
}*/
int main()
{
    // thrd_t trd;
    // thrd_create(&trd, &test, NULL);

    // run_tests();
    //  printf("%d %d ", offsetof(__typeof__(Section), prev), offsetof(__typeof__(Heap), prev));

    for (int i = 0; i < 14; i++) {
        test_size_iter(1 << i, NUMBER_OF_ITEMS, NUMBER_OF_ITERATIONS);
    }
    size_t item_count = 100;
    for (int i = 0; i < 6; i++) {
        // test_size_iter(1 << 3, item_count, NUMBER_OF_ITERATIONS);
        item_count *= 10;
    }
    return 0;
}
