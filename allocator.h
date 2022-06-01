#pragma once
#ifndef _alloc_h_
#define _alloc_h_

#include "../ctest/ctest.h"
#include "../cthread/cthread.h"
CLOGGER(_alloc_h_);

#if defined(_MSC_VER)
#define WINDOWS
#endif

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef WINDOWS
#include <intrin.h>
#include <memoryapi.h>
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#define WSIZE 4 /* Word size in bytes */
#define DSIZE 8 /* Double word size in bytes */
#define SZ_KB 1024ULL
#define SZ_MB (SZ_KB * SZ_KB)
#define SZ_GB (SZ_MB * SZ_KB)

#define MAX(x, y) ((x) > (y) ? (x) : (y))
#define MIN(x, y) ((x) < (y) ? (x) : (y))

#define DEFAULT_OS_PAGE_SIZE 4096ULL

#define SMALL_OBJECT_SIZE DEFAULT_OS_PAGE_SIZE * 4    // 16k
#define DEFAULT_PAGE_SIZE SMALL_OBJECT_SIZE * 8       // 128kb
#define MEDIUM_OBJECT_SIZE DEFAULT_PAGE_SIZE          // 128kb
#define DEFAULT_MID_PAGE_SIZE MEDIUM_OBJECT_SIZE * 4  // 512kb
#define LARGE_OBJECT_SIZE DEFAULT_MID_PAGE_SIZE * 4   // 2Mb
#define DEFAULT_LARGE_PAGE_SIZE LARGE_OBJECT_SIZE * 2 // 4Mb
#define HUGE_OBJECT_SIZE DEFAULT_LARGE_PAGE_SIZE * 8  // 32Mb

#define SECTION_SIZE (1ULL << 22ULL)

#define CACHE_LINE 64
#ifdef WINDOWS
#define cache_align __declspec(align(CACHE_LINE))
#else
#define cache_align __attribute__((aligned(CACHE_LINE)))
#endif

#define AREA_SIZE_SMALL (SECTION_SIZE * 8ULL)  // 32Mb
#define AREA_SIZE_LARGE (SECTION_SIZE * 32ULL) // 128Mb
#define AREA_SIZE_HUGE (SECTION_SIZE * 64ULL)  // 256Mb
#define MASK_FULL 0xFFFFFFFFFFFFFFFF

#define POOL_BIN_COUNT 17 * 8 + 1
#define HEADER_SIZE 64

#define MAX_SMALL_AREAS 192 // 32Mb over 6Gb
#define MAX_LARGE_AREAS 64  // 128Mb over 8Gb
#define MAX_HUGE_AREAS 64   // 256Mb over 16Gb

#define MAX_THREADS 1024
#define POWER_OF_TWO(x) ((x & (x - 1)) == 0)
#define ALIGN(x) ((MAX(x, 1) + sizeof(intptr_t) - 1) & ~(sizeof(intptr_t) - 1))

/*
static int countBits(uint32_t v)
{
    uint32_t c = 0;
    for (; v; c++) {
        v &= v - 1; // clear the least significant bit set
    }
    return c;
}
*/

static inline uintptr_t section_align_up(uintptr_t ptr)
{
    static const uintptr_t mask = SECTION_SIZE - 1;
    return (ptr + mask) & ~mask;
}

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

static cache_align const uintptr_t size_clss_to_exponent[] = {
    17, // 128k
    19, // 512k
    22, // 4Mb
    25, // 32Mb
    27, // 128Mb
    28  // 256Mb
};

typedef enum AreaType_t {
    AT_FIXED_32,  //  containes small allocations
    AT_FIXED_128, //  reserved for a particular partition, collecting object
                  //  between 4 - 32 megs.
    AT_FIXED_256, //  reserved for a particular partition, collecting object
                  //  between 32 - 128 megs.
    AT_VARIABLE,  //  for objects, where we want a single allocatino per item in
                  //  a dedicated partition.
} AreaType;

static cache_align const uintptr_t partitions_offsets[] = {
    ((uintptr_t)2 << 40), // allocations smaller than SECTION_MAX_MEMORY
    ((uintptr_t)4 << 40),
    ((uintptr_t)8 << 40),   // SECTION_MAX_MEMORY < x < AREA_MAX_MEMORY
    ((uintptr_t)16 << 40),  // AREA_MAX_MEMORY < x < 1GB
    ((uintptr_t)32 << 40),  // resource allocations.
    ((uintptr_t)64 << 40),  // Huge allocations
    ((uintptr_t)128 << 40), // end
};

static const uint8_t partition_count = 7;

static inline int8_t partition_from_addr(uintptr_t p)
{
    const int lz = 22 - __builtin_clzll(p);
    if (lz < 0 || lz > partition_count) {
        return -1;
    } else {
        return lz;
    }
}

static inline uint32_t partition_id_from_addr_and_partition(uintptr_t p, int8_t pidx)
{
    static const uintptr_t partitions_size[] = {
        // GIGABYTES
        ((uintptr_t)6 << 30), // the first two partitions are merged
        ((uintptr_t)6 << 30), //
        ((uintptr_t)8 << 30),  ((uintptr_t)16 << 30),  ((uintptr_t)32 << 30),
        ((uintptr_t)64 << 30), ((uintptr_t)128 << 30),
    };
    if (pidx < 0) {
        return -1;
    }
    const ptrdiff_t diff = (uint8_t *)p - (uint8_t *)partitions_offsets[pidx];
    return (uint32_t)(((size_t)diff) / partitions_size[pidx]);
}

static inline int16_t partition_id_from_addr(uintptr_t p)
{
    int8_t pidx = partition_from_addr(p);
    return partition_id_from_addr_and_partition(p, pidx);
}

typedef union Bitmask_u
{
    uint64_t whole;
    uint32_t _w32[2];
} Bitmask;

static inline bool bitmask_is_set_hi(Bitmask *bm, uint8_t bit) { return bm->_w32[1] & ((uint32_t)1 << bit); }
static inline bool bitmask_is_set_lo(Bitmask *bm, uint8_t bit) { return bm->_w32[0] & ((uint32_t)1 << bit); }

static inline bool bitmask_is_full_hi(Bitmask *bm) { return bm->_w32[1] == 0xFFFFFFFF; }
static inline bool bitmask_is_full_lo(Bitmask *bm) { return bm->_w32[0] == 0xFFFFFFFF; }
static inline bool bitmask_is_empty_hi(Bitmask *bm) { return bm->_w32[1] == 0; }

static inline bool bitmask_is_empty_lo(Bitmask *bm) { return bm->_w32[0] == 0; }

static inline void bitmask_reserve_all(Bitmask *bm) { bm->whole = 0xFFFFFFFFFFFFFFFF; }

static inline void bitmask_reserve_hi(Bitmask *bm, uint8_t bit) { bm->_w32[1] |= ((uint32_t)1 << bit); }
static inline void bitmask_reserve_lo(Bitmask *bm, uint8_t bit) { bm->_w32[0] |= ((uint32_t)1 << bit); }

static inline void bitmask_free_all(Bitmask *bm) { bm->whole = 0; }

static inline void bitmask_free_idx_hi(Bitmask *bm, uint8_t bit) { bm->_w32[1] &= ~((uint32_t)1 << bit); }
static inline void bitmask_free_idx_lo(Bitmask *bm, uint8_t bit) { bm->_w32[0] &= ~((uint32_t)1 << bit); }

static inline int8_t bitmask_first_free_hi(Bitmask *bm)
{
    uint32_t m = ~bm->_w32[1];
    return __builtin_ctz(m);
}

static inline int8_t bitmask_first_free_lo(Bitmask *bm)
{
    uint32_t m = ~bm->_w32[0];
    return __builtin_ctz(m);
}

static inline int8_t bitmask_allocate_bit_hi(Bitmask *bm)
{
    if (bitmask_is_full_hi(bm)) {
        return -1;
    }
    int8_t fidx = bitmask_first_free_hi(bm);
    bitmask_reserve_hi(bm, fidx);
    return fidx;
}

static inline int8_t bitmask_allocate_bit_lo(Bitmask *bm)
{
    if (bitmask_is_full_lo(bm)) {
        return -1;
    }
    int8_t fidx = bitmask_first_free_lo(bm);
    bitmask_reserve_lo(bm, fidx);
    return fidx;
}

static inline size_t get_os_page_size()
{
#ifdef WINDOWS
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize;
#else
    return sysconf(_SC_PAGESIZE);
#endif
}
static size_t os_page_size = 4096;

typedef struct spinlock
{
    _Atomic(bool) lock_;
} spinlock;

void spinlock_lock(spinlock *sl)
{
    for (;;) {
        // Optimistically assume the lock is free on the first try
        if (!atomic_exchange_explicit(&sl->lock_, true, memory_order_acquire)) {
            return;
        }
        // Wait for lock to be released without generating cache misses
        while (atomic_load_explicit(&sl->lock_, memory_order_relaxed)) {
            // Issue X86 PAUSE or ARM YIELD instruction to reduce contention
            // between hyper-threads
            __builtin_ia32_pause();
        }
    }
}

bool spinlock_try_lock(spinlock *sl)
{
    // First do a relaxed load to check if lock is free in order to prevent
    // unnecessary cache misses if someone does while(!try_lock())
    return !atomic_load_explicit(&sl->lock_, memory_order_relaxed) &&
           !atomic_exchange_explicit(&sl->lock_, true, memory_order_acquire);
}

void spinlock_unlock(spinlock *sl) { atomic_store_explicit(&sl->lock_, false, memory_order_release); }

typedef struct HeapBlock_t
{
    uint8_t *data;
} HeapBlock;
static inline uint32_t heap_block_get_header(HeapBlock *hb) { return *(uint32_t *)((uint8_t *)&hb->data - WSIZE); }

static inline uint32_t heap_block_get_footer(HeapBlock *hb)
{
    uint32_t size = *(uint32_t *)((uint8_t *)&hb->data - WSIZE) & ~0x7;
    return *(uint32_t *)((uint8_t *)&hb->data + (size)-DSIZE);
}

static inline uint32_t heap_block_get_Alloc(uint32_t v) { return v & 0x1; }

static inline uint32_t heap_block_get_Size(uint32_t v) { return v & ~0x7; }

