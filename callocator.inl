
#ifndef callocator_inl
#define callocator_inl
#include "callocator.h"
//#define ARENA_PATH

#if defined(_MSC_VER)
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif

#define SZ_KB 1024ULL
#define SZ_MB (SZ_KB * SZ_KB)
#define SZ_GB (SZ_MB * SZ_KB)

#define DEFAULT_OS_PAGE_SIZE 4096ULL
#define SECTION_SIZE (1ULL << 22ULL)

#define BASE_AREA_SIZE SECTION_SIZE 
#define AREA_SIZE_SMALL (BASE_AREA_SIZE * 8ULL) // 32Mb
#define AREA_SIZE_MEDIUM (SECTION_SIZE * 16ULL) // 64Mb
#define AREA_SIZE_LARGE (SECTION_SIZE * 32ULL)  // 128Mb
#define AREA_SIZE_HUGE (SECTION_SIZE * 64ULL)   // 256Mb
#define NUM_AREA_PARTITIONS 7

#define SMALL_OBJECT_SIZE DEFAULT_OS_PAGE_SIZE * 4 // 16k
#define MEDIUM_OBJECT_SIZE SMALL_OBJECT_SIZE * 8   // 128kb
#define LARGE_OBJECT_SIZE MEDIUM_OBJECT_SIZE * 16  // 2Mb
#define HUGE_OBJECT_SIZE LARGE_OBJECT_SIZE * 16    // 32Mb

#define POOL_BIN_COUNT 135
#define HEAP_TYPE_COUNT 5
#define ARENA_BIN_COUNT (18*6)
#define MAX_ARES 64
#define MAX_THREADS 4096


#define MAX(x, y) (x ^ ((x ^ y) & -(x < y)))
#define MIN(x, y) (y ^ ((x ^ y) & -(x < y)))
#define POWER_OF_TWO(x) (x && !(x & (x - 1)))
#define SIGN(x) ((x > 0) - (x < 0))
#define CACHE_LINE 64
#if defined(WINDOWS)
#define cache_align __declspec(align(CACHE_LINE))
#else
#define cache_align __attribute__((aligned(CACHE_LINE)))
#endif
#define WSIZE (sizeof(intptr_t))
#define ALIGN_UP_2(x, y) ((((uintptr_t)x) + (((uintptr_t)y) - 1)) & ~(((uintptr_t)y) - 1))
#define ALIGN_DOWN_2(x, y) (((uintptr_t)x) & ~(((uintptr_t)y) - 1))
#define ALIGN(x) ALIGN_UP_2(x, sizeof(intptr_t))
#define ALIGN4(x) ALIGN_UP_2(x, 4)
#define ALIGN_CACHE(x) (((x) + CACHE_LINE - 1) & ~(CACHE_LINE - 1))
#define ADDR_START (1ULL << 39)
#define BASE_ADDR(idx) (((1ULL << 40) << (idx)))
 
 
static size_t os_page_size = DEFAULT_OS_PAGE_SIZE;
typedef enum AreaType_t {
    AT_FIXED_4 = 0,
    AT_FIXED_8 = 1,
    AT_FIXED_16 = 2,
    AT_FIXED_32 = 3,  //  small allocations, mostly pools.
    AT_FIXED_64 = 4,  //
    AT_FIXED_128 = 5, //  larger allocations, but can also contain pools and sections.
    AT_FIXED_256 = 6, //  heap allocations only
    AT_VARIABLE = 7   //  not a fixed size area. found in extended partitions.
} AreaType;

static const uintptr_t partition_type_to_exponent[] = {
    28, // 256 MB
    29, // 512 MB
    30, // 1024 MB
    31, // 2048 MB
    32, // 4096 MB
    33, // 8192 MB
    34, // 16384 MB
};

static const uintptr_t area_type_to_size[] = {
    AREA_SIZE_SMALL>>3, AREA_SIZE_SMALL>>2, AREA_SIZE_SMALL>>1,
    AREA_SIZE_SMALL, AREA_SIZE_MEDIUM, AREA_SIZE_LARGE,
    AREA_SIZE_HUGE, UINT64_MAX};

static inline uint64_t area_size_from_partition_id(uint8_t pid) { return area_type_to_size[pid]; }

static inline int8_t partition_from_addr(uintptr_t p)
{
    // 4, 8, 16, 32, 64, 128, 256
    static const uint8_t partition_count = 9;
    const int lz = 23 - __builtin_clz(p >> 32);
    if (lz < 0 || lz > partition_count) {
        return -1;
    } else {
        return lz;
    }
}

