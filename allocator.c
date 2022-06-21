//
//  alloc.c
//  MemPoolTests
//
//  Created by Vilhelm Sævarsson on 20.6.2022.
//  Copyright © 2022 Vilhelm Sævarsson. All rights reserved.
//

#include "allocator.h"
#include "../cthread/cthread.h"

#if defined(_MSC_VER)
#define WINDOWS
#endif

#ifdef WINDOWS
#include <intrin.h>
#include <memoryapi.h>
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#define WSIZE 4
#define DSIZE 8

#define CACHE_LINE 64
#ifdef WINDOWS
#define cache_align __declspec(align(CACHE_LINE))
#else
#define cache_align __attribute__((aligned(CACHE_LINE)))
#endif

#define SMALL_OBJECT_SIZE DEFAULT_OS_PAGE_SIZE * 4 // 16k
#define MEDIUM_OBJECT_SIZE SMALL_OBJECT_SIZE * 8   // 128kb
#define LARGE_OBJECT_SIZE MEDIUM_OBJECT_SIZE * 16  // 2Mb
#define HUGE_OBJECT_SIZE LARGE_OBJECT_SIZE * 16    // 32Mb

#define BASE_AREA_SIZE (SECTION_SIZE * 8ULL) // 32Mb
#define AREA_SIZE_SMALL BASE_AREA_SIZE
#define AREA_SIZE_MEDIUM (SECTION_SIZE * 16ULL) // 64Mb
#define AREA_SIZE_LARGE (SECTION_SIZE * 32ULL)  // 128Mb
#define AREA_SIZE_HUGE (SECTION_SIZE * 64ULL)   // 256Mb

#define POOL_BIN_COUNT 17 * 8 + 1
#define HEAP_TYPE_COUNT 5

#define MAX_ARES 64

#define MAX_THREADS 1024
#define MAX(x, y) ((x) > (y) ? (x) : (y))
#define MIN(x, y) ((x) < (y) ? (x) : (y))
#define POWER_OF_TWO(x) ((x & (x - 1)) == 0)
#define ALIGN(x) ((MAX(x, 1) + sizeof(intptr_t) - 1) & ~(sizeof(intptr_t) - 1))

static cache_align const uintptr_t size_clss_to_exponent[] = {
    22,
    17, // 128k
    19, // 512k
    22, // 4Mb
};

static cache_align const uintptr_t area_type_to_size[] = {AREA_SIZE_SMALL, AREA_SIZE_MEDIUM, AREA_SIZE_LARGE,
                                                          AREA_SIZE_HUGE, UINT64_MAX};

static cache_align const uintptr_t area_type_to_exponent[] = {25, 26, 27, 28};

static size_t os_page_size = DEFAULT_OS_PAGE_SIZE;

typedef struct HeapBlock_t
{
    uint8_t *data;
} HeapBlock;

typedef struct QNode_t
{
    void *prev;
    void *next;
} QNode;

typedef struct Partition_t
{
    uint64_t partition_id;
    uintptr_t start_addr;
    uintptr_t end_addr;
    AreaType type;
    uint64_t area_mask;
    Area *previous_area;
} Partition;

// lockless message queue
typedef struct message_t
{
    _Atomic(uintptr_t) next;
} message;

typedef struct message_queue_t
{
    uintptr_t head;
    _Atomic(uintptr_t) tail;
} message_queue;

typedef void (*free_func)(void *);
typedef struct PartitionAllocator_t
{
    Partition area[4];

    // sections local to this thread with free pages or pools
    Queue *sections;
    // free pages that have room for various size allocations.
    Queue *heaps;
    // free pools of various sizes.
    Queue *pools;

    // collection of messages for other threads
    message *thread_messages;
    uint32_t message_count; // how many threaded message have we acccumuated for passing out
    // a queue of messages from other threads.
    message_queue *thread_free_queue;

} PartitionAllocator;

typedef struct Allocator_t
{
    int64_t idx;
    PartitionAllocator *part_alloc;
    PartitionAllocator *thread_free_part_alloc;
    Queue partition_allocators;
    Pool *cached_pool;
    uintptr_t cached_pool_start;
    uintptr_t cached_pool_end;
} Allocator;

bool commit_memory(void *base, size_t size)
{
#ifdef WINDOWS
    return VirtualAlloc(base, size, MEM_COMMIT, PAGE_READWRITE) == base;
#else
    return (mprotect(base, size, (PROT_READ | PROT_WRITE)) == 0);
#endif
}

bool decommit_memory(void *base, size_t size)
{
#ifdef WINDOWS
    return VirtualFree(base, size, MEM_DECOMMIT);
#else
    return (mmap(base, size, PROT_NONE, (MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE), -1, 0) == base);
#endif
}

bool free_memory(void *ptr, size_t size)
{
#ifdef WINDOWS
    return VirtualFree(ptr, 0, MEM_RELEASE) == 0;
#else
    return (munmap(ptr, size) == -1);
#endif
}

bool release_memory(void *ptr, size_t size, bool commit)
{
    if (commit) {
        return decommit_memory(ptr, size);
    } else {
        return free_memory(ptr, size);
    }
}

void *alloc_memory(void *base, size_t size, bool commit)
{
#ifdef WINDOWS
    int flags = commit ? MEM_RESERVE | MEM_COMMIT : MEM_RESERVE;
    return VirtualAlloc(base, size, flags, PAGE_READWRITE);
#else
    int flags = commit ? (PROT_WRITE | PROT_READ) : PROT_NONE;
    return mmap(base, size, flags, (MAP_PRIVATE | MAP_ANONYMOUS), -1, 0);
#endif
}

bool reset_memory(void *base, size_t size)
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

static inline uint64_t area_size_from_partition_id(uint8_t pid) { return area_type_to_size[pid]; }

int8_t partition_from_addr(uintptr_t p)
{
    static const uint8_t partition_count = 7;
    const int lz = 22 - __builtin_clz(p >> 32);
    if (lz < 0 || lz > partition_count) {
        return -1;
    } else {
        return lz;
    }
}

uint64_t area_size_from_addr(uintptr_t p) { return area_size_from_partition_id(partition_from_addr(p)); }

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

size_t get_os_page_size(void)
{
#ifdef WINDOWS
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize;
#else
    return sysconf(_SC_PAGESIZE);
#endif
}

uint32_t heap_block_get_header(HeapBlock *hb) { return *(uint32_t *)((uint8_t *)&hb->data - WSIZE); }