static inline void heap_block_set_header(HeapBlock *hb, uint32_t s, uint32_t v)
{
    *(uint32_t *)((uint8_t *)&hb->data - WSIZE) = (s | v);
}

static inline void heap_block_set_footer(HeapBlock *hb, uint32_t s, uint32_t v)
{
    uint32_t size = (*(uint32_t *)((uint8_t *)&hb->data - WSIZE)) & ~0x7;
    *(uint32_t *)((uint8_t *)(&hb->data) + (size)-DSIZE) = (s | v);
}

static inline HeapBlock *heap_block_next(HeapBlock *hb)
{
    uint32_t size = *(uint32_t *)((uint8_t *)&hb->data - WSIZE) & ~0x7;
    return (HeapBlock *)((uint8_t *)&hb->data + (size));
}

static inline HeapBlock *heap_block_prev(HeapBlock *hb)
{
    uint32_t size = *(uint32_t *)((uint8_t *)&hb->data - DSIZE) & ~0x7;
    return (HeapBlock *)((uint8_t *)&hb->data - (size));
}

#define HEAP_NODE_OVERHEAD 8

typedef struct Block_t
{
    struct Block_t *next;
} Block;

typedef enum ExponentType_e { EXP_PUNY = 0, EXP_SMALL, EXP_MEDIUM, EXP_LARGE, EXP_HUGE, EXP_GIGANTIC } ExponentType;

typedef enum ContainerType_e {
    HEAP = 0,
    POOL,
    SLAB,
} ContainerType;

static inline int32_t get_container_exponent(size_t s, ContainerType t)
{
    if (t == HEAP) {
        if (s <= MEDIUM_OBJECT_SIZE) {      // 16k - 4Mb
            return EXP_LARGE;               // 32
        } else if (s <= HUGE_OBJECT_SIZE) { // 4Mb - 32Mb
            return EXP_HUGE;                // 128
        } else {                            // for large than 32Mb objects.
            return EXP_GIGANTIC;            // 256
        }
    } else {
        if (s <= SMALL_OBJECT_SIZE) {         // 8 - 16k
            return EXP_PUNY;                  // 128k
        } else if (s <= MEDIUM_OBJECT_SIZE) { // 16k - 128k
            return EXP_SMALL;                 // 512k
        } else {
            return EXP_MEDIUM; // 4M for > 128k objects.
        }
    }
}
typedef struct Queue_t
{
    void *head;
    void *tail;
} Queue;

typedef struct QNode_t
{
    void *prev;
    void *next;
} QNode;

// list functions
static inline bool list_isEmpty(Queue *q) { return q->head == NULL; }
static inline void _list_enqueue(Queue *tq, void *node, size_t prev_offset)
{
    QNode *tn = (QNode *)((uint8_t *)node + prev_offset);
    tn->next = tq->head;
    tn->prev = NULL;
    if (tq->head != NULL) {
        QNode *temp = (QNode *)((uint8_t *)tq->head + prev_offset);
        temp->prev = node;
        tq->head = node;
    } else {
        tq->tail = tq->head = node;
    }
}

static void _list_remove(Queue *tq, void *node, size_t prev_offset)
{
    QNode *tn = (QNode *)((uint8_t *)node + prev_offset);
    if (tn->prev != NULL) {
        QNode *temp = (QNode *)((uint8_t *)tn->prev + prev_offset);
        temp->next = tn->next;
    }
    if (tn->next != NULL) {
        QNode *temp = (QNode *)((uint8_t *)tn->next + prev_offset);
        temp->prev = tn->prev;
    }
    if (node == tq->head) {
        tq->head = tn->next;
    }

    if (node == tq->tail) {
        tq->tail = tn->prev;
    }
    tn->next = NULL;
    tn->prev = NULL;
}
/*
static void _list_insert_at(void *queue, void *target, void *node, size_t head_offset, size_t prev_offset)
{
    Queue *tq = (Queue *)((uint8_t *)queue + head_offset);
    QNode *tn = (QNode *)((uint8_t *)node + prev_offset);
    QNode *ttn = (QNode *)((uint8_t *)target + prev_offset);
    if (tq->tail == NULL) {
        tq->tail = tq->head = node;
        return;
    }
    tn->next = ttn->next;
    tn->prev = target;
    if (tn->next != NULL) {
        QNode *temp = (QNode *)((uint8_t *)tn->next + prev_offset);
        temp->prev = node;
    } else {
        tq->tail = node;
    }
    ttn->next = node;
}

static void _list_insert_sort(void *queue, void *node, size_t head_offset, size_t prev_offset)
{
    Queue *tq = (Queue *)((uint8_t *)queue + head_offset);
    QNode *tn = (QNode *)((uint8_t *)node + prev_offset);
    if (tq->tail == NULL) {
        tq->tail = tq->head = node;
        return;
    }

    if (tq->tail < node) {
        QNode *temp = (QNode *)((uint8_t *)tq->tail + prev_offset);
        temp->next = node;
        tn->next = NULL;
        tn->prev = tq->tail;
        tq->tail = node;
        return;
    }

    QNode *current = (QNode *)((uint8_t *)tq->head + prev_offset);
    while (current->next != NULL && current->next < node) {
        current = (QNode *)((QNode *)((uint8_t *)current->next + prev_offset))->next;
    }
    tn->prev = current->prev;
    tn->next = current->next;
    current->prev = node;
}
*/
#define list_enqueue(q, n) _list_enqueue(q, n, offsetof(__typeof__(*n), prev))
#define list_remove(q, n) _list_remove(q, n, offsetof(__typeof__(*n), prev))
/*
#define list_insert_at(q, t, n) _list_insert_at(q, t, n, offsetof(__typeof__(*q), head), offsetof(__typeof__(*n), prev))
#define list_insert_sort(q, t, n)                                                                                      \
    _list_insert_sort(q, n, offsetof(__typeof__(*q), head), offsetof(__typeof__(*n), prev))
*/
static const uintptr_t _Area_small_area_mask = 0xff;
static const uintptr_t _Area_large_area_mask = 0xffffffff;
static const uintptr_t _Area_ptr_mask = 0x0000ffffffffffff;
static const uintptr_t _Area_inv_ptr_mask = 0xffff000000000000;

typedef struct Area_t
{
    int64_t partition_id;
    Bitmask constr_mask; // containers that have been constructed.
    Bitmask active_mask; // containers that can be destructed.

    // these members are shared with the first section in the memory block. so,
    // the first high 16 bits are reserved by the section.
    size_t size;
    struct Area_t *prev;
    struct Area_t *next;
} Area;

typedef struct AreaList_t
{
    Area *head;
    Area *tail;
    uint32_t partition_id;
    uintptr_t start_addr;
    uintptr_t end_addr;
    AreaType type;
    size_t area_count;
    Area *previous_area;
} AreaList;

static inline Area *area_get_prev(Area *a)
{
    return (Area *)((uintptr_t)a->prev & _Area_ptr_mask);
} // remove the top 16 bits
static inline Area *area_get_next(Area *a)
{
    return (Area *)((uintptr_t)a->next & _Area_ptr_mask);
} // remove the top 16 bits

static inline void area_set_prev(Area *a, Area *p)
{
    a->prev = (Area *)(((_Area_inv_ptr_mask & (uintptr_t)a->prev) | (uintptr_t)p));
}
static inline void area_set_next(Area *a, Area *n)
{
    a->next = (Area *)(((_Area_inv_ptr_mask & (uintptr_t)a->next) | (uintptr_t)n));
}

static inline void area_list_remove(AreaList *q, Area *a)
{
    if (area_get_prev(a) != NULL)
        area_set_next(area_get_prev(a), area_get_next(a));
    if (area_get_next(a) != NULL)
        area_set_prev(area_get_next(a), area_get_prev(a));
    if (a == q->head)
        q->head = area_get_next(a);
    if (a == q->tail)
        q->tail = area_get_prev(a);
    area_set_next(a, NULL);
    area_set_prev(a, NULL);
}
static inline void area_list_insert_at(AreaList *q, Area *t, Area *a)
{
    if (q->tail == NULL) {
        q->tail = q->head = a;
        return;
    }

    area_set_next(a, area_get_next(t));
    area_set_prev(a, t);
    if (area_get_next(t) != NULL) {
        area_set_prev(area_get_next(t), a);
    } else {
        q->tail = a;
    }
    area_set_next(t, a);
}
static inline bool area_is_empty(Area *a) { return bitmask_is_empty_hi(&a->active_mask); }
static inline bool area_is_free(Area *a) { return area_is_empty(a); }
static inline bool area_is_claimed(Area *a, uint8_t idx) { return bitmask_is_set_hi(&a->constr_mask, idx); }

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
    size_t area_size = a->size;
    if (area_size == AREA_SIZE_SMALL) {
        return 8;
    } else if (area_size == AREA_SIZE_LARGE) {
        return 32;
    } else {
        return 1;
    }
}

static inline int8_t area_claim_section(Area *a) { return bitmask_allocate_bit_hi(&a->constr_mask); }

static inline void area_claim_all(Area *a) { bitmask_reserve_all(&a->constr_mask); }

static inline void area_claim_idx(Area *a, uint8_t idx) { bitmask_reserve_hi(&a->constr_mask, idx); }

static bool area_is_full(Area *a)
{
    if (bitmask_is_full_hi(&a->active_mask)) {
        return true;
    }
    if (a->size == AREA_SIZE_SMALL) {
        return ((a->active_mask.whole >> 32) & _Area_small_area_mask) == _Area_small_area_mask;
    } else {
        return ((a->active_mask.whole >> 32) & _Area_large_area_mask) == _Area_large_area_mask;
    }
}

static inline size_t area_get_size(Area *a) { return a->size; }
static inline void area_set_size(Area *a, size_t s) { a->size = s; }

static inline Area *area_from_addr(uintptr_t p)
{
    static const uint64_t masks[] = {~(AREA_SIZE_SMALL - 1), ~(AREA_SIZE_SMALL - 1), ~(AREA_SIZE_LARGE - 1),
                                     ~(AREA_SIZE_HUGE - 1),  0xffffffffffffffff,     0xffffffffffffffff,
                                     0xffffffffffffffff};

    int8_t pidx = partition_from_addr(p);
    if (pidx < 0) {
        return NULL;
    }
    return (Area *)(p & masks[pidx]);
}

