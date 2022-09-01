
#define CTEST_ENABLED
#include "../ctest/ctest.h"
#include "area.h"
#include "arena.h"
#include "callocator.inl"
//#include "mimalloc.h"
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
 traverse the network of pointers and pass them to their correct pools or heaps.
        : destruct( ptr, schema ) -> no nested destruct calls.
        : construct( ptr, schema ) ->
 */

//
//
// 49152*8 + 65536*8 = 344064 + 458752 = 802816 max 12.8gigs or 3.8gigs of
// heaps. test exhausting all the pools on main thread. then on the last thread
// and various random threads. do the same for each size class.
//
// another test to test heap allocations. small and large.
// allocate all the heaps.
// test on various thread ids
//
// partition 1. 4 meg heaps.
// 192 areas 32 megs. each has 8 sections. each section has 32 pools.
// sections = 192 * 8;  // 1536
// pools = sections*32; // 49152, 12288, 1536
//
// 64 areas * 32        // 2048
// pools = sections*32  // 65536, 16384, 2048
//
// pools can bleed into second partition
// heaps can bleed into third partition.
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

const uint64_t puny_heap_size = 4 * sz_mb;    // allocations <= 16k
const uint64_t small_heap_size = 32 * sz_mb;  // allocations <= 128k
const uint64_t mid_heap_size = 64 * sz_mb;    // allocations <= 32Mb
const uint64_t large_heap_size = 128 * sz_mb; // allocations <= 32Mb
const uint64_t huge_heap_size = 256 * sz_mb;  // allocations <= 128Mb

const uint64_t num_areas_part0 = (sz_gb * 2) / (32 * sz_mb);
const uint64_t num_areas_part1 = (sz_gb * 4) / (64 * sz_mb);
const uint64_t num_areas_part2 = (sz_gb * 8) / (128 * sz_mb);
const uint64_t num_areas_part3 = (sz_gb * 16) / (256 * sz_mb);

const uint64_t num_sections_part0 = (sz_gb * 2) / (4 * sz_mb);
const uint64_t num_sections_part1 = (sz_gb * 4) / (4 * sz_mb);
const uint64_t num_sections_part2 = (sz_gb * 8) / (4 * sz_mb);

const uint64_t max_small_size = 16 * sz_kb;
const uint64_t max_mid_size = 128 * sz_kb;
const uint64_t max_large_size = 2 * sz_mb;
const uint64_t max_huge_size = 32 * sz_mb;

const uint64_t max_puny_size_heap = 16 * sz_kb;
const uint64_t max_small_size_heap = 128 * sz_kb;
const uint64_t max_mid_size_heap = 2 * sz_mb;
const uint64_t max_large_size_heap = 32 * sz_mb;
const uint64_t max_huge_size_heap = 128 * sz_mb;

static inline uintptr_t align_up(uintptr_t sz, size_t alignment)
{
    uintptr_t mask = alignment - 1;
    uintptr_t sm = (sz + mask);
    if ((alignment & mask) == 0) {
        return sm & ~mask;
    } else {
        return (sm / alignment) * alignment;
    }
}
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
 [x] test small heaps.
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
 [ ] test small heap coalesce rules.
 [ ] test pools vs heaps for small sizes


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
into other threads. 4. [ ] improve heap allocations. ordererd lists. double free
tests. [ ] memory API. alloc and string functions.

5.
[ ] add memory block objects and implicit list alocation support.
[ ] stats. leaks.
[ ] integrate with ALLOT
[ ] test with builder. DONE!
*/