static inline int8_t partition_allocator_from_addr_and_part(uintptr_t p, int8_t at)
{
    size_t offset = BASE_ADDR(at);
    const ptrdiff_t diff = (uint8_t *)p - (uint8_t *)offset;
    return (uint32_t)(((size_t)diff) >> partition_type_to_exponent[at]);
}

static inline int8_t partition_allocator_from_addr(uintptr_t p)
{
    int8_t at = partition_from_addr(p);
    return partition_allocator_from_addr_and_part(p, at);
}

static inline uint64_t area_size_from_addr(uintptr_t p) { return area_size_from_partition_id(partition_from_addr(p)); }

typedef enum ContainerType_e { CT_SECTION = 16, CT_HEAP = 32, CT_SLAB = 64 } ContainerType;
typedef enum SectionType_e { ST_HEAP_4M = 0, ST_POOL_128K = 1, ST_POOL_512K = 2, ST_POOL_4M = 3 } SectionType;
typedef enum HeapType_e { HT_4M = 22, HT_32M = 25, HT_64M = 26, HT_128M = 27, HT_256M = 28 } HeapType;

typedef union Bitmask_u
{
    uint64_t whole;
    uint32_t _w32[2];
} Bitmask;

typedef struct Block_t
{
    struct Block_t* next;
} Block;

typedef struct Queue_t
{
    void* head;
    void* tail;
} Queue;

typedef struct IndexQueue_t
{
    uint32_t head;
    uint32_t tail;
} IndexQueue;

// lockless message queue
typedef struct AtomicMessage_t
{
    _Atomic(uintptr_t) next;
} AtomicMessage;

typedef struct AtomicQueue_t
{
    uintptr_t head;
    _Atomic(uintptr_t) tail;
} AtomicQueue;

typedef struct AtomicIndexMessage_t
{
    _Atomic(int32_t) next;
} AtomicIndexMessage;

typedef struct AtomicIndexQueue_t
{
    int32_t head;
    _Atomic(int32_t) tail;
} AtomicIndexQueue;

typedef struct Area_t
{
    uint64_t partition_mask; // id, container type, area_type, num
    Bitmask constr_mask;     // containers that have been constructed.
    Bitmask active_mask;     // containers that cant be destructed.
} Area;

typedef struct Section_t
{
    uint64_t partition_mask; // area inherited values
    Bitmask constr_mask;     // which parts have been touched
    Bitmask active_mask;     // which parts are being used

    SectionType type;
    int32_t idx;

    // links to sections.
    struct Section_t *prev;
    struct Section_t *next;

} Section;


typedef struct Heap_t
{
    Block* free;
    AtomicQueue thread_free;
    //
    void* thread_i;
    void *prev;
    void *next;
} Heap;

typedef struct HeapEx_t
{
    Block* free;
    AtomicQueue thread_free;
    //
    void* thread_i;
    void *prev;
    void *next;
} HeapEx;

static inline void init_heap(Heap*f)
{
    f->free = NULL;
    f->thread_i = NULL;
    f->thread_free = (AtomicQueue){(uintptr_t)&f->thread_i,(uintptr_t)&f->thread_i};
    f->prev = NULL;
    f->next = NULL;
}

void deferred_move_thread_free(Heap* d);
void deferred_thread_enqueue(AtomicQueue *queue, AtomicMessage *first, AtomicMessage *last);

typedef struct Pool_t
{
    // 56 byte header
    Block* deferred_free;
    AtomicQueue thread_free;
    void* _d;
    void *prev;
    void *next;
    
    // 32 byte body
    int32_t idx;        // index in the parent section
    uint32_t block_idx; // index into the pool queue. What size class do you belong to.
    
    uint32_t block_size;
    int32_t num_used;
    
    int32_t num_committed;
    int32_t num_available;

    uint32_t alignment;
    
    Block* free;
} Pool;

typedef struct ImplicitList_t
{
    // 56 byte header
    Block* deferred_free;
    AtomicQueue thread_free;
    void* _d;
    void *prev;
    void *next;
    
    int32_t idx;           // index into the parent section/if in a section.
    uint32_t total_memory; // how much do we have available in total
    uint32_t used_memory;  // how much have we used
    uint32_t min_block;    // what is the minum size block available;
    uint32_t max_block;    // what is the maximum size block available;
    uint32_t num_allocations;

    Queue free_nodes;

} ImplicitList;

typedef struct HeapBlock_t
{
    uint8_t *data;
} HeapBlock;