void heap_block_set_header(HeapBlock *hb, uint32_t s, uint32_t v)
{
    *(uint32_t *)((uint8_t *)&hb->data - WSIZE) = (s | v);
}

void heap_block_set_footer(HeapBlock *hb, uint32_t s, uint32_t v)
{
    const uint32_t size = (*(uint32_t *)((uint8_t *)&hb->data - WSIZE)) & ~0x7;
    *(uint32_t *)((uint8_t *)(&hb->data) + (size)-DSIZE) = (s | v);
}

HeapBlock *heap_block_next(HeapBlock *hb)
{
    const uint32_t size = *(uint32_t *)((uint8_t *)&hb->data - WSIZE) & ~0x7;
    return (HeapBlock *)((uint8_t *)&hb->data + (size));
}

HeapBlock *heap_block_prev(HeapBlock *hb)
{
    const uint32_t size = *(uint32_t *)((uint8_t *)&hb->data - DSIZE) & ~0x7;
    return (HeapBlock *)((uint8_t *)&hb->data - (size));
}

#define HEAP_NODE_OVERHEAD 8

int32_t get_pool_size_class(size_t s)
{
    if (s <= SMALL_OBJECT_SIZE) {         // 8 - 16k
        return ST_POOL_128K;              // 128k
    } else if (s <= MEDIUM_OBJECT_SIZE) { // 16k - 128k
        return ST_POOL_512K;              // 512k
    } else {
        return ST_POOL_4M; // 4M for > 128k objects.
    }
}

int32_t get_heap_size_class(size_t s)
{
    if (s <= SMALL_OBJECT_SIZE) {         // 8 - 16k
        return HT_4M;                     // 4
    } else if (s <= MEDIUM_OBJECT_SIZE) { // 16k - 128k
        return HT_32M;
    } else if (s <= LARGE_OBJECT_SIZE) { // 4Mb - 32Mb
        return HT_64M;
    } else if (s <= HUGE_OBJECT_SIZE) { // 4Mb - 32Mb
        return HT_128M;                 // 128
    } else {                            // for large than 32Mb objects.
        return HT_256M;                 // 256
    }
}