bool test_pools(size_t allocation_size)
{
    size_t pool_size = 0;
    if (allocation_size <= max_small_size) {
        pool_size = small_pool_size;
    } else if (allocation_size <= max_mid_size) {
        pool_size = mid_pool_size;
    } else {
        pool_size = large_pool_size;
    }

    bool result = true;

    uint64_t pools_per_section = section_size / pool_size;
    uint64_t max_count_per_pool = (pool_size - 64) / allocation_size;
    uint64_t num_small_sections = num_sections_part0 + num_sections_part1;
    uint64_t num_pools = num_small_sections * pools_per_section;
    uint64_t num_small_allocations = num_pools * max_count_per_pool;

    uint64_t expected_reserved_mem = DEFAULT_OS_PAGE_SIZE * num_small_allocations; // if all pools are touched
    uint64_t actual_reserver_mem = allocation_size * num_small_allocations; // if all the owned pages would be touched

    uint64_t **variables = (uint64_t **)malloc(num_small_allocations * sizeof(uint64_t));

    double readable_reserved = (double)expected_reserved_mem / (SZ_GB);
    // exhaust part 0 and 1
    int8_t pid = 0;
    for (uint32_t i = 0; i < num_small_allocations; i++) {
        void *all = cmalloc(allocation_size);
        pid = partition_from_addr((uintptr_t)all);
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
        cfree(variables[i]);
    }
    free(variables);
    // release all the system resources
    if (!callocator_release()) {
        return false;
    }

    num_small_sections = num_sections_part0 + num_sections_part1 + num_sections_part2;
    num_pools = num_small_sections * pools_per_section;
    num_small_allocations = num_pools * max_count_per_pool;
    expected_reserved_mem = DEFAULT_OS_PAGE_SIZE * num_small_allocations; // if all pools are touched
    actual_reserver_mem = allocation_size * num_small_allocations;        // if all the owned pages would be touched

    readable_reserved = (double)expected_reserved_mem / (SZ_GB);
    variables = (uint64_t **)malloc(num_small_allocations * sizeof(uint64_t));
    // exhaust part 0, 1, and 2
    for (uint32_t i = 0; i < num_small_allocations; i++) {
        variables[i] = (uint64_t *)cmalloc(allocation_size);
        pid = partition_from_addr((uintptr_t)variables[i]);
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
    void *nll = cmalloc(allocation_size);
    int8_t npid = partition_from_addr((uintptr_t)nll);
    if (npid == pid) {
        result = false;
    }
    cfree(nll);
end:
    for (uint32_t i = 0; i < num_small_allocations; i++) {
        cfree(variables[i]);
    }
    free(variables);
    // release all the system resources

    return callocator_release();
    ;
}

bool test_pools_small(void) { return test_pools(max_small_size); }
bool test_medium_pools(void) { return test_pools(max_mid_size); }

bool test_large_pools(void) { return test_pools(max_large_size); }

bool test_heaps(size_t allocation_size)
{
    bool result = true;
    uint64_t num_allocations = 0;
    uint64_t num_extended_allocations = 0;
    uint64_t max_count_per_heap_1 = ((1 << HT_32M) - sizeof(Area) - sizeof(Heap)) / allocation_size;
    uint64_t max_count_per_heap_2 = ((1 << HT_64M) - sizeof(Area) - sizeof(Heap)) / allocation_size;
    uint64_t max_count_per_heap_3 = ((1 << HT_128M) - sizeof(Area) - sizeof(Heap)) / allocation_size;
    uint64_t max_count_per_heap_4 = ((1 << HT_256M) - sizeof(Area) - sizeof(Heap)) / allocation_size;
    if (allocation_size <= max_small_size) { // 8 - 16k
        uint64_t base_parts = num_areas_part0 * 8 + num_areas_part1 * 16;
        uint64_t max_count_per_heap = ((1 << HT_4M) - sizeof(Section) - sizeof(Heap)) / allocation_size;
        num_allocations = max_count_per_heap * base_parts;
        uint64_t max_count_extended_heap = max_count_per_heap * num_areas_part2 * 32;
        num_extended_allocations = num_allocations + max_count_extended_heap;
    } else if (allocation_size <= max_mid_size) { // 16k - 128k
        num_allocations = max_count_per_heap_1 * num_areas_part0 + max_count_per_heap_2 * num_areas_part1;
        uint64_t extended_parts = max_count_per_heap_3 * num_areas_part2;
        num_extended_allocations = num_allocations + extended_parts;
    } else if (allocation_size <= max_large_size) { // 4Mb - 32Mb
        num_allocations = max_count_per_heap_2 * num_areas_part1;
        uint64_t extended_parts = max_count_per_heap_3 * num_areas_part2;
        num_extended_allocations = num_allocations + extended_parts;
    } else if (allocation_size <= max_huge_size) { // 4Mb - 32Mb
        num_allocations = max_count_per_heap_3 * num_areas_part2;
        uint64_t extended_parts = max_count_per_heap_4 * num_areas_part3;
        num_extended_allocations = num_allocations + extended_parts;
    } else { // for large than 32Mb objects.
        num_allocations = max_count_per_heap_4 * num_areas_part3;
        num_extended_allocations = 0;
    }
    uint64_t expected_reserved_mem = DEFAULT_OS_PAGE_SIZE * num_allocations; // if all pools are touched
    uint64_t actual_reserver_mem = allocation_size * num_allocations;        // if all the owned heaps would be touched
    double readable_reserved = (double)expected_reserved_mem / (SZ_GB);

    uint64_t **variables = (uint64_t **)malloc(num_allocations * sizeof(uint64_t));
    int8_t pid = 0;
    // exhaust part 0 and 1
    for (uint32_t i = 0; i < num_allocations; i++) {
        uint64_t *new_addr = (uint64_t *)cmalloc_from_heap(allocation_size);
        pid = partition_from_addr((uintptr_t)new_addr);
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
    if (num_extended_allocations == 0) {
        // next allocation should be NULL;
        void *nll = cmalloc_from_heap(allocation_size);
        int8_t npid = partition_from_addr((uintptr_t)nll);
        if (npid == pid) {
            result = false;
        }
        cfree(nll);
    }
    for (uint32_t i = 0; i < num_allocations; i++) {
        cfree(variables[i]);
    }
    free(variables);

    // release all the system resources
    if (!callocator_release()) {
        return false;
    }

    if (num_extended_allocations == 0) {
        return result;
    }

    expected_reserved_mem = DEFAULT_OS_PAGE_SIZE * num_extended_allocations; // if all pools are touched
    actual_reserver_mem = allocation_size * num_extended_allocations;        // if all the owned heaps would be touched

    readable_reserved = (double)expected_reserved_mem / (SZ_GB);
    num_allocations = num_extended_allocations;
    variables = (uint64_t **)malloc(num_allocations * sizeof(uint64_t));
    // exhaust part 0, 1, and 2
    for (uint32_t i = 0; i < num_allocations; i++) {
        uint64_t *new_addr = (uint64_t *)cmalloc_from_heap(allocation_size);
        pid = partition_from_addr((uintptr_t)new_addr);
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
    void *nll = cmalloc_from_heap(allocation_size);
    int8_t npid = partition_from_addr((uintptr_t)nll);
    if (npid == pid) {
        result = false;
    }
    cfree(nll);
end:
    for (uint32_t i = 0; i < num_allocations; i++) {
        cfree(variables[i]);
    }
    free(variables);
    // release all the system resources
    if (!callocator_release()) {
        result = false;
    }
    return result;
}

bool test_huge_heaps(void) { return test_heaps(max_huge_size_heap); }

bool test_large_heaps(void) { return test_heaps(max_large_size_heap); }

bool test_medium_heaps(void) { return test_heaps(max_mid_size_heap); }

bool test_small_heaps(void) { return test_heaps(max_small_size_heap); }

bool test_puny_heaps(void) { return test_heaps(max_puny_size_heap); }

bool test_slabs(void)
{
    bool state = true;
    uint64_t allocation_size = 129 * sz_mb;
    uint64_t base_parts = num_areas_part3;
    uint64_t max_count_per_heap = 1;
    uint64_t num_small_areas = base_parts;
    uint64_t num_small_allocations = max_count_per_heap * num_small_areas;

    uint64_t **variables = (uint64_t **)malloc(num_small_allocations * sizeof(uint64_t));

    // exhaust part 0 and 1
    for (uint32_t i = 0; i < num_small_allocations; i++) {
        variables[i] = (uint64_t *)cmalloc_from_heap(allocation_size);
        uintptr_t end = align_up((uintptr_t)variables[i], 256 * sz_mb);
        if ((end - (uintptr_t)variables[i]) < allocation_size) {
            state = false;
            goto end;
        }
        if (variables[i] == NULL) {
            state = false;
            goto end;
        }
    }
    void *nll = cmalloc_from_heap(allocation_size);
    if (nll != NULL) {
        state = false;
    }
end:
    for (uint32_t i = 0; i < num_small_allocations; i++) {
        cfree(variables[i]);
    }
    free(variables);
    return state;
}

bool test_areas(void)
{
    bool state = true;
    uint64_t allocation_size = 129 * sz_mb;
    uint64_t base_parts = num_areas_part3;
    uint64_t num_alloc = base_parts;

    uint64_t **variables = (uint64_t **)malloc(num_alloc * sizeof(uint64_t));
    for (uint32_t i = 0; i < num_alloc; i++) {
        variables[i] = (uint64_t *)cmalloc(allocation_size);
    }
    void *nll = cmalloc_from_heap(allocation_size);
    if (nll != NULL) {
        cfree(nll);
        state = false;
        goto end;
    }

    uintptr_t end_addr = (uintptr_t)variables[num_alloc - 2];
    uintptr_t next_addr = (uintptr_t)variables[num_alloc - 3];
    cfree(variables[num_alloc - 2]);
    cfree(variables[num_alloc - 3]);
    uintptr_t new_addr = (uintptr_t)cmalloc(allocation_size);
    variables[num_alloc - 3] = (uint64_t *)new_addr;
    if (new_addr != next_addr && new_addr != end_addr) {
        state = false;
        goto end;
    }
    new_addr = (uintptr_t)cmalloc(allocation_size);
    variables[num_alloc - 2] = (uint64_t *)new_addr;
    if (new_addr != end_addr) {
        state = false;
        goto end;
    }

    next_addr = (uintptr_t)variables[num_alloc - 5];
    // remove four and try to allocate 780 megs;
    cfree(variables[num_alloc - 2]);
    cfree(variables[num_alloc - 3]);
    cfree(variables[num_alloc - 4]);
    cfree(variables[num_alloc - 5]);
    new_addr = (uintptr_t)cmalloc(780 * sz_mb);
    if (new_addr != next_addr) {
        num_alloc -= 5;
        cfree((void *)new_addr);
        cfree(variables[num_alloc - 1]);
        state = false;
        goto end;
    }
    int32_t pid = partition_from_addr((uintptr_t)new_addr);
    cfree((void *)new_addr);
    variables[num_alloc - 2] = (uint64_t *)cmalloc(allocation_size);
    variables[num_alloc - 3] = (uint64_t *)cmalloc(allocation_size);
    variables[num_alloc - 4] = (uint64_t *)cmalloc(allocation_size);
    variables[num_alloc - 5] = (uint64_t *)cmalloc(allocation_size);
    cfree(variables[num_alloc - 2]);
    nll = cmalloc(256 * sz_mb);
    if (partition_from_addr((uintptr_t)nll) != pid) {
        cfree(nll);
        cfree(variables[num_alloc - 1]);
        num_alloc -= 2;
        state = false;
        goto end;
    }
    variables[num_alloc - 2] = (uint64_t *)cmalloc(allocation_size);
end:
    for (uint32_t i = 0; i < num_alloc; i++) {
        cfree(variables[i]);
    }
    free(variables);
    return state;
}

bool test_huge_alloc(void)
{
    void *gb = (uint64_t *)cmalloc(15 * sz_gb);
    if (gb == NULL) {
        return false;
    }
    cfree(gb);
    gb = (uint64_t *)cmalloc(16 * sz_gb);
    if (gb != NULL) {
        cfree(gb);
        return false;
    }
    return true;
}

bool fillAPool(void)
{
    bool state = true;
    const int num_allocs = 16378;
    uint64_t **allocs = (uint64_t **)malloc(num_allocs * sizeof(uint64_t **));
    for (int i = 0; i < num_allocs; i++) {
        allocs[i] = (uint64_t *)cmalloc(8);
        *allocs[i] = (uint64_t)allocs[i];
    }

    for (int i = 0; i < num_allocs; i++) {
        if (*allocs[i] != (uint64_t)allocs[i]) {
            state = false;
        }
        cfree(allocs[i]);
    }
    free(allocs);

    return state;
}

bool fillASection(void)
{
    bool state = true;
    const int num_pools = 32;
    const int num_allocs = 16378;
    uint64_t **allocs = (uint64_t **)malloc(num_allocs * num_pools * sizeof(uint64_t **));
    for (int s = 0; s < num_pools; s++) {
        for (int i = 0; i < num_allocs; i++) {
            allocs[i + (num_allocs * s)] = (uint64_t *)cmalloc(8);
            *allocs[i + (num_allocs * s)] = (uint64_t)allocs[i + (num_allocs * s)];
        }
    }

    for (int s = 0; s < num_pools; s++) {
        for (int i = 0; i < num_allocs; i++) {
            if (*allocs[i + (num_allocs * s)] != (uint64_t)allocs[i + (num_allocs * s)]) {
                state = false;
            }
            cfree(allocs[i + (num_allocs * s)]);
        }
    }
    free(allocs);
    // fill all 8 pools of one section.
    return state;
}

bool fillAnArea(void)
{
    bool state = true;
    const int num_sections = 8;
    const int num_pools = 32;
    const int num_allocs = 16378;
    const int total_allocs = num_sections * num_pools * num_allocs;
    uint64_t **allocs = (uint64_t **)malloc(total_allocs * sizeof(uint64_t **));

    int index = 0;
    for (int s = 0; s < num_pools * num_sections; s++) {
        for (int i = 0; i < num_allocs; i++) {
            index = i + (num_allocs * s);
            allocs[index] = (uint64_t *)cmalloc(8);
            *allocs[index] = (uint64_t)allocs[index];
        }
    }

    for (int s = 0; s < num_pools * num_sections; s++) {
        for (int i = 0; i < num_allocs; i++) {
            int index = i + (num_allocs * s);
            if (*allocs[index] != (uint64_t)allocs[index]) {
                state = false;
            }
            cfree(allocs[index]);
        }
    }

    free(allocs);

    // fill all 8 pools of one section.
    return state;
}

bool fillAPage(void)
{
    bool state = true;
    const int num_allocs = 1398094;
    uint64_t **allocs = (uint64_t **)malloc(num_allocs * sizeof(uint64_t **));

    for (int i = 0; i < num_allocs; i++) {
        uint64_t *addr = (uint64_t *)cmalloc_from_heap(8);
        allocs[i] = addr;
        *allocs[i] = (uint64_t)allocs[i];
    }

    for (int i = 0; i < num_allocs; i++) {

        if (*allocs[i] != (uint64_t)allocs[i]) {
            state = false;
        }
        cfree(allocs[i]);
    }

    free(allocs);

    // fill all 8 pools of one sections
    return state;
}

void run_tests(void)
{

    START_TEST(Allocator, {});
    TEST(Allocator, pools_small, { EXPECT(test_pools_small()); });
    TEST(Allocator, medium_pools, { EXPECT(test_medium_pools()); });
    TEST(Allocator, large_pools, { EXPECT(test_large_pools()); });
    TEST(Allocator, puny_heaps, { EXPECT(test_puny_heaps()); });
    TEST(Allocator, small_heaps, { EXPECT(test_small_heaps()); });
    TEST(Allocator, medium_heaps, { EXPECT(test_medium_heaps()); });
    TEST(Allocator, large_heaps, { EXPECT(test_large_heaps()); });
    TEST(Allocator, huge_heaps, { EXPECT(test_huge_heaps()); });
    TEST(Allocator, slabs, { EXPECT(test_slabs()); });
    TEST(Allocator, huge_alloc, { EXPECT(test_huge_alloc()); });
    TEST(Allocator, areas, { EXPECT(test_areas()); });
    TEST(Allocator, fillAPool, { EXPECT(fillAPool()); });
    TEST(Allocator, fillASection, { EXPECT(fillASection()); });
    TEST(Allocator, fillAnArea, { EXPECT(fillAnArea()); });
    TEST(Allocator, fillAPage, { EXPECT(fillAPage()); });
    END_TEST(Allocator, {});
    callocator_release();
}

bool testAreaFail(void)
{
    // Why am I stalling this!
    // this is sort of the last fallback step and allows you allocate all the
    // ranges with a single thread. if allocations fail, it can move into other
    // partition sets. then I can move into the thread free part. Which should
    // be a lot simpler.
    //   then it is just testing and cleaning things up..
    void *m = cmalloc_area(48 * sz_mb, AT_FIXED_32);
    //

    // two areas should be marked as bad.
    // see if those areas are marked as bad.
    // how should we mark those areas as bad.
    // if the size of an area is -1, then it is bad.
    void *bb = cmalloc(24);
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
    cfree(bb);
    cfree(m);
    callocator_release();
    bb = cmalloc(24);
    cfree(bb);
    callocator_release();
    return false;
}



uint32_t numConsecutiveZeros(uint64_t test)
{
    if(test == 0)
    {
        return 64;
    }
    
    uint32_t lz = __builtin_clzll(test);
    uint32_t tz = __builtin_ctzll(test);
    if(lz == 0)
    {
        uint32_t l1 = __builtin_clzll(~test);
        if((64 - l1) <= tz)
        {
            return tz;
        }
        test &= (1UL << (64 - (l1 - 1))) - 1;
    }
    
    uint32_t mz = MAX(lz, tz);
    if((64 - (lz + tz)) <= mz)
    {
        return mz;
    }
    
    if(tz == 0)
    {
        test = test >> __builtin_ctzll(~test);
    }
    else
    {
        test = test >> (tz + 1);
    }
    
    while(test >= (1UL << mz))
    {
        tz = __builtin_ctzll(test);
        mz = mz ^ ((mz ^ tz) & -(mz < tz));
        test = test >> (tz + 1);
        test = test >> __builtin_ctzll(~test);
    }
    return mz;
}

uint32_t _numConsecutiveZeros(uint64_t test)
{
    int32_t count = 0;
    int32_t result = 0;
    for(int i = 0; i < 64; i++)
    {
        if(!(test & (1UL << i)))
        {
            count++;
        }
        else
        {
            if(count > result)
            {
                result = count;
            }
            count = 0;
        }
    }
    if(count > result)
    {
        result = count;
    }
    return result;
}
uint32_t minor_test(void)
{
    START_TEST(allocator, {});
    uint32_t lz = 0;
    MEASURE_TIME(allocator, num_zeros, {
        for (uint64_t j = 0; j < 100000000; j++) {
            uint64_t test = j | (j << 32);
            lz += __builtin_ctzll(test);
            __asm__ __volatile__("");
        }
    });
    MEASURE_TIME(allocator, num_zeros, {
        for (uint64_t j = 0; j < 100000000; j++) {
            uint64_t test = j | (j << 32);
            lz = numConsecutiveZeros(test);
            //lz = numConsecutiveZeros(~j);
            __asm__ __volatile__("");
        }
    });
    MEASURE_TIME(allocator, num_zeros_naive, {
        for (uint64_t j = 0; j < 100000000; j++) {
            uint64_t test = j | (j << 32);
            lz = _numConsecutiveZeros(test);
            if(lz != numConsecutiveZeros(test))
            {
                exit(1);
            }
            __asm__ __volatile__("");
        }
    });
    END_TEST(allocator, {});
    return lz;
}

void test_size_iter(uint32_t alloc_size, size_t num_items, size_t num_loops)
{

    START_TEST(allocator, {});
    char **variables = (char **)malloc(num_items * sizeof(char *));

    MEASURE_TIME(allocator, cmalloc, {
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
    MEASURE_TIME(Allocator, mi_malloc, {
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

int test(void *p)
{
    char *test = (char *)cmalloc(16);
    cfree(test);
    return 1;
}

void test_size_arena_iter(uint32_t alloc_size, size_t num_items, size_t num_loops)
{

    START_TEST(allocator, {});
    char **variables = (char **)malloc(num_items * sizeof(char *));

    MEASURE_TIME(allocator, cmalloc, {
        for (uint64_t j = 0; j < num_loops; j++) {
            for (uint64_t i = 0; i < num_items; i++) {
                variables[i] = (char *)cmalloc(alloc_size);
            }
            for (uint64_t i = 0; i < num_items; i++) {
                cfree(variables[i]);
            }
        }
    });
    END_TEST(allocator, {});
    free(variables);
}

void test_new_heap(size_t a_exp, size_t num_items_l0, size_t num_items_l1, size_t num_items_l2, size_t mult )
{
    // [ ] reserve all memory
    // [ ] reserve all levels
    // [ ] release all memory
    // [ ] release all levels
    //
    size_t err_count = 0;
    size_t size_l0 = (1 << (a_exp - 18)) * mult;
    size_t size_l1 = (1 << (a_exp - 12)) * mult;
    size_t size_l2 = (1 << (a_exp - 6)) * mult;
    size_t num_items = num_items_l0 + num_items_l1 + num_items_l2;
    size_t size = (num_items_l0 * size_l0) + (num_items_l1 * size_l1) + (num_items_l2 * size_l2);
    void *mem = cmalloc_arena(SZ_MB * 4, AT_FIXED_4);
    Arena *nh = arena_init((uintptr_t)mem, 0, a_exp);
    char **variables = (char **)malloc(num_items * size);
    size_t current_count = 0;
    
    for(int i = 0; i < num_items_l2; i++)
    {
        void* all = arena_get_block(nh, size_l2);
        if(all == NULL)
        {
            err_count++;
        }
        variables[current_count++] = all;
    }
    
    for(int i = 0; i < num_items_l1; i++)
    {
        void* all = arena_get_block(nh, size_l1);
        if(all == NULL)
        {
            err_count++;
        }
        variables[current_count++] = all;
    }
    
    for(int i = 0; i < num_items_l0; i++)
    {
        void* all = arena_get_block(nh, size_l0);
        if(all == NULL)
        {
            err_count++;
        }
        variables[current_count++] = all;
    }
    print_header(nh,(uintptr_t)variables[0]);
    for (uint64_t i = 0; i < num_items; i++) {
        arena_free(nh, variables[i], false);
    }
    print_header(nh,(uintptr_t)variables[0]);
    free(variables);
    printf("error count: %lu\n", err_count);
}


int main()
{
    int c1 = (64 - 16) * 1;
    int c2 = (64 - 4) * 63;
    int c3 = (64 - 2) * 63*64;
    
    int l0_count =  c1 + c2 + c3;
    int l1_count = 63 * 64;
    int l2_count = 63;
    test_new_heap(22, 22, 0, 0, 3);
    //test_new_heap(63, 1024*64);
    //test_new_heap(63, 1024);
    //test_new_heap(55, 16);
    /*
    test_new_heap(22, 48, 0, 0, 2);
    test_new_heap(22, 48, 63, 63, 1);
    test_new_heap(22, 48, 63, 63, 1);
    test_new_heap(22, l0_count, 0, 0, 1);
    test_new_heap(22, 0, l1_count, 0, 1);
    test_new_heap(22, 0, 0, l2_count, 1);
    */
    /*
    printf("Arena :%llu\n", sizeof(Arena));
    printf("Arena :%llu\n", sizeof(Arena_L0));
    printf("Arena :%llu\n", sizeof(Arena_L1));
    printf("Arena :%llu\n", sizeof(Arena_L2));
     */
    //minor_test();
    // intermittently allocate a block
    //
    
    // size lists for the smallest and medium
    //  - sort by remaining size per block.
    //  - 2, 4, 8, 16, 32
    
    //   thrd_t trd;
    //   thrd_create(&trd, &test, NULL);
    //   blach();
     //run_tests();
    //   void* m = cmalloc_at(DEFAULT_OS_PAGE_SIZE*4, ((uintptr_t)32 << 40)+DEFAULT_OS_PAGE_SIZE);
    //   cfree(m);
    //   m = cmalloc_os(123);
    //   cfree(m);

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
