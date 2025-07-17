
#define CTEST_ENABLED
#include "../ctest/ctest.h"
#include "arena.h"
#include "callocator.inl"
#include <stdlib.h>
#include "pool.h"
//#include "mimalloc.h"
__thread api_tracer _test_tracer = {0,0,0,0,0,0,0,0,0,0};
//extern __thread api_tracer _test_tracer;


void* mi_malloc(size_t s)
{
    return NULL;
}
void mi_free(void* p)
{
    
}
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

bool test_alloc(size_t allocation_size, bool test_align)
{

    bool result = true;
    bool from_pool = false;
    if(allocation_size <= (1 << 15))
    {
        from_pool = true;
    }

    // size > 32k, goes into an arena.
    // size > 4m, goes into the largest arena.
    // 
    //
    //  in what arena is this pool size going to be allocated
    //  the number of pools.
    //
    
    int32_t row_map[] = {0,1,2, 3,4,5, 5,5,5, 5,6,6, 6, 6, 6, 6, 6};
    uint8_t pc = size_to_pool(allocation_size);
    int32_t row = pc/8;
    uint8_t arena_idx = row_map[row];
    size_t area_size = area_size_from_partition_id(arena_idx);
    
    size_t pool_size = area_size >> 6;
    
    uint64_t pools_per_section = 63;
    uint64_t max_count_per_pool = (pool_size - 64) / allocation_size;
    if(!from_pool)
    {
        max_count_per_pool = 63;
        pool_size = area_size;
    }
    uint64_t num_small_sections = 64;
    uint64_t num_pools = num_small_sections * pools_per_section;
    uint64_t num_small_allocations = num_pools * max_count_per_pool;

    //uint64_t expected_reserved_mem = DEFAULT_OS_PAGE_SIZE * num_small_allocations; // if all pools are touched
    //uint64_t actual_reserver_mem = allocation_size * num_small_allocations; // if all the owned pages would be touched

    uint64_t **variables = (uint64_t **)malloc(num_small_allocations * sizeof(uint64_t));

    //double readable_reserved = (double)expected_reserved_mem / (SZ_GB);
    // exhaust part 0 and 1
    
    for (uint32_t i = 0; i < num_small_allocations; i++) {
        void *all = cmalloc(allocation_size);
        int32_t na =_test_tracer.num_allocations;
        if (all == NULL) {
            result = false;
            goto end;
        }
        variables[i] = (uint64_t *)all;
        uintptr_t end = align_up((uintptr_t)variables[i], pool_size);
        intptr_t delta = (end - (uintptr_t)variables[i]);
        if (delta < allocation_size) {
            result = false;
            goto end;
        }
        if (variables[i] == NULL) {
            result = false;
            goto end;
        }
    }
    
    // next allocation should be from a different partition id
    int8_t pid = partition_allocator_from_addr((uintptr_t)variables[0]);
    void *nll = cmalloc(allocation_size);
    int8_t npid = partition_allocator_from_addr((uintptr_t)nll);
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

    if(callocator_release())
    {
        return true;
    }
    else
    {
        return false;
    }
}

bool test_pools_small(void) { return test_alloc(max_small_size, false); }
bool test_medium_pools(void) { return test_alloc(max_mid_size,false); }

bool test_large_pools(void) { return test_alloc(max_large_size,false); }