// list functions
static void _list_enqueue(void *queue, void *node, size_t head_offset, size_t prev_offset)
{
    Queue *tq = (Queue *)((uint8_t *)queue + head_offset);
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

static void _list_remove(void *queue, void *node, size_t head_offset, size_t prev_offset)
{
    Queue *tq = (Queue *)((uint8_t *)queue + head_offset);
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
#define list_enqueue(q, n) _list_enqueue(q, n, offsetof(__typeof__(*q), head), offsetof(__typeof__(*n), prev))
#define list_remove(q, n) _list_remove(q, n, offsetof(__typeof__(*q), head), offsetof(__typeof__(*n), prev))
/*
#define list_insert_at(q, t, n) _list_insert_at(q, t, n, offsetof(__typeof__(*q), head), offsetof(__typeof__(*n), prev))
#define list_insert_sort(q, t, n)                                                                                      \
    _list_insert_sort(q, n, offsetof(__typeof__(*q), head), offsetof(__typeof__(*n), prev))
*/
static const uintptr_t _Area_small_area_mask = 0xff;
static const uintptr_t _Area_medium_area_mask = 0xffff;
static const uintptr_t _Area_large_area_mask = 0xffffffff;

static inline uint32_t area_get_id(const Area *a) { return (uint32_t)(a->partition_mask & 0xffffffff); }
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
    static const uint64_t masks[] = {~(AREA_SIZE_SMALL - 1), ~(AREA_SIZE_MEDIUM - 1), ~(AREA_SIZE_LARGE - 1),
                                     ~(AREA_SIZE_HUGE - 1),  0xffffffffffffffff,      0xffffffffffffffff,
                                     0xffffffffffffffff};

    const int8_t pidx = partition_from_addr(p);
    if (pidx < 0) {
        return NULL;
    }
    return (Area *)(p & masks[pidx]);
}

static inline bool section_is_connected(const Section *s) { return s->prev != NULL || s->next != NULL; }

static uint8_t section_get_collection_count(const Section *s)
{
    switch (s->type) {
    case ST_POOL_128K: {
        return 32;
    }
    case ST_POOL_512K: {
        return 8;
    }
    default: {
        return 1;
    }
    }
}
static inline void section_free_idx(Section *s, uint8_t i)
{
    bitmask_free_idx_lo(&s->active_mask, i);
    const bool section_empty = bitmask_is_empty_lo(&s->active_mask);
    if (section_empty) {
        Area *area = area_from_addr((uintptr_t)s);
        area_free_idx(area, s->idx);
    }
}

static inline bool section_is_claimed(const Section *s, const uint8_t idx)
{
    return bitmask_is_set_lo(&s->constr_mask, idx);
}
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

static inline uint8_t section_reserve_next(Section *s) { return bitmask_allocate_bit_lo(&s->active_mask); }
static inline bool section_is_full(const Section *s)
{
    switch (s->type) {
    case ST_POOL_128K: {
        return (s->active_mask.whole & 0xffffffff) == 0xffffffff;
    }
    case ST_POOL_512K: {
        return (s->active_mask.whole & 0xff) == 0xff;
    }
    default: {
        return (s->active_mask.whole & 0x1) == 0x1;
    }
    }
}

static inline void *section_find_collection(Section *s, void *p)
{
    switch (s->type) {
    case ST_POOL_4M: {
        return (void *)&s->collections[0];
    }
    default: {
        const ptrdiff_t diff = (uint8_t *)p - (uint8_t *)&s->collections[0];
        const uintptr_t exp = size_clss_to_exponent[s->type];
        const int32_t collection_size = 1 << exp;
        const int32_t idx = (int32_t)((size_t)diff >> exp);
        return (void *)((uint8_t *)&s->collections[0] + collection_size * idx);
    }
    };
}

static inline uintptr_t section_get_collection(Section *s, int8_t idx, const int32_t psize)
{
    return (uintptr_t)((uint8_t *)&s->collections[0] + psize * idx);
}

static inline bool pool_is_full(const Pool *p) { return p->num_used >= p->num_available; }
static inline bool pool_is_fully_commited(const Pool *p) { return p->num_committed >= p->num_available; }

void pool_free_block(Pool *p, void *block)
{

    if (--p->num_used == 0) {
        Section *section = (Section *)((uintptr_t)p & ~(SECTION_SIZE - 1));
        section_free_idx(section, p->idx);

        /*
        int32_t psize = (1 << size_clss_to_exponent[section_get_type(section)]);
        const size_t block_memory = psize - sizeof(Pool) - sizeof(uintptr_t);
        const uintptr_t section_end = ((uintptr_t)p + (SECTION_SIZE - 1)) & ~(SECTION_SIZE - 1);
        const size_t remaining_size = section_end - (uintptr_t)&p->blocks[0];
        const size_t the_end = MIN(remaining_size, block_memory);
        const uintptr_t start_addr = (uintptr_t)&p->blocks[0];
        const uintptr_t first_page = (start_addr + (os_page_size - 1)) & ~(os_page_size - 1);
        const size_t mem_size = the_end - first_page;
        reset_memory((void*)first_page, mem_size);
         */
        p->free = (Block *)&p->blocks[0];
        p->free->next = NULL;
        return;
    }
    Block *new_free = (Block *)block;
    new_free->next = p->free;
    p->free = new_free;
}

static inline void *pool_extend(Pool *p)
{
    p->num_used++;
    return ((uint8_t *)&p->blocks[0] + (p->num_committed++ * p->block_size));
}

static void pool_init(Pool *p, const int8_t pidx, const uint32_t block_idx, const uint32_t block_size,
                      const int32_t psize)
{
    const size_t block_memory = psize - sizeof(Pool) - sizeof(uintptr_t);
    const uintptr_t section_end = ((uintptr_t)p + (SECTION_SIZE - 1)) & ~(SECTION_SIZE - 1);
    const size_t remaining_size = section_end - (uintptr_t)&p->blocks[0];
    p->idx = pidx;
    p->block_idx = block_idx;
    p->block_size = block_size;
    p->num_available = (int32_t)(MIN(remaining_size, block_memory) / block_size);
    p->num_committed = 1;
    p->num_used = 0;
    p->next = NULL;
    p->prev = NULL;
    p->free = (Block *)&p->blocks[0];
    p->free->next = NULL;
}

Block *pool_get_free_block(Pool *p)
{
    if (p->num_used++ == 0) {
        Section *section = (Section *)((uintptr_t)p & ~(SECTION_SIZE - 1));
        section_reserve_idx(section, p->idx);
    }
    Block *res = p->free;
    p->free = res->next;
    return res;
}

void *pool_aquire_block(Pool *p)
{
    if (p->free != NULL) {
        return pool_get_free_block(p);
    }

    if (!pool_is_fully_commited(p)) {
        return pool_extend(p);
    }

    return NULL;
}

bool heap_is_connected(const Heap *h) { return h->prev != NULL || h->next != NULL; }
bool heap_has_room(const Heap *h, const size_t s)
{
    if ((h->used_memory + s + HEAP_NODE_OVERHEAD) > h->total_memory) {
        return false;
    }
    if (s <= h->max_block && s >= h->min_block) {
        return true;
    }
    return false;
}

void heap_place(Heap *h, void *bp, uint32_t asize)
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

void *heap_find_fit(Heap *h, uint32_t asize)
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

void *heap_get_block(Heap *h, uint32_t s)
{
    if (s <= DSIZE * 2) {
        s = DSIZE * 2 + HEAP_NODE_OVERHEAD;
    } else {
        s = DSIZE * ((s + HEAP_NODE_OVERHEAD + DSIZE - 1) / DSIZE);
    }
    void *ptr = heap_find_fit(h, s);

    h->used_memory += s;
    if (h->num_allocations++ == 0) {
        if (h->total_memory < SECTION_SIZE) {
            Section *section = (Section *)((uintptr_t)h & ~(SECTION_SIZE - 1));
            section_reserve_idx(section, h->idx);
        } else {
            size_t area_size = area_size_from_addr((uintptr_t)h);
            Area *area = (Area *)((uintptr_t)h & ~(area_size - 1));
            area_reserve_all(area);
        }
    }
    return ptr;
}

void *heap_coalesce(Heap *h, void *bp)
{
    HeapBlock *hb = (HeapBlock *)bp;
    int32_t size = heap_block_get_header(hb) & ~0x7;
    HeapBlock *prev_block = heap_block_prev(hb);
    HeapBlock *next_block = heap_block_next(hb);
    int prev_header = heap_block_get_header(prev_block);
    int next_header = heap_block_get_header(next_block);

    const size_t prev_alloc = prev_header & 0x1;
    const size_t next_alloc = next_header & 0x1;

    QNode *hn = (QNode *)bp;
    if (!(prev_alloc && next_alloc)) {
        const size_t prev_size = prev_header & ~0x7;
        const size_t next_size = next_header & ~0x7;

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

void heap_reset(Heap *h)
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
    const uint32_t size = heap_block_get_header(hb) & ~0x7;
    heap_block_set_header(hb, size, 0);
    heap_block_set_footer(hb, size, 0);

    if (should_coalesce) {
        heap_coalesce(h, bp);
    } else {
        list_enqueue(&h->free_nodes, (QNode *)bp);
    }
    h->used_memory -= size;
    if (--h->num_allocations == 0) {
        if (h->total_memory < SECTION_SIZE) {
            Section *section = (Section *)((uintptr_t)h & ~(SECTION_SIZE - 1));
            section_free_idx(section, h->idx);
        } else {
            size_t area_size = area_size_from_addr((uintptr_t)h);
            Area *area = (Area *)((uintptr_t)h & ~(area_size - 1));
            area_free_all(area);
        }
        heap_reset(h);
    }
}

void heap_extend(Heap *h)
{
    *h->start = 0;
    *(h->start + WSIZE) = DSIZE | 1;   /* Prologue header */
    *(h->start + DSIZE) = (DSIZE | 1); /* Prologue footer */
    *(h->start + WSIZE + DSIZE) = 1;   /* Epilogue header */
    h->start = h->start + DSIZE * 2;

    heap_reset(h);
}

static void heap_init(Heap *h, int8_t pidx, const size_t psize)
{
    const uintptr_t section_end = ((uintptr_t)h + (psize - 1)) & ~(psize - 1);
    const size_t remaining_size = section_end - (uintptr_t)&h->blocks[0];

    const size_t block_memory = psize - sizeof(Heap) - sizeof(Section);
    const size_t header_footer_offset = sizeof(uintptr_t) * 2;
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
    // alignment is smaller than a page size or not a power of two.
    if ((alignment < os_page_size) || !POWER_OF_TWO(alignment))
        return NULL;

    // align size to page size
    size = (size + (os_page_size - 1)) & ~(os_page_size - 1);
    if (!safe_to_aquire(base, NULL, size, end)) {
        return NULL;
    }

    spinlock_lock(&alloc_lock);
    void *ptr = alloc_memory(base, size, false);
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
        ptr = alloc_memory(base, adj_size, false);
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
            goto err;
        }
    }
success:
    spinlock_unlock(&alloc_lock);
    if (!commit_memory(ptr, size)) {
        // something is greatly foobar.
        // not allowed to commit the memory we reserved.
        free_memory(ptr, size);
        return NULL;
    }
    return ptr;
err:
    spinlock_unlock(&alloc_lock);
    return NULL;
}