typedef struct Section_t
{
    int64_t partition_id;
    // 24 bytes as the section header
    Bitmask constr_mask; // 32 pages bit per page.   // lower 32 bits per
                         // section/ high
    Bitmask active_mask;

    // An area and section can overlap, and the prev next pointer of an area
    // will always be under the 32tb range. top 16 bits area always zero.
    size_t asize;              // lower 32 bits per section/ high bits are for area
    size_t container_type;     // top 16 bits.
    size_t container_exponent; // top 16 bits.

    int32_t idx; // index in parent area.

    // links to sections.
    struct Section_t *prev;
    struct Section_t *next;

    uint8_t collections[1];
} Section;

static inline bool section_is_connected(Section *s) { return s->prev != NULL || s->next != NULL; }
static inline ContainerType section_get_container_type(Section *s)
{
    return (ContainerType)((uint16_t)((s->container_type & 0xffff000000000000) >> 48));
}
static inline ExponentType section_get_container_exponent(Section *s)
{
    return (ExponentType)(uint16_t)((s->container_exponent & 0xffff000000000000) >> 48);
}
static uint8_t section_get_collection_count(Section *s)
{
    if (section_get_container_type(s) != POOL) {
        return 1;
    }
    switch (section_get_container_exponent(s)) {
    case EXP_PUNY: {
        return 32;
    }
    case EXP_SMALL: {
        return 8;
    }
    default: {
        return 1;
    }
    }
}
static inline void section_free_idx_pool(Section *s, uint8_t i)
{
    bitmask_free_idx_lo(&s->active_mask, i);
    bool section_empty = bitmask_is_empty_lo(&s->active_mask);
    if (section_empty) {
        Area *area = area_from_addr((uintptr_t)s);
        area_free_idx(area, s->idx);
    }
}
static inline void section_free_idx(Section *s, uint8_t i)
{
    bitmask_free_idx_lo(&s->active_mask, i);
    bool section_empty = bitmask_is_empty_lo(&s->active_mask);
    if (section_empty) {
        // what partition are we in.
        Area *area = area_from_addr((uintptr_t)s);
        switch (section_get_container_type(s)) {
        case POOL: {
            area_free_idx(area, s->idx);
            break;
        }
        default: // SLAB
            area_free_all(area);
            break;
        }
    }
}
static inline bool section_is_claimed(Section *s, uint8_t idx) { return bitmask_is_set_lo(&s->constr_mask, idx); }
static inline void section_reserve_idx(Section *s, uint8_t i)
{
    bitmask_reserve_lo(&s->active_mask, i);
    Area *area = area_from_addr((uintptr_t)s);
    area_reserve_idx(area, s->idx);
}

static inline void section_claim_idx(Section *s, uint8_t i)
{
    bitmask_reserve_lo(&s->constr_mask, i);
    Area *area = area_from_addr((uintptr_t)s);
    area_claim_idx(area, s->idx);
}

static inline void section_claim_all(Section *s)
{
    bitmask_reserve_all(&s->constr_mask);
    Area *area = area_from_addr((uintptr_t)s);
    area_claim_idx(area, s->idx);
}

static inline uint8_t section_reserve_next(Section *s) { return bitmask_allocate_bit_lo(&s->active_mask); }

static inline void section_reserve_all(Section *s)
{
    section_claim_all(s);
    Area *area = area_from_addr((uintptr_t)s);
    area_reserve_idx(area, s->idx);
    bitmask_reserve_all(&s->active_mask);
}

static inline void section_free_all(Section *s)
{
    bitmask_free_all(&s->active_mask);
    Area *area = area_from_addr((uintptr_t)s);
    area_free_idx(area, s->idx);
}

static inline void section_set_container_type(Section *s, ContainerType pt)
{
    s->container_type = (s->container_type & 0x0000ffffffffffff) | ((uint64_t)pt << 48);
}
static inline void section_set_container_exponent(Section *s, uint16_t prt)
{
    s->container_exponent = (s->container_exponent & 0x0000ffffffffffff) | ((uint64_t)prt << 48);
}
static inline size_t section_get_size(Section *s) { return s->asize; }

static inline bool section_is_full(Section *s)
{
    switch (section_get_container_exponent(s)) {
    case EXP_PUNY: {
        return (s->active_mask.whole & 0xffffffff) == 0xffffffff;
    }
    case EXP_SMALL: {
        return (s->active_mask.whole & 0xff) == 0xff;
    }
    default: {
        return (s->active_mask.whole & 0x1) == 0x1;
    }
    }
}

static inline void *section_find_collection(Section *s, void *p)
{
    const ptrdiff_t diff = (uint8_t *)p - (uint8_t *)&s->collections[0];
    const uintptr_t exp = size_clss_to_exponent[(s->container_exponent & 0xffff000000000000) >> 48];
    const int32_t collection_size = 1 << exp;
    const int32_t idx = (int32_t)((size_t)diff >> exp);
    return (void *)((uint8_t *)&s->collections[0] + collection_size * idx);
}

static inline uintptr_t section_get_collection(Section *s, int8_t idx, ExponentType exp)
{
    return (uintptr_t)((uint8_t *)&s->collections[0] + (1 << size_clss_to_exponent[exp]) * idx);
}
static const cache_align int32_t pool_sizes[] = {
    8,       16,      24,      32,      40,      48,      56,      64,      72,      80,      88,      96,      104,
    112,     120,     128,     144,     160,     176,     192,     208,     224,     240,     256,     288,     320,
    352,     384,     416,     448,     480,     512,     576,     640,     704,     768,     832,     896,     960,
    1024,    1152,    1280,    1408,    1536,    1664,    1792,    1920,    2048,    2304,    2560,    2816,    3072,
    3328,    3584,    3840,    4096,    4608,    5120,    5632,    6144,    6656,    7168,    7680,    8192,    9216,
    10240,   11264,   12288,   13312,   14336,   15360,   16384,   18432,   20480,   22528,   24576,   26624,   28672,
    30720,   32768,   36864,   40960,   45056,   49152,   53248,   57344,   61440,   65536,   73728,   81920,   90112,
    98304,   106496,  114688,  122880,  131072,  147456,  163840,  180224,  196608,  212992,  229376,  245760,  262144,
    294912,  327680,  360448,  393216,  425984,  458752,  491520,  524288,  589824,  655360,  720896,  786432,  851968,
    917504,  983040,  1048576, 1179648, 1310720, 1441792, 1572864, 1703936, 1835008, 1966080, 2097152, 2359296, 2621440,
    2883584, 3145728, 3407872, 3670016, 3932160, 4194304, 4718592};

typedef struct Pool_t
{
    int32_t idx;
    uint32_t block_idx;
    uint32_t block_size;
    int32_t num_available;
    int32_t num_committed;
    int32_t num_used;
    uint32_t extend_incr;
    Block *free;
    struct Pool_t *prev;
    struct Pool_t *next;
    uint8_t blocks[1];
} Pool;

static inline bool pool_owns_addr(const Pool *p, const void *addr)
{
    const uintptr_t start = (uintptr_t)&p->blocks[0];
    const uintptr_t end = (uintptr_t)((uint8_t *)start + (p->num_available * p->block_size));
    return (uintptr_t)addr >= start && (uintptr_t)addr <= end;
}

static inline bool pool_is_empty(const Pool *p) { return p->num_used == 0; }
static inline bool pool_is_full(const Pool *p) { return p->num_used >= p->num_available; }
static inline bool pool_is_almost_full(const Pool *p) { return p->num_used >= (p->num_available - 1); }
static inline bool pool_is_fully_commited(const Pool *p) { return p->num_committed >= p->num_available; }
static inline bool pool_is_connected(const Pool *p) { return p->prev != NULL || p->next != NULL; }

static inline void pool_free_block(Pool *p, const void *block)
{
    Block *new_free = (Block *)block;
    new_free->next = p->free;
    p->free = new_free;
    if (--p->num_used == 0) {
        Section *section = (Section *)((uintptr_t)p & ~(SECTION_SIZE - 1));
        section_free_idx_pool(section, p->idx);
    }
}

static void *pool_extend(Pool *p)
{
    if (p->extend_incr == 1) {
        Block *next_free = (Block *)((uint8_t *)&p->blocks[0] + (p->num_committed * p->block_size));
        p->num_used++;
        p->num_committed++;
        return next_free;
    } else {
        Block *next_free = (Block *)((uint8_t *)&p->blocks[0] + (p->num_committed * p->block_size));
        const uint32_t remaining = (p->num_available - p->num_committed);
        const uint32_t steps = MIN(remaining, p->extend_incr);
        p->num_committed += steps;

        uint64_t *block = (uint64_t *)next_free;
        for (uint32_t i = 1; i < steps; ++i) {
            *block = ((uintptr_t)block + p->block_size);
            block = (uint64_t *)*block;
        }
        *block = 0;
        p->num_used++;
        p->free = next_free->next;
        return next_free;
    }
}

static void pool_init(Pool *p, const int8_t pidx, const uint32_t block_idx, const uint32_t block_size,
                      const ExponentType partition)
{
    // where is the end of the pool.
    const size_t size_recip = (1 << 31) / block_size;
    const int32_t psize = 1 << size_clss_to_exponent[partition];
    const size_t block_memory = psize - sizeof(Pool) - 8;

    const uintptr_t section_end = ((uintptr_t)p + (SECTION_SIZE - 1)) & ~(SECTION_SIZE - 1);
    const size_t remaining_size = section_end - (uintptr_t)&p->blocks[0];
    const uintptr_t page_end = ((uintptr_t)&p->blocks[0] + (os_page_size - 1)) & ~(os_page_size - 1);
    const size_t steps = MAX(((page_end - (uintptr_t)&p->blocks[0]) * size_recip) >> 31, 1);

    p->idx = pidx;
    p->block_idx = block_idx;
    p->block_size = block_size;
    p->extend_incr = block_size < os_page_size ? (uint32_t)((os_page_size * size_recip) >> 31) : 1;
    p->num_available = (int32_t)((MIN(remaining_size, block_memory) * size_recip) >> 31);
    p->num_committed = (int32_t)steps;

    p->num_used = 0;
    p->next = NULL;
    p->prev = NULL;

    p->free = (Block *)&p->blocks[0];
    uint64_t *block = (uint64_t *)&p->blocks[0];

    for (uint32_t i = 1; i < steps; ++i) {
        *block = ((uintptr_t)block + p->block_size);
        block = (uint64_t *)*block;
    }
    *block = 0;
}