bool test_alloc_aligned(size_t allocation_size)
{
    bool result = true;
    bool from_pool = false;
    if(allocation_size <= (1 << 15))
    {
        from_pool = true;
    }

    // size > 32k, goes into an arena.
    // size > 4m, goes into the largest arena.
    //
    //
    //  in what arena is this pool size going to be allocated
    //  the number of pools.
    //
    
    int32_t row_map[] = {0,1,2, 3,4,5, 5,5,5, 5, 0, 1, 2, 3, 4, 5, 6};
    uint8_t pc = size_to_pool(allocation_size);
    int32_t row = pc/8;
    uint8_t arena_idx = row_map[row];
    size_t area_size = area_size_from_partition_id(arena_idx);
    
    size_t pool_size = area_size >> 6;
    
    uint64_t pools_per_section = 63;
    uint64_t max_count_per_pool = (pool_size - 64) / allocation_size;
    if(!from_pool)
    {
        max_count_per_pool = 63;
        pool_size = area_size;
    }
    uint64_t num_small_sections = 64;
    uint64_t num_pools = num_small_sections * pools_per_section;
    uint64_t num_small_allocations = num_pools * max_count_per_pool;

    //uint64_t expected_reserved_mem = DEFAULT_OS_PAGE_SIZE * num_small_allocations; // if all pools are touched
    //uint64_t actual_reserver_mem = allocation_size * num_small_allocations; // if all the owned pages would be touched

    uint64_t **variables = (uint64_t **)malloc(num_small_allocations * sizeof(uint64_t));

    //double readable_reserved = (double)expected_reserved_mem / (SZ_GB);
    // exhaust part 0 and 1
    uint32_t shift = 0;
    _test_tracer.fallback = 0;
    for (uint32_t i = 0; i < num_small_allocations; i++) {
        if(_test_tracer.fallback)
        {
            num_small_allocations = i;
            break;
        }
        uint32_t alignment = 8<<(shift%24);
        if(alignment > allocation_size)
        {
            alignment = 8;
            shift = 0;
        }
        else
        {
            shift++;
        }
        pc = size_to_pool(MAX(alignment, allocation_size));
        row = pc/8;
        arena_idx = row_map[MIN(row, 16)];
        area_size = area_size_from_partition_id(arena_idx);
        if(row < 10)
        {
            pool_size = area_size >> 6;
        }
        else
        {
            pool_size = area_size;
        }
        
        void *all = caligned_alloc(alignment, allocation_size);
        if (all == NULL) {
            num_small_allocations = i;
            result = false;
            goto end;
        }
        
        if(!IS_ALIGNED(all, alignment))
        {
            num_small_allocations = i;
            result = false;
            cfree(all);
            goto end;
        }
        uintptr_t end = align_up((uintptr_t)all, pool_size);
        intptr_t delta = (end - (uintptr_t)all);
        if (delta < allocation_size) {
            num_small_allocations = i;
            result = false;
            cfree(all);
            goto end;
        }
        variables[i] = (uint64_t *)all;
    }
    
    /*
    // next allocation should be from a different partition id
    int8_t pid = partition_allocator_from_addr((uintptr_t)variables[0]);
    void *nll = cmalloc(allocation_size);
    int8_t npid = partition_allocator_from_addr((uintptr_t)nll);
    if (npid == pid) {
        result = false;
    }
    cfree(nll);
     */
    
end:
    
    for (uint32_t i = 0; i < num_small_allocations; i++) {
        cfree(variables[i]);
    }
    free(variables);
    // release all the system resources
    
    if(callocator_release())
    {
        return true;
    }
    else
    {
        return false;
    }
}

bool test_huge_heaps(void) { return test_alloc_aligned(max_huge_size_heap); }

bool test_large_heaps(void) { return test_alloc_aligned(max_large_size_heap); }

bool test_medium_heaps(void) { return test_alloc_aligned(max_mid_size_heap); }

bool test_small_heaps(void) { return test_alloc_aligned(max_small_size_heap); }