static inline uint8_t size_to_pool(const size_t as)
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

static int32_t find_first_nzeros(uintptr_t x, int64_t n)
{
    x = ~x;
    int64_t s;
    while (n > 1) {
        s = n >> 1;
        x = x & (x << s);
        n = n - s;
    }
    return 63 - (x == 0 ? 64 : __builtin_clzll(x));
}

uint8_t size_to_heap(const size_t as)
{
    if (as <= SMALL_OBJECT_SIZE) {
        return 0;
    } else if (as <= MEDIUM_OBJECT_SIZE) {
        return 1; // 128Mb pages
    } else if (as <= LARGE_OBJECT_SIZE) {
        return 2; // 128Mb pages
    } else if (as <= HUGE_OBJECT_SIZE) {
        return 3; // 128Mb pages
    } else {
        return 4; // 256Mb pages
    }
}

typedef void (*free_func)(void *);
static Area *partition_allocator_alloc_area(Partition *area_queue, uint64_t area_size, uint64_t alignment);
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

static Area *partition_allocator_get_next_area(Partition *area_queue, uint64_t size, uint64_t alignment)
{
    size_t type_size = area_type_to_size[area_queue->type];
    size_t range = size / type_size;
    range += (size % type_size) ? 1 : 0;
    size = type_size * range;
    uint32_t idx = find_first_nzeros(area_queue->area_mask, range);
    if (idx == -1) {
        return NULL; // no room.
    }
    uint64_t new_mask = (1UL << range) - 1UL;
    area_queue->area_mask |= (new_mask << idx);
    idx = 63 - idx;
    uintptr_t aligned_addr = area_queue->start_addr + (type_size * idx);

    Area *new_area = (Area *)alloc_memory_aligned((void *)aligned_addr, area_queue->end_addr, size, alignment);
    if (new_area == NULL) {
        return NULL;
    }
    area_init(new_area, area_queue->partition_id, area_queue->type, range);
    return new_area;
}

static bool partition_allocator_try_release_containers(PartitionAllocator *pa, Area *area)
{
    if (area_is_free(area)) {

        // all sections should be free and very likely in the free sections
        // list.
        const int num_sections = area_get_section_count(area);
        const ContainerType root_ctype = area_get_container_type(area);
        if (root_ctype == CT_HEAP) {
            Heap *heap = (Heap *)((uintptr_t)area + sizeof(Area));
            uint32_t heapIdx = (area_get_type(area));
            Queue *queue = &pa->heaps[heapIdx];
            list_remove(queue, heap);
            return true;
        }

        for (int i = 0; i < num_sections; i++) {
            Section *section = (Section *)((uint8_t *)area + SECTION_SIZE * i);

            if (!area_is_claimed(area, i)) {
                continue;
            }
            int num_collections = section_get_collection_count(section);
            const SectionType st = section->type;
            int32_t psize = (1 << size_clss_to_exponent[st]);
            for (int j = 0; j < num_collections; j++) {
                if (!section_is_claimed(section, j)) {
                    continue;
                }
                void *collection = (void *)section_get_collection(section, j, psize);
                if (st != ST_HEAP_4M) {
                    Pool *pool = (Pool *)collection;
                    Queue *queue = &pa->pools[pool->block_idx];
                    list_remove(queue, pool);
                } else {
                    Heap *heap = (Heap *)collection;
                    Queue *queue = &pa->heaps[0];
                    list_remove(queue, heap);
                }
            }

            list_remove(pa->sections, section);
        }
        return true;
    }
    return false;
}

static void partition_allocator_free_area_from_list(PartitionAllocator *pa, Area *a, Partition *list, size_t idx)
{
    size_t range = area_get_range(a);
    uint64_t new_mask = ((1UL << range) - 1UL) << (64UL - idx - range);
    list->area_mask = list->area_mask & ~new_mask;
    if ((a == list->previous_area) || (list->area_mask == 0)) {
        list->previous_area = NULL;
    }
    free_memory(a, area_get_size(a));
}

static Partition *partition_allocator_get_area_list(PartitionAllocator *pa, Area *area)
{
    const AreaType at = area_get_type(area);
    return &pa->area[at];
}

static uint32_t partition_allocator_get_area_idx_from_queue(PartitionAllocator *pa, Area *area, Partition *queue)
{
    const ptrdiff_t diff = (uint8_t *)area - (uint8_t *)queue->start_addr;
    return (uint32_t)(((size_t)diff) >> area_type_to_exponent[area_get_type(area)]);
}

static void partition_allocator_free_area(PartitionAllocator *pa, Area *area)
{
    Partition *queue = partition_allocator_get_area_list(pa, area);
    int32_t idx = partition_allocator_get_area_idx_from_queue(pa, area, queue);
    partition_allocator_free_area_from_list(pa, area, queue, idx);
}

static bool partition_allocator_try_free_area(PartitionAllocator *pa, Area *area, Partition *list)
{
    if (area_is_free(area)) {

        int32_t idx = partition_allocator_get_area_idx_from_queue(pa, area, list);
        partition_allocator_free_area_from_list(pa, area, list, idx);
        return true;
    }
    return false;
}

static int32_t area_list_get_next_area_idx(Partition *queue, uint32_t cidx)
{
    uint64_t msk_cpy = queue->area_mask << cidx;
    if (msk_cpy == 0 || (cidx > 63)) {
        return -1;
    }
    return __builtin_clzll(msk_cpy) + cidx;
}

static Area *area_list_get_area(Partition *queue, uint32_t cidx)
{
    uintptr_t area_addr = queue->start_addr + ((1 << queue->type) * BASE_AREA_SIZE) * cidx;
    return (Area *)area_addr;
}