static inline Block *pool_get_free_block(Pool *p)
{
    if (p->num_used++ == 0) {
        Section *section = (Section *)((uintptr_t)p & ~(SECTION_SIZE - 1));
        section_reserve_idx(section, p->idx);
    }
    Block *res = p->free;
    p->free = res->next;
    return res;
}

static inline Block *pool_get_free_block_compact(Pool *p)
{
    p->num_used++;
    Block *res = p->free;
    p->free = res->next;
    return res;
}

static inline void *pool_aquire_block(Pool *p)
{
    if (p->free != NULL) {
        return pool_get_free_block(p);
    }

    if (!pool_is_fully_commited(p)) {
        return pool_extend(p);
    }

    return NULL;
}

typedef struct Heap_t
{
    //
    int32_t idx;
    uint32_t total_memory; // how much do we have available in total
    uint32_t used_memory;  // how much have we used
    uint32_t min_block;    // what is the minum size block available;
    uint32_t max_block;    // what is the maximum size block available;
    uint32_t num_allocations;

    uint8_t *start;
    Queue free_nodes;

    struct Heap_t *prev;
    struct Heap_t *next;

    uint8_t blocks[1];
} Heap;

static inline bool heap_is_connected(Heap *h) { return h->prev != NULL || h->next != NULL; }

static inline bool heap_has_room(Heap *h, size_t s)
{
    if ((h->used_memory + s + HEAP_NODE_OVERHEAD) > h->total_memory) {
        return false;
    }
    if (s <= h->max_block && s >= h->min_block) {
        return true;
    }
    return false;
}

static inline void heap_place(Heap *h, void *bp, uint32_t asize)
{
    HeapBlock *hb = (HeapBlock *)bp;
    int32_t csize = heap_block_get_header(hb) & ~0x7;
    if ((csize - asize) >= (DSIZE + HEAP_NODE_OVERHEAD)) {
        heap_block_set_header(hb, asize, 1);
        heap_block_set_footer(hb, asize, 1);
        hb = heap_block_next(hb);
        heap_block_set_header(hb, csize - asize, 0);
        heap_block_set_footer(hb, csize - asize, 0);
        list_enqueue(&h->free_nodes, (QNode *)hb);
    } else {
        heap_block_set_header(hb, csize, 1);
        heap_block_set_footer(hb, csize, 1);
    }
}

static inline void *heap_find_fit(Heap *h, uint32_t asize)
{
    QNode *current = (QNode *)h->free_nodes.head;
    while (current != NULL) {
        HeapBlock *hb = (HeapBlock *)current;
        int header = heap_block_get_header(hb);
        uint32_t bsize = header & ~0x7;
        if (asize <= bsize) {
            list_remove(&h->free_nodes, current);
            heap_place(h, current, asize);
            return current;
        }
        current = (QNode *)current->next;
    }
    return NULL;
}

static inline void *heap_get_block(Heap *h, uint32_t s)
{
    if (s <= DSIZE * 2) {
        s = DSIZE * 2 + HEAP_NODE_OVERHEAD;
    } else {
        s = DSIZE * ((s + HEAP_NODE_OVERHEAD + DSIZE - 1) / DSIZE);
    }
    void *ptr = heap_find_fit(h, s);

    h->used_memory += s;
    if (h->num_allocations == 0) {
        Section *section = (Section *)((uintptr_t)h & ~(SECTION_SIZE - 1));
        section_reserve_all(section);
    }
    h->num_allocations++;
    return ptr;
}

static inline void *heap_coalesce(Heap *h, void *bp)
{
    HeapBlock *hb = (HeapBlock *)bp;
    int32_t size = heap_block_get_header(hb) & ~0x7;
    HeapBlock *prev_block = heap_block_prev(hb);
    HeapBlock *next_block = heap_block_next(hb);
    int prev_header = heap_block_get_header(prev_block);
    int next_header = heap_block_get_header(next_block);

    size_t prev_alloc = prev_header & 0x1;
    size_t next_alloc = next_header & 0x1;

    QNode *hn = (QNode *)bp;
    if (!(prev_alloc && next_alloc)) {
        size_t prev_size = prev_header & ~0x7;
        size_t next_size = next_header & ~0x7;

        // next is free
        if (prev_alloc && !next_alloc) {
            size += next_size;
            heap_block_set_header(hb, size, 0);
            heap_block_set_footer(hb, size, 0);
            QNode *h_next = (QNode *)next_block;
            list_remove(&h->free_nodes, h_next);
            list_enqueue(&h->free_nodes, hn);
        } // prev is fre
        else if (!prev_alloc && next_alloc) {
            size += prev_size;
            heap_block_set_footer(hb, size, 0);
            heap_block_set_header(prev_block, size, 0);
            bp = (void *)heap_block_prev(hb);
        } else { // both next and prev are free
            size += prev_size + next_size;
            heap_block_set_header(prev_block, size, 0);
            heap_block_set_footer(next_block, size, 0);
            bp = (void *)heap_block_prev(hb);
            QNode *h_next = (QNode *)next_block;
            list_remove(&h->free_nodes, h_next);
        }
    } else {
        list_enqueue(&h->free_nodes, hn);
    }

    return bp;
}

static inline void heap_reset(Heap *h)
{
    h->free_nodes.head = NULL;
    h->free_nodes.tail = NULL;
    HeapBlock *hb = (HeapBlock *)h->start;
    list_enqueue(&h->free_nodes, (QNode *)h->start);
    heap_block_set_header(hb, h->total_memory, 0);
    heap_block_set_footer(hb, h->total_memory, 0);
    heap_block_set_header(heap_block_next(hb), 0, 1);
}

static void heap_free(Heap *h, void *bp, bool should_coalesce)
{
    if (bp == 0)
        return;

    HeapBlock *hb = (HeapBlock *)bp;
    uint32_t size = heap_block_get_header(hb) & ~0x7;
    heap_block_set_header(hb, size, 0);
    heap_block_set_footer(hb, size, 0);

    if (should_coalesce) {
        heap_coalesce(h, bp);
    } else {
        list_enqueue(&h->free_nodes, (QNode *)bp);
    }
    h->used_memory -= size;
    h->num_allocations--;
    if (h->num_allocations == 0) {
        Section *section = (Section *)((uintptr_t)h & ~(SECTION_SIZE - 1));
        section_free_all(section);
        heap_reset(h);
    }
}

static inline void heap_extend(Heap *h)
{
    *h->start = 0;
    *(h->start + WSIZE) = DSIZE | 1;   /* Prologue header */
    *(h->start + DSIZE) = (DSIZE | 1); /* Prologue footer */
    *(h->start + WSIZE + DSIZE) = 1;   /* Epilogue header */
    h->start = h->start + DSIZE * 2;

    heap_reset(h);
}

static void heap_init(Heap *h, int8_t pidx, ExponentType partition)
{
    size_t psize = 1 << size_clss_to_exponent[partition];
    uintptr_t section_end = align_up((uintptr_t)h, psize);
    size_t remaining_size = section_end - (uintptr_t)&h->blocks[0];

    size_t block_memory = psize - sizeof(Heap) - sizeof(Section);
    size_t header_footer_offset = sizeof(uintptr_t) * 2;
    h->idx = pidx;
    h->used_memory = 0;
    h->total_memory = (uint32_t)((MIN(remaining_size, block_memory)) - header_footer_offset - HEAP_NODE_OVERHEAD);
    h->max_block = h->total_memory;
    h->min_block = sizeof(uintptr_t);
    h->num_allocations = 0;
    h->start = &h->blocks[0];
    h->next = NULL;
    h->prev = NULL;
    heap_extend(h);
}

// lockless message queue
typedef struct message_t
{
    _Atomic(uintptr_t) next;
} message;

typedef struct message_queue_t
{
    _Atomic(uintptr_t) head;
    _Atomic(uintptr_t) tail;
} message_queue;

static cache_align message message_sentinals[MAX_THREADS];
static cache_align message_queue message_queues[MAX_THREADS];
static cache_align int64_t partition_owners[MAX_THREADS];

mutex_t partition_mutex;
static inline int8_t reserve_any_partition_set()
{
    mutex_lock(&partition_mutex);
    int8_t reserved_id = -1;
    for (int i = 0; i < 1024; i++) {
        if (partition_owners[i] == -1) {
            partition_owners[i] = i;
            reserved_id = i;
            break;
        }
    }
    mutex_unlock(&partition_mutex);
    return reserved_id;
}
static inline int8_t reserve_any_partition_set_for(int8_t midx)
{
    mutex_lock(&partition_mutex);
    int8_t reserved_id = -1;
    for (int i = 0; i < 1024; i++) {
        if (partition_owners[i] == -1) {
            partition_owners[i] = midx;
            reserved_id = i;
            break;
        }
    }
    mutex_unlock(&partition_mutex);
    return reserved_id;
}
static inline bool reserve_partition_set(int8_t idx, int8_t midx)
{
    mutex_lock(&partition_mutex);
    if (partition_owners[idx] == -1) {
        partition_owners[idx] = midx;
        return true;
    }
    mutex_unlock(&partition_mutex);
    return false;
}

static inline void release_partition_set(int8_t idx)
{
    if (idx >= 0) {
        mutex_lock(&partition_mutex);
        partition_owners[idx] = -1;
        mutex_unlock(&partition_mutex);
    }
}

static inline bool commit_memory(void *base, size_t size)
{
#ifdef WINDOWS
    return VirtualAlloc(base, size, MEM_COMMIT, PAGE_READWRITE) == base;
#else
    return (mprotect(base, size, (PROT_READ | PROT_WRITE)) == 0);
#endif
}

