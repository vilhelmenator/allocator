//
//  area.inl
//  MemPoolTests
//
//  Created by Vilhelm Sævarsson on 23.6.2022.
//  Copyright © 2022 Vilhelm Sævarsson. All rights reserved.
//

#ifndef area_inl
#define area_inl

#include "bitmask.inl"
static inline int32_t get_next_mask_idx(uint64_t mask, uint32_t cidx)
{
    uint64_t msk_cpy = mask << cidx;
    if (msk_cpy == 0 || (cidx > 63)) {
        return -1;
    }
    return __builtin_clzll(msk_cpy) + cidx;
}
static bool safe_to_aquire(void *base, void *ptr, size_t size, uintptr_t end)
{
    if (base == ptr) {
        return true;
    }
    uintptr_t range = (uintptr_t)((uint8_t *)ptr + size);
    if (range > end) {
        return false;
    }
    return true;
}

static spinlock alloc_lock = {0};
void *alloc_memory_aligned(void *base, uintptr_t end, size_t size, size_t alignment)
{
    // base must be aligned to alignment
    // size must be at least the size of an os page.
    // alignment is smaller than a page size or not a power of two.
    if (!safe_to_aquire(base, NULL, size, end)) {
        return NULL;
    }
    // this is not frequently called
    spinlock_lock(&alloc_lock);
    void *ptr = alloc_memory(base, size, false);
    if (ptr == NULL) {
        goto err;
    }

    if (!safe_to_aquire(base, ptr, size, end)) {
        free_memory(ptr, size);
        ptr = NULL;
        goto err;
    }

    if ((((uintptr_t)ptr & (alignment - 1)) != 0)) {
        // this should happen very rarely, if at all.
        // release our failed attempt.
        free_memory(ptr, size);

        // Now we attempt to overallocate
        size_t adj_size = size + alignment;
        ptr = alloc_memory(base, adj_size, false);
        if (ptr == NULL) {
            goto err;
        }

        // if the new ptr is not in our current partition set
        if (!safe_to_aquire(base, ptr, adj_size, end)) {
            free_memory(ptr, adj_size);
            ptr = NULL;
            goto err;
        }

        // if we got our aligned memory
        if (((uintptr_t)ptr & (alignment - 1)) != 0) {
            goto success;
        }
        // we are still not aligned, but we have an address that is aligned.
        free_memory(ptr, adj_size);

        void *aligned_p = (void *)(((uintptr_t)ptr + (alignment - 1)) & ~(alignment - 1));
        // get our aligned address
        ptr = alloc_memory(aligned_p, size, false);
        if (ptr != aligned_p) {
            // Why would this fail?
            free_memory(ptr, size);
            ptr = NULL;
            goto err;
        }
    }
success:
    if (!commit_memory(ptr, size)) {
        // something is greatly foobar.
        // not allowed to commit the memory we reserved.
        free_memory(ptr, size);
        ptr = NULL;
    }
err:
    spinlock_unlock(&alloc_lock);
    return ptr;
}
static inline int32_t find_first_nones(uintptr_t x, int64_t n)
{
    int64_t s;
    while (n > 1) {
        s = n >> 1;
        x = x & (x << s);
        n = n - s;
    }
    return 63 - (x == 0 ? 64 : __builtin_clzll(x));
}

static inline int32_t find_first_nzeros(uintptr_t x, int64_t n) { return find_first_nones(~x, n); }

static const uintptr_t _Area_small_area_mask = UINT8_MAX;
static const uintptr_t _Area_medium_area_mask = UINT16_MAX;
static const uintptr_t _Area_large_area_mask = UINT32_MAX;

static cache_align const uintptr_t area_type_to_exponent[] = {
    25, // 2^25 == 32MB
    26, // 2^26 == 64MB
    27, // 2^27 == 128MB
    28  // 2^28 == 256MB
};

static inline uint32_t area_get_id(const Area *a) { return (uint32_t)(a->partition_mask & UINT32_MAX); }
static inline AreaType area_get_type(const Area *a) { return (AreaType)((a->partition_mask >> 32) & 0xf); }
static inline ContainerType area_get_container_type(const Area *a)
{
    return (ContainerType)((a->partition_mask >> 32) & 0xf0);
}

static inline size_t area_get_range(const Area *a) { return (AreaType)((a->partition_mask >> 48)); }
static inline size_t area_get_size(const Area *a)
{
    return (1 << area_get_type(a)) * BASE_AREA_SIZE * area_get_range(a);
}

static inline void area_set_container_type(Area *a, ContainerType ct)
{
    a->partition_mask = (a->partition_mask & 0xffff00fffffffff) | (((uint64_t)ct) << 32);
}

static inline void area_init(Area *a, size_t pid, AreaType at, size_t range)
{
    a->active_mask.whole = 0;
    a->constr_mask.whole = 0;
    a->partition_mask = pid | ((uint64_t)at << 32) | (range << 48);
}

static inline bool area_is_empty(const Area *a) { return bitmask_is_empty_hi(&a->active_mask); }
static inline bool area_is_free(const Area *a) { return area_is_empty(a); }
static inline bool area_is_claimed(const Area *a, const uint8_t idx) { return bitmask_is_set_hi(&a->constr_mask, idx); }
static inline void area_free_idx(Area *a, uint8_t i) { bitmask_free_idx_hi(&a->active_mask, i); }
static inline void area_free_all(Area *a) { bitmask_free_all(&a->active_mask); }
static inline void area_reserve_idx(Area *a, uint8_t i)
{
    bitmask_reserve_hi(&a->constr_mask, i);
    bitmask_reserve_hi(&a->active_mask, i);
}

static inline void area_reserve_all(Area *a)
{
    bitmask_reserve_all(&a->constr_mask);
    bitmask_reserve_all(&a->active_mask);
}

static int8_t area_get_section_count(Area *a)
{
    const AreaType at = area_get_type(a);
    if (at == AT_FIXED_32) {
        return 8;
    } else if (at == AT_FIXED_64) {
        return 16;
    } else if (at == AT_FIXED_128) {
        return 32;
    } else {
        return 1;
    }
}

static inline void area_claim_idx(Area *a, uint8_t idx) { bitmask_reserve_hi(&a->constr_mask, idx); }
static inline int8_t area_claim_section(Area *a)
{
    int8_t idx = bitmask_allocate_bit_hi(&a->active_mask);
    area_claim_idx(a, idx);
    return idx;
}
static bool area_is_full(const Area *a)
{
    if (bitmask_is_full_hi(&a->active_mask)) {
        return true;
    }
    if (area_get_type(a) == AT_FIXED_32) {
        return ((a->active_mask.whole >> 32) & _Area_small_area_mask) == _Area_small_area_mask;
    } else if (area_get_type(a) == AT_FIXED_64) {
        return ((a->active_mask.whole >> 32) & _Area_medium_area_mask) == _Area_medium_area_mask;
    } else {
        return ((a->active_mask.whole >> 32) & _Area_large_area_mask) == _Area_large_area_mask;
    }
}

static inline Area *area_from_addr(uintptr_t p)
{
    static const uint64_t masks[] = {~(AREA_SIZE_SMALL - 1),
                                     ~(AREA_SIZE_MEDIUM - 1),
                                     ~(AREA_SIZE_LARGE - 1),
                                     ~(AREA_SIZE_HUGE - 1),
                                     UINT64_MAX,
                                     UINT64_MAX,
                                     UINT64_MAX};

    const int8_t pidx = partition_from_addr(p);
    if (pidx < 0) {
        return NULL;
    }
    return (Area *)(p & masks[pidx]);
}

#endif // bitmaks_inl