static bool partition_allocator_release_areas_from_queue(PartitionAllocator *pa, Partition *queue)
{
    bool was_released = false;
    int32_t area_idx = area_list_get_next_area_idx(queue, 0);
    // find free section.
    // detach all pools/pages/sections.
    while (area_idx != -1) {
        Area *start = area_list_get_area(queue, area_idx);
        was_released |= !partition_allocator_try_release_containers(pa, start);
        area_idx = area_list_get_next_area_idx(queue, area_idx + 1);
    }
    area_idx = area_list_get_next_area_idx(queue, 0);
    while (area_idx != -1) {
        Area *start = area_list_get_area(queue, area_idx);
        was_released |= !partition_allocator_try_free_area(pa, start, queue);
        area_idx = area_list_get_next_area_idx(queue, area_idx + 1);
    }
    return !was_released;
}

static bool partition_allocator_release_single_area_from_queue(PartitionAllocator *pa, Partition *queue)
{
    int32_t area_idx = area_list_get_next_area_idx(queue, 0);
    // find free section.
    // detach all pools/pages/sections.
    while (area_idx != -1) {
        Area *start = area_list_get_area(queue, area_idx);
        if (partition_allocator_try_release_containers(pa, start)) {
            return partition_allocator_try_free_area(pa, start, queue);
        }
        area_idx = area_list_get_next_area_idx(queue, area_idx + 1);
    }
    return false;
}

static bool partition_allocator_release_local_areas(PartitionAllocator *pa)
{
    bool was_released = false;
    for (size_t i = 0; i < 4; i++) {
        if (pa->area[i].area_mask != 0) {
            was_released |= !partition_allocator_release_areas_from_queue(pa, &pa->area[i]);
        }
    }
    return !was_released;
}

static Area *partition_allocator_get_free_area_from_queue(Partition *current_queue)
{
    // the areas are empty
    Area *new_area = NULL;
    Area *previous_area = current_queue->previous_area;
    if (previous_area != NULL) {
        if (!area_is_full(previous_area)) {
            new_area = previous_area;
        }
    } else {
        if (current_queue->area_mask != 0) {
            int32_t area_idx = area_list_get_next_area_idx(current_queue, 0);
            while (area_idx != -1) {
                Area *start = area_list_get_area(current_queue, area_idx);
                if (!area_is_full(start)) {
                    new_area = start;
                    break;
                }
                area_idx = area_list_get_next_area_idx(current_queue, area_idx + 1);
            }
        }
    }
    return new_area;
}

static Partition *partition_allocator_get_current_queue(PartitionAllocator *pa, AreaType t, const size_t s,
                                                        size_t *area_size, size_t *alignement)
{
    if (t == AT_VARIABLE) {
        *area_size = s;
        *alignement = AREA_SIZE_HUGE;
        return &pa->area[AT_VARIABLE];
    } else {
        *area_size = area_type_to_size[t];
        *alignement = *area_size;
        return &pa->area[t];
    }
}

static Partition *partition_allocator_promote_area(PartitionAllocator *pa, AreaType *t, size_t *area_size,
                                                   size_t *alignement)
{
    if (*t > AT_FIXED_128) {
        return NULL;
    } else {
        (*t)++;
        *area_size = area_type_to_size[*t];
        *alignement = *area_size;
        return &pa->area[*t];
    }
}

static Area *partition_allocator_get_free_area(PartitionAllocator *pa, size_t s, AreaType t)
{
    size_t area_size = AREA_SIZE_SMALL;
    size_t alignment = area_size;
    Partition *current_queue = partition_allocator_get_current_queue(pa, t, s, &area_size, &alignment);
    Area *new_area = partition_allocator_get_free_area_from_queue(current_queue);
    while (new_area == NULL && (UINT64_MAX == current_queue->area_mask)) {
        // try releasing an area first.
        bool was_released = partition_allocator_release_single_area_from_queue(pa, current_queue);
        if (was_released) {
            new_area = partition_allocator_get_free_area_from_queue(current_queue);
            if (new_area) {
                break;
            }
        }
        // try promotion
        current_queue = partition_allocator_promote_area(pa, &t, &area_size, &alignment);
        if (current_queue == NULL) {
            return NULL;
        }
        new_area = partition_allocator_get_free_area_from_queue(current_queue);
    }

    if (new_area == NULL) {
        if (s < os_page_size) {
            s = os_page_size;
        }
        new_area = partition_allocator_alloc_area(current_queue, s, alignment);
        if (new_area == NULL) {
            return NULL;
        }
    }

    return new_area;
}
static inline uint32_t partition_allocator_claim_section(Area *area) { return area_claim_section(area); }
static AreaType get_area_type_for_heap(const size_t size)
{
    AreaType at = AT_FIXED_32;
    if (size > MEDIUM_OBJECT_SIZE) {
        if (size <= LARGE_OBJECT_SIZE) {
            at = AT_FIXED_64;
        } else if (size <= HUGE_OBJECT_SIZE) {
            at = AT_FIXED_128;
        } else {
            at = AT_FIXED_256;
        }
    }
    return at;
}

static Section *partition_allocator_alloc_section(PartitionAllocator *pa, const size_t size)
{
    Area *new_area = partition_allocator_get_free_area(pa, size, AT_FIXED_32);
    if (new_area == NULL) {
        return NULL;
    }
    size_t area_size = area_type_to_size[area_get_type(new_area)];
    if (area_size > AREA_SIZE_LARGE) {
        // sections are not supported in areas larger than 128 megs
        if (area_is_free(new_area)) {

            partition_allocator_free_area(pa, new_area);
        }
        return NULL;
    }
    const int32_t section_idx = partition_allocator_claim_section(new_area);
    area_set_container_type(new_area, CT_SECTION);
    Section *section = (Section *)((uint8_t *)new_area + SECTION_SIZE * section_idx);
    section->constr_mask._w32[0] = 0;
    section->active_mask._w32[0] = 0;
    section->idx = section_idx;
    section->partition_mask = new_area->partition_mask;
    return section;
}

static Area *partition_allocator_alloc_area(Partition *area_queue, const uint64_t size, const uint64_t alignment)
{
    Area *new_area = partition_allocator_get_next_area(area_queue, size, alignment);
    if (new_area == NULL) {
        return NULL;
    }
    area_queue->previous_area = new_area;
    return new_area;
}

static cache_align message message_sentinals[MAX_THREADS];
static cache_align message_queue message_queues[MAX_THREADS];
static const int32_t thread_message_imit = 100;
static cache_align Queue pool_queues[MAX_THREADS][POOL_BIN_COUNT];
static cache_align Queue heap_queues[MAX_THREADS][HEAP_TYPE_COUNT];
static cache_align Queue section_queues[MAX_THREADS];
static cache_align PartitionAllocator partition_allocators[MAX_THREADS];
static cache_align int64_t partition_owners[MAX_THREADS];
static cache_align Allocator allocator_list[MAX_THREADS];