typedef struct QNode_t
{
    void* prev;
    void* next;
} QNode;

typedef struct QIndexNode_t
{
    uint32_t prev;
    uint32_t next;
} QIndexNode;

typedef struct Arena_t
{
    uint32_t partition_id;
    uint8_t container_exponent;
} Arena; // 8 bytes

typedef struct ArenaAllocation_t
{
    Arena *arena;
    int32_t tidx; // top level idx
    int32_t midx; // mid level idx
    int32_t bidx; // bottom level idx
} ArenaAllocation; // 16 bytes

typedef enum ArenaLevel_t
{
    AL_LOW = 0,
    AL_MID = 1,
    AL_HIGH = 2
} ArenaLevel;

typedef struct Arena_L2_t
{
    Block* deferred_free;
    AtomicQueue thread_free;
    void* _d;
    // 32
    void *prev;
    void *next;
    
    // 64
    uint64_t  L0_allocations;   // base allocations here at the root
    uint64_t  L0_ranges;        // size of allocations at the root.

    uint64_t  L1_allocations;   // base allocations here at the root
    uint64_t  L1_ranges;        // sizes of allocations at the root
    uint64_t  L1_zero;          // have the L0 headers been zeroed at the root 64th part.
    
    // L1 - 64 bytes
    uint64_t  L2_allocations;   // base allocations for largest element
    uint64_t  L2_ranges;        // sizes of allocations.
    uint64_t  L2_zero;          // have the l2 headers been zeroed at each 64th part
    
} Arena_L2; // 136 bytes

typedef struct Arena_L1_t
{
    Block* deferred_free;
    AtomicQueue thread_free;
    void* _d;
    void *prev;
    void *next;

    // 64
    uint64_t  L0_allocations;   
    uint64_t  L0_ranges;
    
    uint64_t  L1_allocations;
    uint64_t  L1_ranges;
    uint64_t  L1_zero;
} Arena_L1; // 112

typedef struct Arena_L0_t
{
    Block* deferred_free;
    AtomicQueue thread_free;
    void* _d;
    void *prev;
    void *next;
    
    uint64_t  L0_allocations;
    uint64_t  L0_ranges;
    
} Arena_L0; // 88 bytes


typedef struct Partition_t
{
    uint64_t area_mask;     // which parsts have been allocated
    uint64_t range_mask;    // the extends for each part
    uint64_t zero_mask;     // which parts have been initilized.
    uint64_t full_mask;     // which parts have free internal memory
} Partition;

typedef void (*free_func)(void *);
typedef struct PartitionAllocator_t
{
    int64_t idx;
    Partition area[7];
    uint64_t previous_partitions;
    // sections local to this thread with free heaps or pools
    Queue *sections;
    // free pages that have room for various size allocations.
    Queue *heaps;
    
    // free pools of various sizes.
    Queue *pools;
    // 18 size classes. 6 per size class for remaining block exponent. 1,2,4,8,16,32
    Queue *aligned_z_cls;
    Queue *aligned_cls;
    
    // collection of messages for other threads
    AtomicMessage *thread_messages;
    uint32_t message_count; // how many threaded message have we acccumuated for passing out
    // a queue of messages from other threads.
    AtomicQueue *thread_free_queue;
    struct PartitionAllocator_t *prev;
    struct PartitionAllocator_t *next;

} PartitionAllocator;

typedef enum cache_type_t
{
    CACHE_POOL,
    CACHE_HEAP,
    CACHE_SLAB,
    CACHE_ARENA,
} cache_type;

typedef struct alloc_cache_t
{
    uintptr_t header;
    int32_t end;
    int32_t start;
    int32_t block_size;
    int32_t alignment;
    int32_t counter;
    int32_t req_size;
    cache_type cache_type;
} alloc_cache;

typedef struct deferred_cache_t
{
    Queue items;
    uintptr_t start;
    uintptr_t end;
    uint32_t owned;
    uint32_t num;
} deferred_cache;


static inline void deferred_cache_enqueue( deferred_cache*c, Heap* dl)
{
    if(c->owned)
    {
        ((Block*)c->items.tail)->next = dl->free;
        dl->free = c->items.head;
    }
    else
    {
        deferred_thread_enqueue(&dl->thread_free, c->items.head, c->items.tail);
    }
    c->items.head = NULL;
    c->items.tail = NULL;
}

static inline void deferred_cache_add(deferred_cache*c, void* p)
{
    ((Block*)p)->next = c->items.head;
    c->items.head =  p;
    c->num++;
}

