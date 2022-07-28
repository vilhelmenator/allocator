//
//  area.inl
//  MemPoolTests
//
//  Created by Vilhelm Sævarsson on 23.6.2022.
//  Copyright © 2022 Vilhelm Sævarsson. All rights reserved.
//
#include "os.h"
#include "area.h"
#include "../cthread/cthread.h"
int8_t area_get_section_count(Area *a)
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

static const uintptr_t _Area_small_area_mask = UINT8_MAX;
static const uintptr_t _Area_medium_area_mask = UINT16_MAX;
static const uintptr_t _Area_large_area_mask = UINT32_MAX;


bool area_is_full(const Area *a)
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