// These two partitions have a special function
static Partition partition_ex_0 = {0, ((uintptr_t)32 << 40), ((uintptr_t)64 << 40), AT_VARIABLE, 0, NULL};
static Partition partition_ex_1 = {0, ((uintptr_t)64 << 40), ((uintptr_t)128 << 40), AT_VARIABLE, 0, NULL};

static spinlock partition_lock = {0};
int8_t reserve_any_partition_set(void)
{
    spinlock_lock(&partition_lock);
    int32_t reserved_id = -1;
    for (int i = 0; i < 1024; i++) {
        if (partition_owners[i] == -1) {
            partition_owners[i] = i;
            reserved_id = i;
            break;
        }
    }
    spinlock_unlock(&partition_lock);
    return reserved_id;
}
int8_t reserve_any_partition_set_for(const int32_t midx)
{
    spinlock_lock(&partition_lock);
    int32_t reserved_id = -1;
    for (int i = 0; i < 1024; i++) {
        if (partition_owners[i] == -1) {
            partition_owners[i] = midx;
            reserved_id = i;
            break;
        }
    }
    spinlock_unlock(&partition_lock);
    return reserved_id;
}
bool reserve_partition_set(const int32_t idx, const int32_t midx)
{
    spinlock_lock(&partition_lock);
    if (partition_owners[idx] == -1) {
        partition_owners[idx] = midx;
        return true;
    }
    spinlock_unlock(&partition_lock);
    return false;
}

void release_partition_set(const int32_t idx)
{
    if (idx >= 0) {
        spinlock_lock(&partition_lock);
        partition_owners[idx] = -1;
        spinlock_unlock(&partition_lock);
    }
}

static void allocator_set_cached_pool(Allocator *a, Pool *p)
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
        return ((uintptr_t)p >= a->cached_pool_start) && ((uintptr_t)p <= a->cached_pool_end);
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

void allocator_thread_enqueue(message_queue *queue, message *first, message *last)
{
    atomic_store_explicit(&last->next, (uintptr_t)NULL,
                          memory_order_release); // last.next = null
    message *prev = (message *)atomic_exchange_explicit(&queue->tail, (uintptr_t)last,
                                                        memory_order_release); // swap back and last
    atomic_store_explicit(&prev->next, (uintptr_t)first,
                          memory_order_release); // prev.next = first
}

void allocator_flush_thread_free(Allocator *a)
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

void allocator_thread_free(Allocator *a, void *p, const uint64_t pid)
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

void allocator_free_from_section(Allocator *a, void *p, Section *section, uint32_t part_id)
{
    if (section->type != ST_HEAP_4M) {
        Pool *pool = (Pool *)section_find_collection(section, p);
        pool_free_block(pool, p);
        allocator_set_cached_pool(a, pool);
    } else {
        Heap *heap = (Heap *)&section->collections[0];
        uint32_t heapIdx = area_get_type((Area *)section);
        heap_free(heap, p, false);
        // if the free pools list is empty.
        if (!heap_is_connected(heap)) {
            // reconnect
            PartitionAllocator *_part_alloc = &partition_allocators[part_id];
            Queue *queue = &_part_alloc->heaps[heapIdx];
            if (queue->head != heap && queue->tail != heap) {
                list_enqueue(queue, heap);
            }
        }
    }
    if (!section_is_connected(section)) {
        PartitionAllocator *_part_alloc = &partition_allocators[part_id];
        Queue *sections = _part_alloc->sections;
        if (sections->head != section && sections->tail != section) {
            list_enqueue(sections, section);
        }
    }
}

void allocator_free_from_container(Allocator *a, void *p, const size_t area_size)
{
    Area *area = (Area *)((uintptr_t)p & ~(area_size - 1));
    const uint32_t part_id = area_get_id(area);
    if (a->idx == partition_owners[part_id]) {
        switch (area_get_container_type(area)) {
        case CT_SECTION: {
            Section *section = (Section *)((uintptr_t)p & ~(SECTION_SIZE - 1));
            allocator_free_from_section(a, p, section, part_id);
            break;
        }
        case CT_HEAP: {
            Heap *heap = (Heap *)((uintptr_t)area + sizeof(Area));
            heap_free(heap, p, true);
            // if the pool is disconnected from the queue
            if (!heap_is_connected(heap)) {
                PartitionAllocator *_part_alloc = &partition_allocators[part_id];
                Queue *queue = &_part_alloc->heaps[(area_get_type(area))];
                // reconnect
                if (queue->head != heap && queue->tail != heap) {
                    list_enqueue(queue, heap);
                }
            }
            break;
        }
        default: {
            PartitionAllocator *_part_alloc = &partition_allocators[part_id];
            partition_allocator_free_area(_part_alloc, area);
            break;
        }
        }
    } else {
        allocator_thread_free(a, p, part_id);
    }
}

Section *allocator_get_free_section(Allocator *a, const size_t s, SectionType st)
{
    Section *free_section = (Section *)a->part_alloc->sections->head;

    // find free section.
    while (free_section != NULL) {
        Section *next = free_section->next;
        if (free_section->type == st) {
            if (!section_is_full(free_section)) {
                break;
            } else {
                list_remove(a->part_alloc->sections, free_section);
            }
        }
        free_section = next;
    }

    if (free_section == NULL) {
        Section *new_section = partition_allocator_alloc_section(a->part_alloc, s);
        if (new_section == NULL) {
            return NULL;
        }
        new_section->type = st;

        new_section->next = NULL;
        new_section->prev = NULL;
        list_enqueue(a->part_alloc->sections, new_section);

        free_section = new_section;
    }
    return free_section;
}

void *allocator_alloc_from_heap(Allocator *a, const size_t s)
{
    const uint32_t heap_sizes[] = {1 << HT_4M, 1 << HT_32M, 1 << HT_64M, 1 << HT_128M, 1 << HT_256M};
    const uint32_t heap_size_cls = size_to_heap(s);
    Queue *queue = &a->part_alloc->heaps[heap_size_cls];
    Heap *start = (Heap *)queue->head;
    while (start != NULL) {
        Heap *next = start->next;
        if (heap_has_room(start, s)) {
            return heap_get_block(start, (uint32_t)s);
        } else {
            list_remove(queue, start);
        }
        start = next;
    }

    if (heap_size_cls == 0) {
        Section *new_section = allocator_get_free_section(a, s, ST_HEAP_4M);
        if (new_section != NULL) {
            const unsigned int coll_idx = section_reserve_next(new_section);
            int32_t psize = (1 << size_clss_to_exponent[ST_HEAP_4M]);
            start = (Heap *)section_get_collection(new_section, coll_idx, psize);
            heap_init(start, coll_idx, heap_sizes[0]);
            section_claim_idx(new_section, coll_idx);
            list_enqueue(queue, start);
            return heap_get_block(start, (uint32_t)s);
        }
    }

    AreaType at = get_area_type_for_heap(s);
    Area *new_area = partition_allocator_get_free_area(a->part_alloc, s, at);
    if (new_area == NULL) {
        return NULL;
    }

    uint64_t area_size = area_get_size(new_area);
    area_set_container_type(new_area, CT_HEAP);
    area_reserve_all(new_area);
    start = (Heap *)((uintptr_t)new_area + sizeof(Area));
    heap_init(start, 0, area_size);

    list_enqueue(queue, start);
    return heap_get_block(start, (uint32_t)s);
}