typedef struct Allocator_t
{
    int32_t idx;
    uint32_t prev_size;
    PartitionAllocator *part_alloc;
    
    // per allocator lookup structures
    alloc_cache c_cache;        // allocation cache structure.
    deferred_cache c_deferred;  // release cache structure.
    
    PartitionAllocator *thread_free_part_alloc;
    Queue partition_allocators;
} Allocator;

void deferred_cache_init(Allocator* a, void*p);
void deferred_cache_release(Allocator* a, void* p);

// list utilities
static inline bool qnode_is_connected(QNode* n)
{
    return (n->prev != 0) || (n->next != 0);
}
// list utilities
static inline bool qnode_indsex_is_connected(QIndexNode* n)
{
    return (n->prev != 0) || (n->next != 0);
}

static inline void _list_enqueue(void *queue, void *node, size_t head_offset, size_t prev_offset)
{
    Queue *tq = (Queue *)((uint8_t *)queue + head_offset);
    if (tq->head != 0) {
        QNode *tn = (QNode *)((uint8_t *)node + prev_offset);
        tn->next = tq->head;
        tn->prev = 0;
        QNode *temp = (QNode *)((uint8_t *)tq->head + prev_offset);
        temp->prev = tq->head = node;
    } else {
        tq->tail = tq->head = node;
    }
}

void _list_remove(void *queue, void* node, size_t head_offset, size_t prev_offset);
#define list_enqueue(q, n) _list_enqueue(q, n, offsetof(__typeof__(*q), head), offsetof(__typeof__(*n), prev))
#define list_remove(q, n) _list_remove(q, n, offsetof(__typeof__(*q), head), offsetof(__typeof__(*n), prev))
static inline void list_enqueueIndex(void *queue, void *node, void*base)
{
    IndexQueue *tq = (IndexQueue *)queue;
    if (tq->head != 0xFFFFFFFF) {
        QIndexNode *tn = (QIndexNode *)node;
        tn->next = tq->head;
        tn->prev = 0xFFFFFFFF;
        QIndexNode *temp = (QIndexNode *)((uint8_t *)base + tq->head);
        temp->prev = tq->head = (uint32_t)((uint64_t)base - (uint64_t)node);
    } else {
        tq->tail = tq->head = (uint32_t)((uint64_t)base - (uint64_t)node);
    }
}
void list_remove32(void *queue, void* node, void* base);

static inline uint64_t compressMask(uint64_t mask_a, int32_t exp)
{
    uint64_t submask = (1ULL << (exp + 1)) - 1;
    uint64_t width = 1ULL << exp;
    uint32_t steps = 64 >> exp;
    uint64_t new_mask = ~((1ULL << steps) - 1);
    for(int i = 0; i < steps; i++)
    {
        int8_t on = ((submask & (mask_a >> i*width)) == submask);
        new_mask |= (on << i);
    }
    return new_mask;
};

static inline int32_t find_first_nones(uintptr_t x, int64_t n, uint32_t step_exp)
{
    if(n > 64)
    {
        return -1;
    }
    
    int64_t s;
    if(step_exp > 0)
    {
        uint32_t width = 1U << step_exp;
        x = compressMask(x, step_exp);
        while (n > 1) {
            s = n >> 1;
            x = x & (x >> s);
            n = n - s;
        }
        return x == 0 ? -1 : __builtin_ctzll(x) * width;
    }
    else
    {
        while (n > 1) {
            s = n >> 1;
            x = x & (x >> s);
            n = n - s;
        }
    }
    return x == 0 ? -1 : __builtin_ctzll(x);
}

static inline int32_t find_first_nzeros(uintptr_t x, int64_t n, int32_t exp) { return find_first_nones(~x, n, exp); }
static inline int32_t get_next_mask_idx(uint64_t mask, uint32_t cidx)
{
    uint64_t msk_cpy = mask >> cidx;
    if (msk_cpy == 0 || (cidx > 63)) {
        return -1;
    }
    return __builtin_ctzll(msk_cpy) + cidx;
}
static inline uint64_t apply_range(uint32_t range, uint32_t at)
{
    if(range == 1)
    {
        return 0;
    }
    
    return (1ULL << at) | (1ULL << (at + (range - 1)));
}

static inline uint32_t get_range(uint32_t at, uint64_t mask)
{
    if(mask == 0)
    {
        return 1;
    }
    if ((mask & (1ULL << at)) == 0)
    {
        return 1;
    }
    return __builtin_ctzll(mask >> (at + 1)) + 2;
}

#endif /* callocator_inl */
