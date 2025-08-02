
#ifndef ARENA_H
#define ARENA_H

#include "callocator.inl"
#include <stdatomic.h>

#define ARENA_BASE_SIZE_EXPONENT 22
#define ARENA_SIZE(x) (1 << (ARENA_BASE_SIZE_EXPONENT + x))
#define ARENA_SIZE_EXPONENT(x) (ARENA_BASE_SIZE_EXPONENT + x)
#define ARENA_CHUNK_SIZE_EXPONENT(x) ((ARENA_BASE_SIZE_EXPONENT + x) - 6)
#define ARENA_CHUNK_SIZE(x) (1 << ((ARENA_BASE_SIZE_EXPONENT + x) - 6))
static const uint32_t arena_level_offset = ARENA_BASE_SIZE_EXPONENT;

typedef struct arena_size_table_t
{
    uint64_t exponents[2];
    uint64_t sizes[2];
} arena_size_table;
typedef struct arena_empty_mask_table_t
{
    uint64_t sizes[3];
} arena_empty_mask_table;


uintptr_t new_arena_get_mask_addr(Arena *h, size_t i, size_t j);
uintptr_t new_arena_get_data_addr(Arena *h, size_t i, size_t j, size_t k);

static inline uintptr_t reserve_range_idx(size_t range, size_t idx) { return ((1ULL << range) - 1ULL) << idx; }

static inline void arena_init_zero(uintptr_t baseptr)
{
    *(uint64_t *)baseptr = 1ULL;
    *(uint64_t *)(baseptr + sizeof(uint64_t)) = 0;
    *(uint64_t *)(baseptr + sizeof(uint64_t) * 2) = 0;
}

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
void arena_unuse_blocks(Allocator* alloc, Arena *a, int start_bit);
void arena_use_blocks(Allocator* alloc, Arena *a, int start_bit);
#endif // ARENA_H