void *allocator_alloc_slab(Allocator *a, const size_t s)
{
    const size_t totalSize = sizeof(Area) + s;
    Area *area = partition_allocator_get_free_area(a->part_alloc, totalSize, AT_FIXED_256);
    if (area == NULL) {
        return NULL;
    }
    area_reserve_all(area);
    area_set_container_type(area, CT_SLAB);
    return (void *)((uintptr_t)area + sizeof(Area));
}

Pool *allocator_alloc_pool(Allocator *a, const uint32_t idx, const uint32_t s)
{
    Section *sfree_section = allocator_get_free_section(a, s, get_pool_size_class(s));
    if (sfree_section == NULL) {
        return NULL;
    }

    const unsigned int coll_idx = section_reserve_next(sfree_section);
    int32_t psize = (1 << size_clss_to_exponent[sfree_section->type]);
    Pool *p = (Pool *)section_get_collection(sfree_section, coll_idx, psize);
    pool_init(p, coll_idx, idx, s, psize);
    section_claim_idx(sfree_section, coll_idx);
    return p;
}

void *allocator_alloc_from_pool(Allocator *a, const size_t s)
{
    const int32_t pool_idx = size_to_pool(s);
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
            allocator_set_cached_pool(a, start);
            return pool_get_free_block(start);
        }
        start = next;
    }

    start = allocator_alloc_pool(a, pool_idx, (uint32_t)s);
    if (start == NULL) {
        return NULL;
    }

    allocator_set_cached_pool(a, start);
    return pool_get_free_block(start);
}

static void _allocator_free(Allocator *a, void *p)
{
    if (allocator_check_cached_pool(a, p)) {
        pool_free_block(a->cached_pool, p);
    } else {
        a->cached_pool = NULL;
        if (partition_from_addr((uintptr_t)p) >= 0) {
            allocator_free_from_container(a, p, area_size_from_addr((uintptr_t)p));
        }
    }
}

static const Allocator default_alloc = {-1, NULL, NULL, {NULL, NULL}, 0, 0, 0};

static __thread Allocator *thread_instance = (Allocator *)&default_alloc;
static Allocator *main_instance = &allocator_list[0];
Allocator *allocator_get_thread_instance(void) { return thread_instance; }

static tls_t _thread_key = (tls_t)(-1);
static void thread_done(void *a)
{
    if (a != NULL) {
        Allocator *alloc = (Allocator *)a;
        release_partition_set((int32_t)alloc->idx);
    }
}

static Allocator *init_thread_instance(void)
{
    int32_t idx = reserve_any_partition_set();
    Allocator *new_alloc = &allocator_list[idx];
    new_alloc->part_alloc = &partition_allocators[idx];
    thread_instance = new_alloc;
    tls_set(_thread_key, new_alloc);
    return new_alloc;
}

void allocator_thread_dequeue_all(Allocator *a, message_queue *queue)
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

static inline void allocator_flush_thread_free_queue(Allocator *a)
{
    message_queue *q = a->part_alloc->thread_free_queue;
    if (q->head != q->tail) {
        allocator_thread_dequeue_all(a, q);
    }
}

static inline void *allocator_try_malloc(Allocator *a, size_t as)
{
    if (as <= LARGE_OBJECT_SIZE) {
        return allocator_alloc_from_pool(a, as);
    } else if (as <= AREA_SIZE_LARGE) {
        // allocate form the large page
        return allocator_alloc_from_heap(a, as);
    } else {
        return allocator_alloc_slab(a, as);
    }
}

void *allocator_malloc(Allocator *a, size_t s)
{
    // align reqested memory size to 8 bytes.
    const size_t as = ALIGN(s);
    // do we have  cached pool to use of a fitting size?
    if (a->cached_pool != NULL) {
        if (as == a->cached_pool->block_size) {
            void *block = pool_aquire_block(a->cached_pool);
            if (block != NULL) {
                return block;
            }
        }
        allocator_unset_cached_pool(a);
    }

    // this also needs attention, so I can wrap up the allocator soon!!
    allocator_flush_thread_free_queue(a);
    // attempt to get the memory requested
    void *ptr = allocator_try_malloc(a, as);

    // fallback if not succesfull
    if (ptr == NULL) {
        // reset caching structs
        a->cached_pool = NULL;
        // try again by fetching a new partition set to use
        const int8_t new_partition_set_idx = reserve_any_partition_set_for((int32_t)a->idx);
        if (new_partition_set_idx != -1) {
            // flush our threaded pools.
            allocator_flush_thread_free(a);
            // move our default partition allocator to the new slot and try again.
            a->part_alloc = &partition_allocators[new_partition_set_idx];
            ptr = allocator_try_malloc(a, as);
        }
    }
    // hopefully this is not NULL
    return ptr;
}

void *allocator_malloc_th(Allocator *a, size_t s)
{
    if (a == &default_alloc) {
        a = init_thread_instance();
    }
    return allocator_malloc(a, s);
}

void *allocator_malloc_heap(Allocator *a, size_t s)
{
    a->cached_pool = NULL;
    allocator_flush_thread_free_queue(a);
    if (s <= AREA_SIZE_LARGE) {
        // allocate form the large page
        return allocator_alloc_from_heap(a, s);
    } else {
        return allocator_alloc_slab(a, s);
    }
}

void allocator_free(Allocator *a, void *p)
{
    if (p == NULL)
        return;
    allocator_flush_thread_free_queue(a);
    _allocator_free(a, p);
}

