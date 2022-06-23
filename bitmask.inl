
#ifndef bitmask_inl
#define bitmask_inl

#include "callocator.inl"

static inline bool bitmask_is_set_hi(const Bitmask *bm, uint8_t bit) { return bm->_w32[1] & ((uint32_t)1 << bit); }
static inline bool bitmask_is_set_lo(const Bitmask *bm, uint8_t bit) { return bm->_w32[0] & ((uint32_t)1 << bit); }
static inline bool bitmask_is_full_hi(const Bitmask *bm) { return bm->_w32[1] == UINT32_MAX; }
static inline bool bitmask_is_full_lo(const Bitmask *bm) { return bm->_w32[0] == UINT32_MAX; }
static inline bool bitmask_is_empty_hi(const Bitmask *bm) { return bm->_w32[1] == 0; }
static inline bool bitmask_is_empty_lo(const Bitmask *bm) { return bm->_w32[0] == 0; }
static inline void bitmask_reserve_all(Bitmask *bm) { bm->whole = UINT64_MAX; }
static inline void bitmask_reserve_hi(Bitmask *bm, uint8_t bit) { bm->_w32[1] |= ((uint32_t)1 << bit); }
static inline void bitmask_reserve_lo(Bitmask *bm, uint8_t bit) { bm->_w32[0] |= ((uint32_t)1 << bit); }
static inline void bitmask_free_all(Bitmask *bm) { bm->whole = 0; }
static inline void bitmask_free_idx_hi(Bitmask *bm, uint8_t bit) { bm->_w32[1] &= ~((uint32_t)1 << bit); }
static inline void bitmask_free_idx_lo(Bitmask *bm, uint8_t bit) { bm->_w32[0] &= ~((uint32_t)1 << bit); }

static inline int8_t bitmask_first_free_hi(Bitmask *bm)
{
    const uint32_t m = ~bm->_w32[1];
    return __builtin_ctz(m);
}

static inline int8_t bitmask_first_free_lo(Bitmask *bm)
{
    const uint32_t m = ~bm->_w32[0];
    return __builtin_ctz(m);
}

static inline int8_t bitmask_allocate_bit_hi(Bitmask *bm)
{
    if (bitmask_is_full_hi(bm)) {
        return -1;
    }
    const int8_t fidx = bitmask_first_free_hi(bm);
    bitmask_reserve_hi(bm, fidx);
    return fidx;
}

static inline int8_t bitmask_allocate_bit_lo(Bitmask *bm)
{
    if (bitmask_is_full_lo(bm)) {
        return -1;
    }
    const int8_t fidx = bitmask_first_free_lo(bm);
    bitmask_reserve_lo(bm, fidx);
    return fidx;
}

#endif // bitmask_inl
