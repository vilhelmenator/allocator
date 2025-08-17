
#ifndef ARENA_H
#define ARENA_H
/*
    * Arena Memory Allocator
    *    * A memory allocator that manages memory in fixed-size chunks.
    *    * It supports allocation and deallocation of blocks within arenas.
    *    * The allocator uses atomic operations for thread safety.
    *    * It maintains metadata for active, in-use, and dirty blocks.
    *    * The allocator is designed to be efficient for large memory allocations.
    * 
*/
#include "callocator.inl"

// Arena structure definition
#define ARENA_BASE_SIZE_EXPONENT 22
#define ARENA_SIZE(x) (1ULL << (ARENA_BASE_SIZE_EXPONENT + x))
#define ARENA_SIZE_EXPONENT(x) (ARENA_BASE_SIZE_EXPONENT + x)
#define ARENA_CHUNK_SIZE_EXPONENT(x) ((ARENA_BASE_SIZE_EXPONENT + x) - 6)
#define ARENA_CHUNK_SIZE(x) (1ULL << ((ARENA_BASE_SIZE_EXPONENT + x) - 6))
static const uint32_t arena_level_offset = ARENA_BASE_SIZE_EXPONENT;

static inline uintptr_t reserve_range_idx(size_t range, size_t idx) { return ((1ULL << range) - 1ULL) << idx; }

static inline uint32_t delta_exp_to_idx(uintptr_t a, uintptr_t b, size_t exp)
{
    const ptrdiff_t diff = (uint8_t *)a - (uint8_t *)b;
    return (uint32_t)((size_t)diff >> exp);
}

static inline Arena* arena_get_header(uintptr_t addr)
{
    size_t asize = area_size_from_addr((uintptr_t)addr);
    uintptr_t base = ALIGN_DOWN_2((uintptr_t)addr, asize);
    uintptr_t header = base;
    return (Arena*)header;
}

static inline bool arena_is_connected(const Arena *s) { return s->prev != NULL || s->next != NULL; }
void arena_allocate_blocks(Allocator* alloc, Arena *a, int start_bit, int size_in_blocks);
void arena_free_blocks(Allocator* alloc, Arena *a, int start_bit);
void arena_unuse_blocks(Arena *a, int start_bit);
void arena_use_blocks(Arena *a, int start_bit);
void arena_set_dirty_blocks(Arena *a, int start_bit);
void arena_clear_dirty(Arena *a);
bool arena_free_active(Allocator* alloc, Arena *a, bool decommit);
bool arena_reallocate(Arena *a, int32_t start_idx, int32_t size_in_blocks, bool zero);
#endif // ARENA_H