void allocator_free_th(Allocator *a, void *p)
{
    if (p == NULL)
        return;
    if (a->idx == -1) {
        // free is being called before anythiing has been allocated for this thread.
        // the address is either bogus or belong to some other thread.
        if (partition_from_addr((uintptr_t)p) >= 0) {
            // it is within our address ranges.
            size_t area_size = area_size_from_addr((uintptr_t)p);
            Area *area = (Area *)((uintptr_t)p & ~(area_size - 1));
            const uint32_t part_id = area_get_id(area);
            if (part_id < 1024) {
                // well we are able to read the contents of this memory address.
                // get the idx of the area within its partition.
                if (reserve_partition_set(part_id, 1024)) {
                    // no one had this partition reserved.
                    //
                    Allocator *temp = &allocator_list[part_id];
                    _allocator_free(temp, p);
                    // try and release whatever is in it.
                    allocator_release_local_areas(temp);
                    release_partition_set(part_id);
                } else {
                    // someone has this partition reserved, so we just pluck the
                    // address to its thread free queue.
                    PartitionAllocator *_part_alloc = &partition_allocators[part_id];
                    // if there is a partition allocator that owns this, we can just
                    // push it on to its queue.
                    message *new_free = (message *)p;
                    new_free->next = (uintptr_t)0;
                    allocator_thread_enqueue(_part_alloc->thread_free_queue, new_free, new_free);
                }
            }
        }
    } else {
        allocator_free(a, p);
    }
}

bool allocator_release_local_areas(Allocator *a)
{
    a->cached_pool = NULL;
    bool result = false;
    const uint8_t midx = a->idx;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (partition_owners[i] == midx) {
            PartitionAllocator *palloc = &partition_allocators[i];
            allocator_thread_dequeue_all(a, palloc->thread_free_queue);
            bool was_released = partition_allocator_release_local_areas(palloc);
            if (was_released) {
                palloc->sections->head = NULL;
                palloc->sections->tail = NULL;

                for (int j = 0; j < HEAP_TYPE_COUNT; j++) {
                    palloc->heaps[j].head = NULL;
                    palloc->heaps[j].tail = NULL;
                }

                for (int j = 0; j < POOL_BIN_COUNT; j++) {
                    if (palloc->pools[j].head != NULL || palloc->pools[j].tail != NULL) {
                        palloc->pools[j].head = NULL;
                        palloc->pools[j].tail = NULL;
                    }
                }
            }

            if (midx != i && was_released) {
                release_partition_set(i);
            }
            result |= !was_released;
        }
    }
    a->part_alloc = &partition_allocators[midx];
    return !result;
}

static inline uintptr_t get_thread_id(void)
{
#if defined(WINDOWS)
    return (uintptr_t)NtCurrentTeb();
#elif defined(__GNUC__)
    void *res;
    const size_t ofs = 0;
#if defined(__APPLE__)
#if defined(__x86_64__)
    __asm__("movq %%gs:%1, %0" : "=r"(res) : "m"(*((void **)ofs)) :);
#elif defined(__aarch64__)
    void **tcb;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tcb));
    tcb = (void **)((uintptr_t)tcb & ~0x07UL);
    res = *tcb;
#endif
#elif defined(__x86_64__)
    __asm__("movq %%fs:%1, %0" : "=r"(res) : "m"(*((void **)ofs)) :);
#endif
    return (uintptr_t)res;
#else
    return (uintptr_t)&thread_instance;
#endif
}

uintptr_t main_thread_id = 0;
static void allocator_init(size_t max_threads)
{
    static bool init = false;
    if (init)
        return;
    init = true;

    main_thread_id = get_thread_id();
    thread_instance = &allocator_list[0];
    os_page_size = get_os_page_size();

    tls_create(&_thread_key, &thread_done);
    for (size_t i = 0; i < max_threads; i++) {
        Queue *pool_base = pool_queues[i];
        for (int j = 0; j < POOL_BIN_COUNT; j++) {
            pool_base[j].head = NULL;
            pool_base[j].tail = NULL;
        }
    }

    for (size_t i = 0; i < max_threads; i++) {
        Queue *heap_base = heap_queues[i];
        for (size_t j = 0; j < HEAP_TYPE_COUNT; j++) {
            heap_base[j].head = NULL;
            heap_base[j].tail = NULL;
        }
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
        message_base->head = (uintptr_t)&message_sentinals[i];
        message_base->tail = (uintptr_t)&message_sentinals[i];
    }

    for (size_t i = 0; i < max_threads; i++) {
        partition_owners[i] = -1;
    }
    for (size_t i = 0; i < max_threads; i++) {
        PartitionAllocator *palloc = &partition_allocators[i];
        size_t size = (SZ_GB * 2);
        size_t offset = ((size_t)2 << 40);
        uint32_t area_type = 0;
        for (size_t j = 0; j < 4; j++) {
            palloc->area[j].partition_id = (uint32_t)i;
            palloc->area[j].start_addr = (i)*size + offset;
            palloc->area[j].end_addr = palloc->area[j].start_addr + size;
            palloc->area[j].type = area_type;
            palloc->area[j].area_mask = 0;
            palloc->area[j].previous_area = NULL;
            size *= 2;
            offset *= 2;
            area_type++;
        }

        palloc->sections = &section_queues[i];
        palloc->heaps = heap_queues[i];
        palloc->pools = pool_queues[i];
        palloc->thread_messages = NULL;
        palloc->message_count = 0;
        palloc->thread_free_queue = &message_queues[i];
    }
    for (size_t i = 0; i < max_threads; i++) {
        allocator_list[i].idx = (int32_t)i;
        allocator_list[i].part_alloc = NULL;
        allocator_list[i].thread_free_part_alloc = NULL;
        allocator_list[i].partition_allocators.head = NULL;
        allocator_list[i].partition_allocators.tail = NULL;
        allocator_list[i].cached_pool = NULL;
        allocator_list[i].cached_pool_start = 0;
        allocator_list[i].cached_pool_end = 0;
    }
    // reserve the first allocator for the main thread.
    partition_owners[0] = 0;
    allocator_list[0].part_alloc = &partition_allocators[0];
}

static void allocator_destroy()
{
    static bool done = false;
    if (done)
        return;
    done = true;

    tls_set(_thread_key, NULL);
    tls_delete(_thread_key);
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

void *cmalloc(size_t s)
{
    if (get_thread_id() == main_thread_id) {
        return allocator_malloc(main_instance, s);
    } else {
        return allocator_malloc_th(thread_instance, s);
    }
}

void cfree(void *p)
{
    if (get_thread_id() == main_thread_id) {
        allocator_free(main_instance, p);
    } else {
        allocator_free_th(thread_instance, p);
    }
}
