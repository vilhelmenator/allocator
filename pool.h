
#ifndef POOL_H
#define POOL_H

#include "../cthread/cthread.h"
#include "section.h"

static const int32_t pool_sizes[] = {
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

static const int32_t pool_recips[] = {
    536870912, 268435456, 178956971, 134217728, 107374183, 89478486, 76695845,
    67108864,  59652324,  53687092,  48806447,  44739243,  41297763, 38347923,
    35791395,  33554432,  29826162,  26843546,  24403224,  22369622, 20648882,
    19173962,  17895698,  16777216,  14913081,  13421773,  12201612, 11184811,
    10324441,  9586981,   8947849,   8388608,   7456541,   6710887,  6100806,
    5592406,   5162221,   4793491,   4473925,   4194304,   3728271,  3355444,
    3050403,   2796203,   2581111,   2396746,   2236963,   2097152,  1864136,
    1677722,   1525202,   1398102,   1290556,   1198373,   1118482,  1048576,
    932068,    838861,    762601,    699051,    645278,    599187,   559241,
    524288,    466034,    419431,    381301,    349526,    322639,   299594,
    279621,    262144,    233017,    209716,    190651,    174763,   161320,
    149797,    139811,    131072,    116509,    104858,    95326,    87382,
    80660,     74899,     69906,     65536,     58255,     52429,    47663,
    43691,     40330,     37450,     34953,     32768,     29128,    26215,
    23832,     21846,     20165,     18725,     17477,     16384,    14564,
    13108,     11916,     10923,     10083,     9363,      8739,     8192,
    7282,      6554,      5958,      5462,      5042,      4682,     4370,
    4096,      3641,      3277,      2979,      2731,      2521,     2341,
    2185,      2048,      1821,      1639,      1490,      1366,     1261,
    1171,      1093,      4194304,   4718592};

static inline int32_t get_pool_size_class(size_t s)
{
    if (s <= SMALL_OBJECT_SIZE) {         // 8 - 16k
        return ST_POOL_128K;              // 128k
    } else if (s <= MEDIUM_OBJECT_SIZE) { // 16k - 128k
        return ST_POOL_512K;              // 512k
    } else {
        return ST_POOL_4M; // 4M for > 128k objects.
    }
}

static inline uint8_t size_to_pool(const size_t as)
{
    static const int bmask = ~0x7f;
    if ((bmask & as) == 0) {
        // the first 2 rows
        return (as >> 3) - 1;
    } else {
        int32_t lz = __builtin_clzll(as);
        const uint32_t top_mask = UINT32_MAX;
        const uint64_t bottom_mask = (top_mask >> (lz + 4));
        const uint64_t incr = (bottom_mask & as) > 0;
        const size_t row = (26 - lz) << 3;
        return (row + ((as >> (28 - lz)) & 0x7)) + incr - 1;
    }
}

static inline void pool_post_free(Pool *p)
{
    Section *section = (Section *)((uintptr_t)p & ~(SECTION_SIZE - 1));
    section_free_idx(section, p->idx);
}

static inline void pool_post_reserved(Pool *p)
{
    Section *section = (Section *)((uintptr_t)p & ~(SECTION_SIZE - 1));
    section_reserve_idx(section, p->idx);
}

bool pool_thread_free_block(Pool* p, void* block)
{
    uintptr_t base_addr = (uintptr_t)p + sizeof(Pool);
    if((uintptr_t)block < base_addr)
    {
        return false;
    }
    if((uintptr_t)block >= (base_addr + p->num_available*p->block_size))
    {
        return false;
    }
    uint32_t idx = (uint32_t)(((size_t)((uint8_t *)block - base_addr) * p->block_recip) >> 32);
    *(int32_t*)block = -1;
    // swap tail and new memory
    int32_t prev = atomic_exchange_explicit(&p->thread_free.tail, idx, memory_order_release);
    if(prev == -1)
    {
        // there was no previous message
        return true;
    }
    AtomicIndexMessage* msg_prev = (AtomicIndexMessage*)(void*)(base_addr + (prev * p->block_size));
    // connect the new memory to the queue
    atomic_store_explicit(&msg_prev->next, idx, memory_order_release); // prev.next = first
    return true;
}

void pool_move_thread_free(Pool* p)
{
    int32_t back = (int32_t )atomic_load_explicit(&p->thread_free.tail, memory_order_relaxed);
    int32_t curr = (int32_t )p->thread_free.head;
    if(curr == back)
    {
        return;
    }
    if(curr == -1)
    {
        // only one item in the queue.
        p->thread_free.head = back;
        return;
    }
    int32_t tail = -1;
    uint8_t* base_addr = (uint8_t *)p + sizeof(Pool);
    int32_t count = 0;
    // loop between start and end addres
    while (curr != back) {
        int32_t next_idx = *(int32_t*)(base_addr + (curr * p->block_size));
        if (next_idx == -1)
            break;
        tail = curr;
        curr = next_idx;
        count++;
    }
    
    *(int32_t*)(base_addr + (tail * p->block_size)) = p->free;
    p->free = p->thread_free.head;
    p->thread_free.head = back;
    p->num_used -= count;
}

void pool_collect_thread_free(Pool* p)
{
    if(p->num_used > 0)
    {
        pool_move_thread_free(p);
        if(p->num_used == 1)
        {
            // the thread free collection always leaves one behind
            p->num_used = 0;
            pool_post_free(p);
            // the last piece was returned so make the first item the start of the free
            p->free = 0;
            *(int32_t*)((uintptr_t)p + sizeof(Pool)) = -1;
            p->thread_free = (AtomicIndexQueue){-1, -1};
            p->num_committed = 1;
        }
    }
}
static inline void *pool_extend(Pool *p)
{
    p->num_used++;
    return ((uint8_t *)p + sizeof(Pool) + (p->num_committed++ * p->block_size));
}

static inline bool pool_is_empty(const Pool *p) { return p->num_used >= p->num_available; }
static inline bool pool_is_full(const Pool *p) { return p->num_used == 0; }
static inline bool pool_is_fully_commited(const Pool *p) { return p->num_committed >= p->num_available; }
static inline int32_t pool_get_remaining_contiguous_range(const Pool *p) {
    switch(p->type)
    {
        case PT_INDEXED:
        {
            return p->num_available - p->num_committed;
        }
        case PT_MASKED:
        {
            PoolM* pm = (PoolM*)p;
            return __builtin_ctzll(1|pm->allocation_mask);
        }
        default:
        {
            PoolC*pc = (PoolC*)p;
            return pc->num_available - pc->num_used;
        }
    };
}

static inline void pool_alloc_remaining_contiguous_range(Pool *p) {
    switch(p->type)
    {
        case PT_INDEXED:
        {
            p->num_used = p->num_available;
            p->num_committed = p->num_available;
            break;
        }
        case PT_MASKED:
        {
            PoolM* pm = (PoolM*)p;
            pm->allocation_mask = UINT64_MAX;
            break;
        }
        default:
        {
            PoolC*pc = (PoolC*)p;
            p->num_used = p->num_available;
            pc->allocation_counter = p->num_available;
            break;
        }
    };
}

static inline void pool_free_contiguous_range(Pool *p, int32_t range) {
    switch(p->type)
    {
        case PT_INDEXED:
        {
            p->num_used -= range;
            p->num_committed -= range;
            break;
        }
        case PT_MASKED:
        {
            PoolM* pm = (PoolM*)p;
            pm->allocation_mask &= ~((1ULL << (range + 1)) - 1);
            break;
        }
        default:
        {
            PoolC*pc = (PoolC*)p;
            size_t delta = pc->num_available - pc->allocation_counter;
            pc->allocation_counter -= range;
            pc->num_used -= (range + delta);
            break;
        }
    };
}

static inline void pool_free_block(void *_p, void *block)
{
    Pool* p = (Pool*)_p;
    if (--p->num_used == 0) {
        pool_post_free(p);
        // the last piece was returned so make the first item the start of the free
        p->free = 0;
        *(int32_t*)((uintptr_t)p + sizeof(Pool)) = -1;
        p->num_committed = 1;
        return;
    }
    
    uintptr_t base_addr = (uintptr_t)p + sizeof(Pool);
    uint32_t idx = (uint32_t)(((size_t)((uint8_t *)block - base_addr) * p->block_recip) >> 32);
    *(uint32_t *)block = p->free;
    p->free = idx;
}

static inline void *pool_get_free_block(Pool *p)
{
    uint8_t* base_addr = (uint8_t *)p + sizeof(Pool);
    if (p->num_used++ == 0) {
        pool_post_reserved(p);
        p->free = -1;
        return base_addr;
    }
    
    uint32_t next_idx = *(uint32_t*)(base_addr + (p->free * p->block_size));
    int32_t cur_idx = p->free;
    p->free = next_idx >= p->num_available? -1: next_idx;
    return (void*)(base_addr + (cur_idx * p->block_size));
}

static inline void *pool_aquire_block(void *_p)
{
    Pool* p = (Pool*)_p;
    if (p->free != -1) {
       return pool_get_free_block(p);
    }
    if (!pool_is_fully_commited(p)) {
        return pool_extend(p);
    }
    else
    {
        pool_move_thread_free(p);
        if(p->free != -1)
        {
            return pool_get_free_block(p);
        }
    }
    return NULL;
}

static inline void *pool_aquire_block_c(void *_p)
{
    if(!pool_is_empty((Pool*)_p))
    {
        PoolC* p = (PoolC*)_p;
        if (p->num_used++ == 0) {
            pool_post_reserved((Pool*)_p);
            return (uint8_t *)p + sizeof(PoolC);
        }
        
        return ((uint8_t *)p + sizeof(PoolC) + (p->allocation_counter++ * p->block_size));
    }
    return NULL;
}

static inline void pool_free_block_c(void *_p, void* b)
{
    PoolC* p = (PoolC*)_p;
    if(--p->allocation_counter == 0)
    {
        pool_post_free((Pool*)_p);
        p->num_used = 0;
        p->allocation_counter = 1;
    }
}
static void pool_init(Pool *p, const int8_t pidx, const uint32_t block_idx, const int32_t psize, size_t ps)
{
    void *blocks = (uint8_t *)p + sizeof(Pool);
    const size_t block_memory = psize - sizeof(Pool);
    const uintptr_t section_end = ((uintptr_t)p + (SECTION_SIZE - 1)) & ~(SECTION_SIZE - 1);
    const size_t remaining_size = section_end - (uintptr_t)blocks;
    p->idx = pidx;
    p->block_idx = block_idx;
    p->block_size = pool_sizes[block_idx];
    p->block_recip = pool_recips[p->block_idx];
    p->num_available = (int32_t)((MIN(remaining_size, block_memory) * p->block_recip) >> 32);
    p->num_committed = 1;
    p->free_fn = &pool_free_block_c;
    p->alloc_fn = &pool_aquire_block_c;
    p->type = PT_COUNTER;
    p->num_used = 0;
    p->next = NULL;
    p->prev = NULL;
    p->free = 0;
    p->thread_free = (AtomicIndexQueue){-1, -1};
    *(int32_t*)((uint8_t *)p + sizeof(Pool)) = -1;
}


#endif