bool test_puny_heaps(void) { return test_alloc_aligned(max_puny_size_heap); }

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
        variables[i] = (uint64_t *)caligned_alloc(512, allocation_size);
        uintptr_t all = (uintptr_t)variables[i];
        uintptr_t end = align_up(all, 256 * sz_mb*64);
        if(all != end)
        {
            if ((end - (uintptr_t)variables[i]) < allocation_size) {
                state = false;
                goto end;
            }
        }
        
        if (variables[i] == NULL) {
            state = false;
            goto end;
        }
    }
    void *nll = caligned_alloc(1024, allocation_size);
    if (nll != NULL) {
        state = false;
    }
    void* gb = (uint64_t *)cmalloc(16 * sz_gb);
    if (gb != NULL) {
        cfree(gb);
        return false;
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
    void *nll = caligned_alloc(2048, allocation_size);
    if (nll != NULL) {
        cfree(nll);
        state = false;
        goto end;
    }

    uintptr_t end_addr = (uintptr_t)variables[num_alloc - 2];
    uintptr_t next_addr = (uintptr_t)variables[num_alloc - 3];
    cfree(variables[num_alloc - 2]);
    variables[num_alloc - 2] = NULL;
    cfree(variables[num_alloc - 3]);
    variables[num_alloc - 3] = NULL;
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
    variables[num_alloc - 2] = NULL;
    cfree(variables[num_alloc - 3]);
    variables[num_alloc - 3] = NULL;
    cfree(variables[num_alloc - 4]);
    variables[num_alloc - 4] = NULL;
    cfree(variables[num_alloc - 5]);
    variables[num_alloc - 5] = NULL;
    new_addr = (uintptr_t)cmalloc(780 * sz_mb);
    if (new_addr != next_addr) {
        num_alloc -= 5;
        cfree((void *)new_addr);
        cfree(variables[num_alloc - 1]);
        variables[num_alloc - 1] = NULL;
        state = false;
        goto end;
    }
    int32_t pid = partition_id_from_addr((uintptr_t)new_addr);
    cfree((void *)new_addr);
    variables[num_alloc - 2] = (uint64_t *)cmalloc(allocation_size);
    variables[num_alloc - 3] = (uint64_t *)cmalloc(allocation_size);
    variables[num_alloc - 4] = (uint64_t *)cmalloc(allocation_size);
    variables[num_alloc - 5] = (uint64_t *)cmalloc(allocation_size);
    cfree(variables[num_alloc - 2]);
    variables[num_alloc - 2] = NULL;
    nll = cmalloc(256 * sz_mb);
    if (partition_id_from_addr((uintptr_t)nll) != pid) {
        cfree(nll);
        cfree(variables[num_alloc - 1]);
        variables[num_alloc - 1] = NULL;
        num_alloc -= 2;
        state = false;
        goto end;
    }
    else
    {
        if(nll != NULL)
        {
            cfree(nll);
        }
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
    const int num_allocs = 16373;
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
        uint64_t *addr = (uint64_t *)caligned_alloc(4096, 8);
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
    if(!callocator_release())
    {
        printf("leak at end of test\n");
    }
}




uint32_t numConsecutiveZeros(uint64_t test)
{
    if(test == 0)
    {
        return 64;
    }
    if((test & (test - 1)) == 0)
    {
        return MAX(__builtin_clzll(test), __builtin_ctzll(test));
    }

    if((~test & (~test >> 1)) == 0)
    {
        return 1;
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
        test &= (1ULL << (64 - (l1 - 1))) - 1;
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
    
    while(test >= (1ULL << mz))
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
        if(!(test & (1ULL << i)))
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

void test_size_iter_leak(uint32_t alloc_size, size_t num_items, size_t num_loops, int t)
{

    START_TEST(allocator, {});
    char **variables = (char **)malloc(num_items * sizeof(char *));
    if(t)
    {
        MEASURE_TIME(allocator, cmalloc, {
            for (uint64_t j = 0; j < num_loops; j++) {
                for (uint64_t i = 0; i < num_items; i++) {
                    variables[i] = (char *)cmalloc(alloc_size);
                }
            }
        });
    }
    else
    {
        MEASURE_TIME(Allocator, mi_malloc, {
            for (uint64_t j = 0; j < num_loops; j++) {
                for (uint64_t i = 0; i < num_items; i++)
                    variables[i] = (char *)mi_malloc(alloc_size);
            }
        });
    }
    END_TEST(allocator, {});
    free(variables);
}
void test_size_iter_immediate(uint32_t alloc_size, size_t num_items, size_t num_loops, int t)
{

    START_TEST(allocator, {});
    char **variables = (char **)malloc(num_items * sizeof(char *));
    if(t)
    {
        MEASURE_TIME(allocator, cmalloc, {
            for (uint64_t j = 0; j < num_loops; j++) {
                for (uint64_t i = 0; i < num_items; i++) {
                    variables[i] = (char *)cmalloc(alloc_size);
                    cfree(variables[i]);
                }
                
            }
        });
    }
    else
    {
        MEASURE_TIME(Allocator, mi_malloc, {
            for (uint64_t j = 0; j < num_loops; j++) {
                for (uint64_t i = 0; i < num_items; i++)
                {
                    variables[i] = (char *)mi_malloc(alloc_size);
                    mi_free(variables[i]);
                }
            }
        });
    }
    END_TEST(allocator, {});
    free(variables);
}
void test_size_iter(uint32_t alloc_size, size_t num_items, size_t num_loops, int t)
{

    START_TEST(allocator, {});
    char **variables = (char **)malloc(num_items * sizeof(char *));
    if(t)
    {
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
    }
    else
    {
        MEASURE_TIME(Allocator, mi_malloc, {
            for (uint64_t j = 0; j < num_loops; j++) {
                for (uint64_t i = 0; i < num_items; i++)
                    variables[i] = (char *)mi_malloc(alloc_size);

                for (uint64_t i = 0; i < num_items; i++)
                    mi_free(variables[i]);
            }
        });
    }
    END_TEST(allocator, {});
    free(variables);
}
void test_size_iter_scatter(uint32_t alloc_size, size_t num_items, size_t num_loops, int t)
{

    START_TEST(allocator, {});
    char **variables = (char **)malloc(num_items * sizeof(char *));
    size_t sizes[] = {alloc_size, alloc_size*2, alloc_size*4};
    if(t)
    {
        MEASURE_TIME(allocator, cmalloc, {
            for (uint64_t j = 0; j < num_loops; j++) {
                for (uint64_t i = 0; i < num_items; i++) {
                    variables[i] = (char *)cmalloc(sizes[i%3]);
                }
                for (uint64_t i = 0; i < num_items; i++) {
                    cfree(variables[i]);
                }
            }
        });
    }
    else
    {
        MEASURE_TIME(Allocator, mi_malloc, {
            for (uint64_t j = 0; j < num_loops; j++) {
                for (uint64_t i = 0; i < num_items; i++)
                    variables[i] = (char *)mi_malloc(sizes[i%3]);

                for (uint64_t i = 0; i < num_items; i++)
                    mi_free(variables[i]);
            }
        });
    }
    END_TEST(allocator, {});
    free(variables);
}
 void test_size_iter_scatter_leak(uint32_t alloc_size, size_t num_items, size_t num_loops, int t)
 {

     START_TEST(allocator, {});
     char **variables = (char **)malloc(num_items * sizeof(char *));
     size_t sizes[] = {alloc_size, alloc_size*2, alloc_size*4};
     if(t)
     {
         MEASURE_TIME(allocator, cmalloc, {
             for (uint64_t j = 0; j < num_loops; j++) {
                 for (uint64_t i = 0; i < num_items; i++) {
                     variables[i] = (char *)cmalloc(sizes[i%3]);
                 }
             }
         });
     }
     else
     {
         MEASURE_TIME(Allocator, mi_malloc, {
             for (uint64_t j = 0; j < num_loops; j++) {
                 for (uint64_t i = 0; i < num_items; i++)
                     variables[i] = (char *)mi_malloc(sizes[i%3]);
             }
         });
     }
     END_TEST(allocator, {});
     free(variables);
 }
void test_size_iter_sparse(size_t num_items, size_t num_loops, int t)
{

    size_t alloc_size = 8;
    START_TEST(allocator, {});
    char **variables = (char **)malloc(num_items * sizeof(char *));
    if(t)
    {
        MEASURE_TIME(allocator, cmalloc, {
            for (uint64_t j = 0; j < num_loops; j++) {
                for (uint64_t i = 0; i < num_items; i++) {
                    alloc_size += 8;
                    variables[i] = (char *)cmalloc(alloc_size%1024);
                }
                for (uint64_t i = 0; i < num_items; i++) {
                    cfree(variables[i]);
                }
            }
        });
    }
    else
    {
        MEASURE_TIME(Allocator, mi_malloc, {
            for (uint64_t j = 0; j < num_loops; j++) {
                for (uint64_t i = 0; i < num_items; i++)
                {
                    alloc_size += 8;
                    variables[i] = (char *)mi_malloc(alloc_size%1024);
                }
                for (uint64_t i = 0; i < num_items; i++)
                    mi_free(variables[i]);
            }
        });
    }
    
    END_TEST(allocator, {});
    free(variables);
}

void test_size_iter_sparse_reverse(size_t num_items, size_t num_loops,int t)
{

    size_t alloc_size = 8;
    START_TEST(allocator, {});
    char **variables = (char **)malloc(num_items * sizeof(char *));
    if(t)
    {
        MEASURE_TIME(allocator, cmalloc, {
            for (uint64_t j = 0; j < num_loops; j++) {
                for (uint64_t i = 0; i < num_items; i++) {
                    alloc_size += 8;
                    variables[i] = (char *)cmalloc(alloc_size%1024);
                }
                for (int64_t i = num_items - 1; i >= 0; i--) {
                    cfree(variables[i]);
                }
            }
        });
    }
    else
    {
        MEASURE_TIME(Allocator, mi_malloc, {
            for (uint64_t j = 0; j < num_loops; j++) {
                for (uint64_t i = 0; i < num_items; i++)
                {
                    alloc_size += 8;
                    variables[i] = (char *)mi_malloc(alloc_size%1024);
                }
                for (uint64_t i = 0; i < num_items; i++)
                    mi_free(variables[i]);
            }
        });
    }
    
    END_TEST(allocator, {});
    free(variables);
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
    /*
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
    cfree_arena(mem);
     */
}
int test(void *p)
{
    char *test = (char *)cmalloc(16);
    cfree(test);
    return 1;
}

#include <sys/mman.h>
int32_t temp_hit_counter = 0;

int main(void)
{
    
    /*
    size_t size = 1ULL << 22;
    void *t1 = mmap(BASE_ADDR(0), size, PROT_NONE, (MAP_PRIVATE | MAP_ANONYMOUS), -1, 0);
    int32_t p1 = mprotect(t1, size, (PROT_READ | PROT_WRITE));
    int32_t res = munmap(t1, size);
    void *t2 = mmap(BASE_ADDR(0)+0x40000, size, PROT_NONE, (MAP_PRIVATE | MAP_ANONYMOUS), -1, 0);
    int32_t p2 = mprotect(t2, size, (PROT_READ | PROT_WRITE));
    int32_t res2 = munmap(t2, size);
     */
    //void *t2 = mmap((1ULL << 39), 1ULL << 16, PROT_NONE, (MAP_PRIVATE | MAP_ANONYMOUS), -1, 0);
    //cfree(BASE_ADDR(0) - 8);
    //thrd_t trd;
    //thrd_create(&trd, &test, NULL);
    // test_new_heap();
    //thrd_t trd;
    //thrd_create(&trd, &test, NULL);
    //thrd_t trd2;
    //thrd_create(&trd2, &test, NULL);
    //  blach();
    run_tests();
    //  void* m = cmalloc_at(DEFAULT_OS_PAGE_SIZE*4, ((uintptr_t)32 << 40)+DEFAULT_OS_PAGE_SIZE);
    //  cfree(m);
    //  m = cmalloc_os(123);
    //  cfree(m);

    
    // TODO
    /*
     // tests
     [ ] - Test structs.
         - run_test:
         - trace_struct:
         - test_struct:
             - allocation:
             -
         - Define test.
             - run_test(ALLOCATOR)
     
     [ ] Run the old tests with the new code path.
     
     [ ] Pool allocation tests. All sizes and bounds.
        - run on one partition allocator.
        - use test struct to monitor internal state.
        - check alignment.
     [ ] Arena allocation tests. All sizes and bounds.
        - Alignment tests.
     [ ] Slab allocations tests. All sizes and bounds.
     
     // new tests.
     [ ] Zero allocation tests.
     [ ] Reallocation tests.
     
     [x] Remove all the old code paths.

     // new thread local approach
     [ ] Allocator and thread locals.
     [ ] Partition allocators and mutexes.

     // new tests and perf checks.
     [ ] aligned_alloc test.
     [ ] zalloc_tests.
     [ ] realloc_tests.

     // compare with mimalloc
     [ ] mimalloc comparison.
     [ ] thread tests.
     [ ] stress tests.

     // see how it compares against the big boys.
     [ ] Benchmarks. compare against other allocators.
     */
    uint32_t temp_count = 8181 + 511;
    char **variables = (char **)malloc(temp_count * sizeof(char *));

    for (uint64_t i = 0; i < temp_count; i++) {
        variables[i] = (char *)cmalloc(8);
    }
    for (uint64_t i = 0; i < temp_count; i++) {
        cfree(variables[i]);
    }
    variables[0] = (char *)cmalloc(8);
    variables[1] = (char *)cmalloc(8);
    cfree(variables[0]);
    cfree(variables[1]);
    
    char* c = (char *)cmalloc(1ULL << 23);
    cfree(c);
    
    int test_local = 1;
    for (int i = 0; i < 14; i++) {
        test_size_iter(1 << i, NUMBER_OF_ITEMS, NUMBER_OF_ITERATIONS, 1);
        //printf("hit %d \n", temp_hit_counter);
    }
    for (int i = 13; i >= 0; i--) {
        //test_size_iter(1 << i, NUMBER_OF_ITEMS, NUMBER_OF_ITERATIONS, 0);
        //printf("hit %d \n", temp_hit_counter);
    }
    for (int i = 0; i < 10; i++) {
        //test_size_iter(1 << i, NUMBER_OF_ITEMS, NUMBER_OF_ITERATIONS, 1);
        //printf("hit %d \n", temp_hit_counter);
    }
    for (int i = 0; i < 10; i++) {
        //test_size_iter_scatter(1 << i, NUMBER_OF_ITEMS, NUMBER_OF_ITERATIONS, 1);
        //printf("hit %d \n", temp_hit_counter);
    }

    
    return 0;
}