static inline bool decommit_memory(void *base, size_t size)
{
#ifdef WINDOWS
    return VirtualFree(base, size, MEM_DECOMMIT);
#else
    return (mmap(base, size, PROT_NONE, (MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE), -1, 0) == base);
#endif
}

static inline bool free_memory(void *ptr, size_t size)
{
#ifdef WINDOWS
    return VirtualFree(ptr, 0, MEM_RELEASE) == 0;
#else
    return (munmap(ptr, size) == -1);
#endif
}

static inline bool release_memory(void *ptr, size_t size, bool commit)
{
    if (commit) {
        return decommit_memory(ptr, size);
    } else {
        return free_memory(ptr, size);
    }
}

static inline void *alloc_memory(void *base, size_t size, bool commit)
{
#ifdef WINDOWS
    int flags = commit ? MEM_RESERVE | MEM_COMMIT : MEM_RESERVE;
    return VirtualAlloc(base, size, flags, PAGE_READWRITE);
#else
    int flags = commit ? (PROT_WRITE | PROT_READ) : PROT_NONE;
    return mmap(base, size, flags, (MAP_PRIVATE | MAP_ANONYMOUS), -1, 0);
#endif
}

static inline bool reset_memory(void *base, size_t size)
{
#ifdef WINDOWS
    int flags = MEM_RESET;
    void *p = VirtualAlloc(base, size, flags, PAGE_READWRITE);
    if (p == base && base != NULL) {
        VirtualUnlock(base, size); // VirtualUnlock after MEM_RESET removes the
                                   // memory from the working set
    }
    if (p != base)
        return false;
#else
    int err;
    size_t advice = MADV_FREE;
    int oadvice = (int)advice;
    while ((err = madvise(base, size, oadvice)) != 0 && errno == EAGAIN) {
        errno = 0;
    };
    if (err != 0 && errno == EINVAL && oadvice == MADV_FREE) {
        err = madvise(base, size, MADV_DONTNEED);
    }
    if (err != 0)
        return false;
#endif
    return true;
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
// yes, for the rare occation an issue arises, we just lock the mutex to avoid
// thread clashing issues on windows
mutex_t aligned_alloc_mutex;
static void *alloc_memory_aligned(void *base, size_t size, size_t alignment, bool commit, uintptr_t end)
{
    // alignment is smaller than a page size or not a power of two.
    if (!(alignment >= os_page_size && POWER_OF_TWO(alignment)))
        return NULL;
    size = align_up(size, os_page_size);
    if (size >= (SIZE_MAX - alignment))
        return NULL;

    mutex_lock(&aligned_alloc_mutex);
    void *ptr = alloc_memory(base, size, commit);
    if (ptr == NULL) {
        goto err;
    }

    if (!safe_to_aquire(base, ptr, size, end)) {
        free_memory(ptr, size);
        goto err;
    }

    if (((uintptr_t)ptr % alignment != 0)) {
        // this should happen very rarely, if at all.
        // release our failed attempt.
        free_memory(ptr, size);

        // Now we attempt to overallocate
        size_t adj_size = size + alignment;
        ptr = alloc_memory(base, adj_size, commit);
        if (ptr == NULL) {
            goto err;
        }

        // if the new ptr is not in our current partition set
        if (!safe_to_aquire(base, ptr, adj_size, end)) {
            free_memory(ptr, adj_size);
            goto err;
        }

        // if we got our aligned memory
        if (((uintptr_t)ptr % alignment) == 0) {
            // drop our excess request
            decommit_memory((uint8_t *)ptr + size, alignment);
            goto success;
        }
        // we are still not aligned, but we have an address that is aligned.
        free_memory(ptr, adj_size);
        //
        void *aligned_p = (void *)align_up((uintptr_t)ptr, alignment);
        // get our aligned address
        ptr = alloc_memory(aligned_p, size, commit);
        if (ptr == NULL) {
            // Why would this fail?
            goto err;
        }
        if (!safe_to_aquire(base, ptr, size, end)) {
            free_memory(ptr, size);
            goto err;
        }

        // if the system fails to get memory from the part we just released.
        // there is some other allocator screwing with our assumptions... so
        // FAIL!
        if (((uintptr_t)ptr % alignment) != 0) {
            free_memory(ptr, size);
            goto err;
        }
    }
success:
    mutex_unlock(&aligned_alloc_mutex);
    return ptr;
err:
    mutex_unlock(&aligned_alloc_mutex);
    return NULL;
}

static cache_align Queue pool_queues[MAX_THREADS][POOL_BIN_COUNT];
static cache_align Queue heap_queues[MAX_THREADS][3];
static cache_align Queue section_queues[MAX_THREADS];
static inline uint8_t sizeToPool(const size_t as)
{
    static const int bmask = ~0x7f;
    if ((bmask & as) == 0) {
        // the first 2 rows
        return (as >> 3) - 1;
    } else {
        const uint32_t top_mask = 0xffffffff;
        const int tz = __builtin_clz((uint32_t)as);
        const uint64_t bottom_mask = (top_mask >> (tz + 4));
        const uint64_t incr = (bottom_mask & as) > 0;
        const size_t row = (26 - tz) << 3;
        return (row + ((as >> (28 - tz)) & 0x7)) + incr - 1;
    }
}

static inline uint8_t sizeToPage(const size_t as)
{
    if (as <= MEDIUM_OBJECT_SIZE) {
        return 0; // 32mb pages
    } else if (as <= AREA_SIZE_SMALL) {
        return 1; // 128Mb pages
    } else {
        return 2; // 256Mb pages
    }
}

typedef void (*free_func)(void *);
typedef struct PartitionAllocator_t
{
    AreaList area_01;
    AreaList area_2;
    AreaList area_3;

    // sections local to this thread with free pages or pools
    Queue *sections;
    // free pages that have room for various size allocations.
    Queue *heaps;
    // free pools of various sizes.
    Queue *pools;

    // how man pages in total have been allocated.
    uint32_t heap_count;
    // how many pools in total have been allocated.
    uint32_t pool_count;

    // collection of messages for other threads
    message *thread_messages;
    uint32_t message_count; // how many threaded message have we acccumuated for passing out
    // a queue of messages from other threads.
    message_queue *thread_free_queue;

} PartitionAllocator;

static Area *partition_allocator_alloc_area(AreaList *area_queue, uint64_t area_size, uint64_t alignment);
static message *partition_allocator_get_last_message(PartitionAllocator *pa)
{
    message *msg = pa->thread_messages;
    if (msg == NULL) {
        return NULL;
    }
    while ((uintptr_t)msg->next != 0) {
        msg = (message *)(uintptr_t)msg->next;
    }
    return msg;
}

static void partition_allocator_thread_free(PartitionAllocator *pa, void *p)
{
    message *new_free = (message *)p;
    new_free->next = (uintptr_t)pa->thread_messages;
    pa->thread_messages = new_free;
    pa->message_count++;
}

static Area *partition_allocator_get_next_area(AreaList *area_queue, uint64_t size, uint64_t alignment)
{

    void *aligned_addr = NULL;
    Area *insert = NULL;
    uintptr_t asize = align_up(size, alignment);
    uint64_t delta = (uint64_t)((uint8_t *)area_queue->end_addr - area_queue->start_addr);
    if (area_queue->head == NULL) {
        aligned_addr = (void *)area_queue->start_addr;
    } else {
        // is there room at the end
        //
        uint64_t tail_end = (uint64_t)((uint8_t *)area_queue->tail + area_get_size(area_queue->tail));
        delta = (uint64_t)((uint8_t *)area_queue->end_addr - tail_end);
        if (delta < size && (area_queue->tail != area_queue->head)) {
            Area *current = area_queue->head;
            while (current != area_queue->tail) {
                Area *next = area_get_next(current);
                size_t c_size = area_get_size(current);
                size_t c_end = c_size + (size_t)(uint8_t *)current;
                delta = (uint64_t)((uint8_t *)next - c_end);
                if (delta >= asize) {
                    insert = current;
                    break;
                }
                current = next;
            }
        } else if (delta >= size) {
            insert = area_queue->tail;
        }

        if (insert == NULL) {
            delta = (uint64_t)((uint8_t *)area_queue->head - area_queue->start_addr);
        } else {
            size_t si = area_get_size(insert);
            size_t offset = (uintptr_t)((uint8_t *)insert + si);
            aligned_addr = (void *)align_up((uintptr_t)offset, alignment);
        }
    }
    if (delta < size) {
        return NULL;
    }

    Area *new_area = (Area *)alloc_memory_aligned(aligned_addr, size, alignment, true, area_queue->end_addr);
    if (new_area == NULL) {
        return NULL;
    }
    new_area->active_mask.whole = 0;
    new_area->constr_mask.whole = 0;
    new_area->partition_id = area_queue->partition_id;
    area_set_next(new_area, NULL);
    area_set_prev(new_area, NULL);
    area_list_insert_at(area_queue, insert, new_area);
    // list_enqueue(*area_queue, new_area);
    return new_area;
}

static bool partition_allocator_try_release_containers(PartitionAllocator *pa, Area *area)
{
    if (area_is_free(area)) {

        // all sections should be free and very likely in the free sections
        // list.
        int num_sections = area_get_section_count(area);
        Section *root_section = (Section *)area;
        ContainerType root_ctype = section_get_container_type(root_section);
        if (root_ctype == HEAP) {
            ExponentType exp = section_get_container_exponent(root_section);
            Heap *heap = (Heap *)section_get_collection(root_section, 0, exp);
            Queue *queue = &pa->heaps[section_get_container_exponent(root_section) - 3];
            list_remove(queue, heap);
            list_remove(pa->sections, root_section);
            return true;
        }

        for (int i = 0; i < num_sections; i++) {
            Section *section = (Section *)((uint8_t *)area + SECTION_SIZE * i);

            if (!area_is_claimed(area, i)) {
                continue;
            }
            int num_collections = section_get_collection_count(section);
            ExponentType exp = section_get_container_exponent(section);

            for (int j = 0; j < num_collections; j++) {
                if (!section_is_claimed(section, j)) {
                    continue;
                }
                Pool *pool = (Pool *)section_get_collection(section, j, exp);
                Queue *queue = &pa->pools[pool->block_idx];
                list_remove(queue, pool);
            }

            list_remove(pa->sections, section);
        }
        return true;
    }
    return false;
}

static void partition_allocator_free_area(PartitionAllocator *pa, Area *a, const AreaType t)
{
    switch (t) {
    case AT_FIXED_32: {
        pa->area_01.area_count--;
        area_list_remove(&pa->area_01, a);
        if ((a == pa->area_01.previous_area) || (pa->area_01.area_count == 0)) {
            pa->area_01.previous_area = NULL;
        }

        break;
    }
    case AT_FIXED_128: {
        pa->area_2.area_count--;
        area_list_remove(&pa->area_2, a);
        if ((a == pa->area_2.previous_area) || (pa->area_2.area_count == 0)) {
            pa->area_2.previous_area = NULL;
        }
        break;
    }
    default: {
        pa->area_3.area_count--;
        area_list_remove(&pa->area_3, a);
        if ((a == pa->area_3.previous_area) || (pa->area_3.area_count == 0)) {
            pa->area_3.previous_area = NULL;
        }
        break;
    }
    };
    free_memory(a, area_get_size(a));
}

static void partition_allocator_try_free_area(PartitionAllocator *pa, Area *area)
{
    size_t size = area_get_size(area);
    if (size == AREA_SIZE_SMALL) {
        partition_allocator_free_area(pa, area, AT_FIXED_32);
    } else if (size == AREA_SIZE_LARGE) {
        partition_allocator_free_area(pa, area, AT_FIXED_128);
    } else if (size == AREA_SIZE_HUGE) {
        partition_allocator_free_area(pa, area, AT_FIXED_256);
    } else {
        partition_allocator_free_area(pa, area, AT_VARIABLE);
    }
}

static bool partition_allocator_try_release_area(PartitionAllocator *pa, Area *area)
{
    if (area_is_free(area)) {

        partition_allocator_try_free_area(pa, area);
        return true;
    }
    return false;
}

static bool partition_allocator_release_areas_from_queue(PartitionAllocator *pa, AreaList *queue)
{
    bool was_released = false;
    Area *start = queue->head;
    // find free section.
    // detach all pools/pages/sections.
    while (start != NULL) {
        Area *next = area_get_next(start);
        was_released |= partition_allocator_try_release_containers(pa, start);
        start = next;
    }
    start = queue->head;
    while (start != NULL) {
        Area *next = area_get_next(start);
        was_released |= partition_allocator_try_release_area(pa, start);
        start = next;
    }
    return was_released;
}

static bool partition_allocator_release_single_area_from_queue(PartitionAllocator *pa, AreaList *queue)
{
    bool was_released = false;
    Area *start = queue->head;
    // find free section.
    // detach all pools/pages/sections.
    while (start != NULL) {
        Area *next = area_get_next(start);
        was_released |= partition_allocator_try_release_containers(pa, start);
        if (was_released) {
            was_released |= partition_allocator_try_release_area(pa, start);
            break;
        }
        start = next;
    }
    return was_released;
}

static bool partition_allocator_release_local_areas(PartitionAllocator *pa)
{
    bool was_released = false;
    if (pa->area_01.area_count) {
        was_released |= partition_allocator_release_areas_from_queue(pa, &pa->area_01);
    }
    if (pa->area_2.area_count) {
        was_released |= partition_allocator_release_areas_from_queue(pa, &pa->area_2);
    }
    if (pa->area_3.area_count) {
        was_released |= partition_allocator_release_areas_from_queue(pa, &pa->area_3);
    }
    return was_released;
}

/*
static AreaList *partition_allocator_get_area_queue(PartitionAllocator *pa, Area *area)
{
    size_t size = area_get_size(area);
    if (size == AREA_SIZE_SMALL) {
        return &pa->area_01;
    } else if (size == AREA_SIZE_LARGE) {
        return &pa->area_2;
    } else {
        return &pa->area_3;
    }
}
*/

static Area *partition_allocator_get_free_area_from_queue(AreaList *current_queue)
{
    // the areas are empty
    Area *new_area = NULL;
    Area *previous_area = current_queue->previous_area;
    if (previous_area != NULL) {
        if (!area_is_full(previous_area)) {
            new_area = previous_area;
        }
    } else {
        if (current_queue->head != NULL) {
            Area *start = current_queue->head;
            while (start != NULL) {
                Area *next = area_get_next(start);
                if (!area_is_full(start)) {
                    new_area = start;
                    break;
                }
                start = next;
            }
        }
    }
    return new_area;
}

static inline uint32_t partition_allocator_get_max_area_count(AreaType t)
{
    switch (t) {
    case AT_FIXED_32: {
        return MAX_SMALL_AREAS;
    }
    case AT_FIXED_128: {
        return MAX_LARGE_AREAS;
    }
    default: {
        return MAX_HUGE_AREAS;
    }
    };
}
static AreaList *partition_allocator_get_current_queue(PartitionAllocator *pa, AreaType t, const size_t s,
                                                       size_t *area_size, size_t *alignement)
{
    switch (t) {
    case AT_FIXED_32: {
        *area_size = AREA_SIZE_SMALL;
        *alignement = *area_size;
        return &pa->area_01;
    }
    case AT_FIXED_128: {
        *area_size = AREA_SIZE_LARGE;
        *alignement = *area_size;
        return &pa->area_2;
    }
    case AT_FIXED_256: {
        *area_size = AREA_SIZE_HUGE;
        *alignement = *area_size;
        return &pa->area_3;
    }
    default: {
        *area_size = s;
        *alignement = AREA_SIZE_HUGE;
        return &pa->area_3;
    }
    };
}

static AreaList *partition_allocator_promote_area(PartitionAllocator *pa, AreaType *t, size_t *area_size,
                                                  size_t *alignement)
{
    switch (*t) {
    case AT_FIXED_32: {
        *area_size = AREA_SIZE_LARGE;
        *alignement = *area_size;
        *t = AT_FIXED_128;
        return &pa->area_2;
    }
    case AT_FIXED_128: {
        *area_size = AREA_SIZE_HUGE;
        *alignement = *area_size;
        *t = AT_FIXED_256;
        return &pa->area_3;
    }
    default: {
        return NULL;
    }
    };
}

static Area *partition_allocator_get_free_area(PartitionAllocator *pa, const size_t s, AreaType t)
{
    size_t area_size = AREA_SIZE_SMALL;
    size_t alignment = area_size;
    AreaList *current_queue = partition_allocator_get_current_queue(pa, t, s, &area_size, &alignment);

    // the areas are empty
    Area *new_area = partition_allocator_get_free_area_from_queue(current_queue);
    // try promoting first.
    if (new_area == NULL && (partition_allocator_get_max_area_count(t) == current_queue->area_count)) {
        current_queue = partition_allocator_promote_area(pa, &t, &area_size, &alignment);
        if (current_queue == NULL) {
            return NULL;
        }
        new_area = partition_allocator_get_free_area_from_queue(current_queue);
        if (new_area == NULL && (partition_allocator_get_max_area_count(t) == current_queue->area_count)) {
            return NULL;
        }
    }

    if (new_area == NULL) {
        if (area_size < os_page_size) {
            area_size = os_page_size;
        }
        new_area = partition_allocator_alloc_area(current_queue, area_size, alignment);
        if (new_area == NULL) {
            return NULL;
        }
        area_set_size(new_area, area_size);
    }

    return new_area;
}
static inline uint32_t partition_allocator_claim_section(Area *area) { return area_claim_section(area); }
static Area *partition_allocator_get_area(PartitionAllocator *pa, const size_t size, const ContainerType t)
{
    Area *curr_area = NULL;
    uint32_t small_area_limit = LARGE_OBJECT_SIZE;
    if (t == HEAP) {
        small_area_limit = MEDIUM_OBJECT_SIZE;
    }
    if (size <= small_area_limit) {
        curr_area = partition_allocator_get_free_area(pa, size, AT_FIXED_32);
        if (curr_area == NULL) {
            bool was_released = partition_allocator_release_single_area_from_queue(pa, &pa->area_01);
            was_released |= partition_allocator_release_single_area_from_queue(pa, &pa->area_2);
            if (was_released) {
                // try again.
                curr_area = partition_allocator_get_free_area(pa, size, AT_FIXED_32);
            }
        }
    } else if (size <= AREA_SIZE_SMALL) {
        curr_area = partition_allocator_get_free_area(pa, size, AT_FIXED_128);
        if (curr_area == NULL) {
            bool was_released = partition_allocator_release_single_area_from_queue(pa, &pa->area_2);
            was_released |= partition_allocator_release_single_area_from_queue(pa, &pa->area_3);
            if (was_released) {
                // try again.
                curr_area = partition_allocator_get_free_area(pa, size, AT_FIXED_128);
            }
        }
    } else if (size <= AREA_SIZE_LARGE) {
        curr_area = partition_allocator_get_free_area(pa, size, AT_FIXED_256);
        if (curr_area == NULL) {
            if (partition_allocator_release_single_area_from_queue(pa, &pa->area_3)) {
                // try again.
                curr_area = partition_allocator_get_free_area(pa, size, AT_FIXED_256);
            }
        }
    } else {
        curr_area = partition_allocator_get_free_area(pa, size, AT_VARIABLE);
        if (curr_area == NULL) {
            if (partition_allocator_release_single_area_from_queue(pa, &pa->area_3)) {
                // try again.
                curr_area = partition_allocator_get_free_area(pa, size, AT_VARIABLE);
            }
        }
    }

    return curr_area;
}

static Section *partition_allocator_alloc_section(PartitionAllocator *pa, const size_t size, const ContainerType t)
{
    Area *new_area = partition_allocator_get_area(pa, size, t);
    if (new_area == NULL) {
        return NULL;
    }

    int32_t section_idx = partition_allocator_claim_section(new_area);

    Section *section = (Section *)((uint8_t *)new_area + SECTION_SIZE * section_idx);
    section->constr_mask.whole = 0;
    section->active_mask.whole = 0;
    section->idx = section_idx;
    section->partition_id = new_area->partition_id;
    return section;
}

static Area *partition_allocator_alloc_area(AreaList *area_queue, const uint64_t area_size, const uint64_t alignment)
{
    Area *new_area = partition_allocator_get_next_area(area_queue, area_size, alignment);
    if (new_area == NULL) {
        return NULL;
    }
    area_queue->previous_area = new_area;
    area_queue->area_count++;
    return new_area;
}

#define PARTITION_0 ((uintptr_t)2 << 40)
#define PARTITION_1 ((uintptr_t)4 << 40)
#define PARTITION_2 ((uintptr_t)8 << 40)
#define PARTITION_3 ((uintptr_t)16 << 40)
#define PARTITION_4 ((uintptr_t)32 << 40)
#define PARTITION_5 ((uintptr_t)64 << 40)
#define PARTITION_6 ((uintptr_t)128 << 40)

static cache_align PartitionAllocator partition_allocators[MAX_THREADS];

static AreaList partition_area_4 = {NULL, NULL, 0, ((uintptr_t)32 << 40), ((uintptr_t)64 << 40), AT_VARIABLE, 0, NULL};
static AreaList partition_area_5 = {NULL, NULL, 0, ((uintptr_t)64 << 40), ((uintptr_t)128 << 40), AT_VARIABLE, 0, NULL};

static const int32_t thread_message_imit = 100;

typedef struct Allocator_t
{
    int64_t _thread_idx;
    PartitionAllocator *part_alloc;
    PartitionAllocator *thread_free_part_alloc;
    Pool *cached_pool;
    uintptr_t cached_pool_start;
    uintptr_t cached_pool_end;
} Allocator;

static inline void allocator_free(Allocator *a, void *p);

static inline void allocator_set_cached_pool(Allocator *a, Pool *p)
{
    if (p == a->cached_pool) {
        return;
    }
    a->cached_pool = p;
    a->cached_pool_start = (uintptr_t)&p->blocks[0];
    a->cached_pool_end = (uintptr_t)((uint8_t *)a->cached_pool_start + (p->num_available * p->block_size));
}

static inline bool allocator_check_cached_pool(const Allocator *a, const void *p)
{
    if (a->cached_pool) {
        return (uintptr_t)p >= a->cached_pool_start && (uintptr_t)p <= a->cached_pool_end;
    } else {
        return false;
    }
}

static inline void allocator_unset_cached_pool(Allocator *a)
{
    if (a->cached_pool) {
        if (!pool_is_full(a->cached_pool)) {
            Queue *queue = &a->part_alloc->pools[a->cached_pool->block_idx];
            list_enqueue(queue, a->cached_pool);
        }
    }
    a->cached_pool = NULL;
}

static void allocator_thread_enqueue(message_queue *queue, message *first, message *last)
{
    atomic_store_explicit(&last->next, (uintptr_t)NULL,
                          memory_order_release); // last.next = null
    message *prev = (message *)atomic_exchange_explicit(&queue->tail, (uintptr_t)last,
                                                        memory_order_release); // swap back and last
    atomic_store_explicit(&prev->next, (uintptr_t)first,
                          memory_order_release); // prev.next = first
}
static void allocator_thread_dequeue_all(Allocator *a, message_queue *queue);
// static void allocator_thread_dequeue(Allocator *a, message_queue *queue);

static inline void allocator_flush_thread_free_queue(Allocator *a)
{
    Queue *q = (Queue *)a->part_alloc->thread_free_queue;
    if ((uintptr_t)q->head != (uintptr_t)q->tail) {
        allocator_thread_dequeue_all(a, a->part_alloc->thread_free_queue);
    }
}

static void allocator_flush_thread_free(Allocator *a)
{
    if (a->thread_free_part_alloc != NULL) {
        // get the first and last item of the tf queue
        message *lm = partition_allocator_get_last_message(a->part_alloc);
        if (lm != NULL) {
            allocator_thread_enqueue(a->thread_free_part_alloc->thread_free_queue, a->part_alloc->thread_messages, lm);
            a->part_alloc->message_count = 0;
        }
    }
}

static void allocator_thread_free(Allocator *a, void *p, const uint64_t pid)
{
    PartitionAllocator *_part_alloc = &partition_allocators[pid];
    if (_part_alloc != a->thread_free_part_alloc) {
        allocator_flush_thread_free(a);
        a->thread_free_part_alloc = _part_alloc;
    }
    partition_allocator_thread_free(a->part_alloc, p);
    if (a->part_alloc->message_count > thread_message_imit) {
        allocator_flush_thread_free(a);
    }
}

static inline void allocator_free_from_section(Allocator *a, void *p, const size_t area_size)
{
    Section *section = (Section *)((uintptr_t)p & ~(area_size - 1));
    // if it is page section, free
    if (a->_thread_idx == partition_owners[section->partition_id]) {
        if (section_get_container_type(section) == POOL) {
            section = (Section *)((uintptr_t)p & ~(SECTION_SIZE - 1));
            Pool *pool = (Pool *)section_find_collection(section, p);
            pool_free_block(pool, p);
            allocator_set_cached_pool(a, pool);
            if (!section_is_connected(section)) {
                PartitionAllocator *_part_alloc = &partition_allocators[section->partition_id];
                Queue *sections = _part_alloc->sections;
                if (sections->head != section && sections->tail != section) {
                    list_enqueue(sections, section);
                }
            }
        } else {
            Heap *heap = (Heap *)section_find_collection(section, p);
            uint32_t heapIdx = section_get_container_exponent(section) - 3;
            heap_free(heap, p, heapIdx > 0);
            // if the free pools list is empty.
            if (!heap_is_connected(heap)) {
                // reconnect
                PartitionAllocator *_part_alloc = &partition_allocators[section->partition_id];
                Queue *queue = &_part_alloc->heaps[heapIdx];
                if (queue->head != heap && queue->tail != heap) {
                    list_enqueue(queue, heap);
                }
            }
        }
    } else {
        allocator_thread_free(a, p, section->partition_id);
    }
}

static inline void allocator_free_huge(Allocator *a, void *p)
{
    Section *section = (Section *)((uintptr_t)p & ~(AREA_SIZE_HUGE - 1));
    // if it is page section, free
    if (a->_thread_idx == partition_owners[section->partition_id]) {
        PartitionAllocator *_part_alloc = &partition_allocators[section->partition_id];
        if (section_get_container_type(section) == HEAP) {
            Heap *heap = (Heap *)section_find_collection(section, p);
            heap_free(heap, p, true);
            // if the pool is disconnected from the queue
            if (!heap_is_connected(heap)) {
                Queue *queue = &_part_alloc->heaps[section_get_container_exponent(section) - 3];
                // reconnect
                list_enqueue(queue, heap);
            }
        } else // SLAB
        {
            Area *area = (Area *)section;
            partition_allocator_try_free_area(_part_alloc, area);
        }
    } else {
        allocator_thread_free(a, p, section->partition_id);
    }
}

static Section *allocator_get_free_section(Allocator *a, const size_t s, const ContainerType t)
{
    int32_t exponent = get_container_exponent(s, t);
    Section *free_section = (Section *)a->part_alloc->sections->head;

    // find free section.
    while (free_section != NULL) {
        Section *next = free_section->next;
        if (section_get_container_exponent(free_section) == exponent) {
            if (!section_is_full(free_section)) {
                break;
            } else {
                list_remove(a->part_alloc->sections, free_section);
            }
        }
        free_section = next;
    }

    if (free_section == NULL) {
        Section *new_section = partition_allocator_alloc_section(a->part_alloc, s, t);
        if (new_section == NULL) {
            return NULL;
        }
        section_set_container_exponent(new_section, exponent);
        section_set_container_type(new_section, t);

        new_section->next = NULL;
        new_section->prev = NULL;
        list_enqueue(a->part_alloc->sections, new_section);

        free_section = new_section;
    }
    return free_section;
}

__attribute__((malloc)) static void *allocator_alloc_from_page(Allocator *a, const size_t s)
{
    Heap *start = NULL;
    Queue *queue = &a->part_alloc->heaps[sizeToPage(s)];
    if (queue->head != NULL) {
        start = (Heap *)queue->head;
        while (start != NULL) {
            Heap *next = start->next;
            if (heap_has_room(start, s)) {
                return heap_get_block(start, (uint32_t)s);
            } else {
                // disconnect full pages
                list_remove(queue, start);
            }
            start = next;
        }
    }

    Area *new_area = partition_allocator_get_area(a->part_alloc, s, HEAP);
    if (new_area == NULL) {
        return NULL;
    }

    Section *new_section = (Section *)((uint8_t *)new_area);
    new_section->idx = 0;

    ExponentType exponent = (ExponentType)get_container_exponent(s, HEAP);
    new_section->partition_id = new_area->partition_id;
    section_set_container_exponent(new_section, exponent);
    section_set_container_type(new_section, HEAP);

    new_section->next = NULL;
    new_section->prev = NULL;

    section_claim_all(new_section);
    start = (Heap *)section_get_collection(new_section, 0, exponent);
    heap_init(start, 0, exponent);

    a->part_alloc->heap_count++;
    list_enqueue(queue, start);
    return heap_get_block(start, (uint32_t)s);
}

__attribute__((malloc)) static void *allocator_alloc_slab(Allocator *a, const size_t s)
{
    size_t totalSize = sizeof(Area) + s;
    Area *area = partition_allocator_get_area(a->part_alloc, totalSize, HEAP);
    if (area == NULL) {
        return NULL;
    }
    Section *section = (Section *)area;
    area_reserve_all(area);
    section_reserve_all(section);
    section_set_container_type(section, SLAB);
    return &section->collections[0];
}

__attribute__((malloc)) static inline Pool *allocator_alloc_pool(Allocator *a, const uint32_t idx, const uint32_t s)
{
    Section *sfree_section = allocator_get_free_section(a, s, POOL);
    if (sfree_section == NULL) {
        return NULL;
    }

    const unsigned int coll_idx = section_reserve_next(sfree_section);
    const ExponentType exp = section_get_container_exponent(sfree_section);
    Pool *p = (Pool *)section_get_collection(sfree_section, coll_idx, exp);
    pool_init(p, coll_idx, idx, s, exp);
    section_claim_idx(sfree_section, coll_idx);
    return p;
}

static inline void *allocator_alloc_from_pool(Allocator *a, const size_t s)
{
    int32_t pool_idx = sizeToPool(s);
    Queue *queue = &a->part_alloc->pools[pool_idx];
    Pool *start = queue->head;

    while (start != NULL) {
        Pool *next = start->next;
        if (start->free == NULL) {
            if (!pool_is_fully_commited(start)) {
                allocator_set_cached_pool(a, start);
                return pool_extend(start);
            }
            list_remove(queue, start);
        } else {
            goto return_block;
        }
        start = next;
    }

    start = allocator_alloc_pool(a, pool_idx, (uint32_t)s);
    if (start == NULL) {
        return NULL;
    }

    a->part_alloc->pool_count++;
return_block:

    allocator_set_cached_pool(a, start);
    return pool_get_free_block(start);
}

static inline bool allocator_is_main(Allocator *a) { return a->_thread_idx == 0; }
static inline int64_t allocator_thread_id(Allocator *a) { return (int64_t)a; }

static inline void _allocator_free(Allocator *a, void *p)
{
    if (allocator_check_cached_pool(a, p)) {
        pool_free_block(a->cached_pool, p);
    } else {
        a->cached_pool = NULL;
        switch (partition_from_addr((uintptr_t)p)) {
        case 0:
        case 1:
            allocator_free_from_section(a, p, AREA_SIZE_SMALL);
            break;
        case 2:
            allocator_free_from_section(a, p, AREA_SIZE_LARGE);
            break;
        case 3:
            allocator_free_huge(a, p);
            break;
        default:
            break;
        }
    }
}
static void allocator_thread_dequeue_all(Allocator *a, message_queue *queue)
{
    message *back = (message *)atomic_load_explicit(&queue->tail, memory_order_relaxed);
    message *curr = (message *)(uintptr_t)queue->head;

    // loop between start and end addres
    while (curr != back) {
        message *next = (message *)atomic_load_explicit(&curr->next, memory_order_acquire);
        if (next == NULL)
            break;
        _allocator_free(a, curr);
        curr = next;
    }
    queue->head = (uintptr_t)curr;
}

/*
static void allocator_thread_dequeue(Allocator *a, message_queue *queue)
{
    message *back = (message *)atomic_load_explicit(&queue->tail, memory_order_relaxed);
    message *curr = (message *)(uintptr_t)queue->head;

    // loop between start and end addres
    if (curr != back) {
        message *next = (message *)atomic_load_explicit(&curr->next, memory_order_acquire);
        if (next == NULL)
            return;
        _allocator_free(a, curr);
        curr = next;
    }
    queue->head = (uintptr_t)curr;
}
*/
static __thread int32_t allocator_main_index = 0;
static cache_align Allocator allocator_list[MAX_THREADS];
static __thread Allocator *thread_instance = &allocator_list[0];

static inline void *allocator_try_alloc_from_pool(Allocator *a, size_t s)
{
    void *ptr = allocator_alloc_from_pool(a, s);
    if (ptr == NULL) {
        a->cached_pool = NULL;
        // try again
        int8_t new_partition_set_idx = reserve_any_partition_set_for(allocator_main_index);
        if (new_partition_set_idx != -1) {
            allocator_flush_thread_free(a);
            allocator_flush_thread_free_queue(a);
            a->part_alloc = &partition_allocators[new_partition_set_idx];
            ptr = allocator_alloc_from_pool(a, s);
        }
    }
    return ptr;
}

static inline void *allocator_malloc(Allocator *a, size_t s)
{
    const size_t as = ALIGN(s);
    if (a->cached_pool != NULL) {
        if (as == a->cached_pool->block_size) {
            void *block = pool_aquire_block(a->cached_pool);
            if (block != NULL) {
                return block;
            }
            allocator_unset_cached_pool(a);
        } else {
            allocator_unset_cached_pool(a);
        }
    }

    // this also needs attention, so I can wrap up the allocator soon!!
    allocator_flush_thread_free_queue(a);

    if (s <= LARGE_OBJECT_SIZE) {
        return allocator_alloc_from_pool(a, as);
    } else if (s <= AREA_SIZE_LARGE) {
        // allocate form the large page
        return allocator_alloc_from_page(a, as);
    } else {
        return allocator_alloc_slab(a, as);
    }
}

void *allocator_malloc_page(Allocator *a, size_t s)
{
    a->cached_pool = NULL;
    allocator_flush_thread_free_queue(a);
    if (s <= AREA_SIZE_LARGE) {
        // allocate form the large page
        return allocator_alloc_from_page(a, s);
    } else {
        return allocator_alloc_slab(a, s);
    }
}

static inline void allocator_free(Allocator *a, void *p)
{
    if (p == NULL)
        return;
    allocator_flush_thread_free_queue(a);
    _allocator_free(a, p);
}

bool allocator_release_local_areas(Allocator *a)
{
    a->cached_pool = NULL;
    bool result = false;
    uint8_t midx = allocator_main_index;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (partition_owners[i] == midx) {
            PartitionAllocator *palloc = &partition_allocators[i];
            allocator_thread_dequeue_all(a, palloc->thread_free_queue);
            bool was_released = partition_allocator_release_local_areas(palloc);
            for (int j = 0; j < POOL_BIN_COUNT; j++) {
                if (palloc->pools[j].head != NULL || palloc->pools[j].tail != NULL) {
                    palloc->pools[j].head = NULL;
                    palloc->pools[j].tail = NULL;
                }
            }
            if (midx != i && was_released) {
                release_partition_set(i);
            }
            result |= was_released;
        }
    }

    return result;
}

static void allocator_destroy()
{

    static bool done = false;
    if (done)
        return;
    done = true;

#if defined(_WIN32) && !defined(MI_SHARED_LIB)
    tls_set(alloc_tls_key, NULL);
    tls_delete(alloc_tls_key);
#endif

    if (allocator_main_index != 0)
        release_partition_set(allocator_main_index);
    //  clean memory
}
static Allocator *allocator_get_thread_instance(void) { return thread_instance; }

static void allocator_init(size_t max_threads)
{
    static bool init = false;
    if (init)
        return;
    init = true;

    os_page_size = get_os_page_size();

    for (size_t i = 0; i < max_threads; i++) {
        Queue *pool_base = pool_queues[i];
        for (int j = 0; j < POOL_BIN_COUNT; j++) {
            pool_base[j].head = NULL;
            pool_base[j].tail = NULL;
        }
    }

    for (size_t i = 0; i < max_threads; i++) {
        Queue *heap_base = heap_queues[i];
        heap_base[0].head = NULL;
        heap_base[0].tail = NULL;
        heap_base[1].head = NULL;
        heap_base[1].tail = NULL;
        heap_base[2].head = NULL;
        heap_base[2].tail = NULL;
    }

    for (size_t i = 0; i < max_threads; i++) {
        Queue *section_base = &section_queues[i];
        section_base->head = NULL;
        section_base->tail = NULL;
    }

    for (size_t i = 0; i < max_threads; i++) {
        message *message_base = &message_sentinals[i];
        message_base->next = (0UL);
    }

    for (size_t i = 0; i < max_threads; i++) {
        message_queue *message_base = &message_queues[i];
        message_base->head = (0UL);
        message_base->tail = (0UL);
    }

    partition_owners[0] = 0;
    for (size_t i = 1; i < max_threads; i++) {
        partition_owners[i] = -1;
    }

    for (size_t i = 0; i < max_threads; i++) {
        PartitionAllocator *palloc = &partition_allocators[i];
        palloc->area_01.head = NULL;
        palloc->area_01.tail = NULL;
        palloc->area_01.partition_id = (uint32_t)i;
        palloc->area_01.start_addr = (i) * (SZ_GB * 6) + PARTITION_0;
        palloc->area_01.end_addr = (i) * (SZ_GB * 6) + PARTITION_0 + (SZ_GB * 6);
        palloc->area_01.type = AT_FIXED_32;
        palloc->area_01.area_count = 0;
        palloc->area_01.previous_area = NULL;

        palloc->area_2.head = NULL;
        palloc->area_2.tail = NULL;
        palloc->area_2.partition_id = (uint32_t)i;
        palloc->area_2.start_addr = (i) * (SZ_GB * 8) + PARTITION_2;
        palloc->area_2.end_addr = (i) * (SZ_GB * 8) + PARTITION_2 + (SZ_GB * 8);
        palloc->area_2.type = AT_FIXED_128;
        palloc->area_2.area_count = 0;
        palloc->area_2.previous_area = NULL;

        palloc->area_3.head = NULL;
        palloc->area_3.tail = NULL;
        palloc->area_3.partition_id = (uint32_t)i;
        palloc->area_3.start_addr = (i) * (SZ_GB * 16) + PARTITION_3;
        palloc->area_3.end_addr = (i) * (SZ_GB * 16) + PARTITION_3 + (SZ_GB * 16);
        palloc->area_3.type = AT_FIXED_256;
        palloc->area_3.area_count = 0;
        palloc->area_3.previous_area = NULL;

        palloc->sections = &section_queues[i];
        palloc->heaps = heap_queues[i];
        palloc->heap_count = 0;
        palloc->pools = pool_queues[i];
        palloc->thread_messages = NULL;
        palloc->message_count = 0;
        palloc->thread_free_queue = &message_queues[i];
    }

    for (size_t i = 0; i < max_threads; i++) {
        allocator_list[i]._thread_idx = (int32_t)i;
        allocator_list[i].part_alloc = &partition_allocators[i];
        allocator_list[i].thread_free_part_alloc = NULL;
        allocator_list[i].cached_pool = NULL;
        allocator_list[i].cached_pool_start = 0;
        allocator_list[i].cached_pool_end = 0;
    }
}

#if defined(__cplusplus)
struct thread_init
{
    thread_init() { allocator_init(MAX_THREADS); }
    ~thread_init() { allocator_destroy(); }
};
static thread_init init;

#elif defined(__GNUC__) || defined(__clang__)
static void __attribute__((constructor)) library_init(void) { allocator_init(MAX_THREADS); }
static void __attribute__((destructor)) library_destroy(void) { allocator_destroy(); }
#endif

void *__attribute__((malloc)) cmalloc(size_t s) { return allocator_malloc(allocator_get_thread_instance(), s); }
void cfree(void *p) { allocator_free(allocator_get_thread_instance(), p); }
#endif /* Malloc_h */
