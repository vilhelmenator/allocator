
#ifndef callocator_inl
#define callocator_inl
#include "callocator.h"

#if defined(_MSC_VER)
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif

#define CACHE_LINE 64
#if defined(WINDOWS)
#define cache_align __declspec(align(CACHE_LINE))
#else
#define cache_align __attribute__((aligned(CACHE_LINE)))
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
#define NUM_AREA_PARTITIONS 9

#define SMALL_OBJECT_SIZE DEFAULT_OS_PAGE_SIZE * 4 // 16k
#define MEDIUM_OBJECT_SIZE SMALL_OBJECT_SIZE * 8   // 128kb
#define LARGE_OBJECT_SIZE MEDIUM_OBJECT_SIZE * 16  // 2Mb
#define HUGE_OBJECT_SIZE LARGE_OBJECT_SIZE * 16    // 32Mb

#define ARENA_LEVELS 3
#define POOL_BIN_COUNT 80
#define HEAP_TYPE_COUNT 5
#define ARENA_SBIN_COUNT 7 // 1,2,4,8,16,32

#define ARENA_BIN_COUNT (NUM_AREA_PARTITIONS*ARENA_LEVELS*ARENA_SBIN_COUNT)
#define MAX_ARES 64
#define MAX_THREADS 1024
#define MIN_BLOCKS_PER_COUNTER_ALLOC 8

#define MAX(x, y) (x ^ ((x ^ y) & -(x < y)))
#define MIN(x, y) (y ^ ((x ^ y) & -(x < y)))
#define POWER_OF_TWO(x) (x && !(x & (x - 1)))
#define SIGN(x) ((x > 0) - (x < 0))

#define WSIZE (sizeof(intptr_t)/2)
#define DSIZE WSIZE*2

#define ALIGN_UP_2(x, y) ((((uintptr_t)x) + (((uintptr_t)y) - 1)) & ~(((uintptr_t)y) - 1))
#define ALIGN_DOWN_2(x, y) (((uintptr_t)x) & ~(((uintptr_t)y) - 1))
#define ALIGN(x) ALIGN_UP_2(x, sizeof(intptr_t))
#define ALIGN4(x) ALIGN_UP_2(x, 4)
#define ALIGN_CACHE(x) (((x) + CACHE_LINE - 1) & ~(CACHE_LINE - 1))

#define ADDR_START (1ULL << 39)
#define BASE_ADDR(idx) ((1ULL << 40ULL) + ((1ULL << 38ULL) << (idx)))
#define DEFAULT_ALIGNMENT sizeof(intptr_t)
#define NEXT_POWER_OF_TWO(x) (1ULL << ((63 - __builtin_clzll(x)) + 1))
#define PREV_POWER_OF_TWO(x) (1ULL << (63 - __builtin_clzll(x)))
#define IS_ALIGNED(x,y)(((uintptr_t)x & ((uintptr_t)y - 1)) == 0)


#define API_TRACE_ENABLED
#if defined(API_TRACE_ENABLED)
typedef struct api_tracer_t
{
    int32_t num_allocations;
    int64_t allocation_size;
    int64_t arena_size;
    int64_t alignment;
    int64_t slot_used;
    int64_t slot_idx;
    int64_t slots_remaining;
    int64_t slot_reused;
    int64_t deferred_free;
    int64_t fallback;
} api_tracer;
extern __thread api_tracer _test_tracer;
#define clz(x) (x == 0? 32:__builtin_clz(x))
#define T_INCR(label, x) (_test_tracer.label += x)
#define T_DECR(label, x) (_test_tracer.label -= x)
#define T_SET(label, x) (_test_tracer.label = x)
#else
#define T_INCR(label, x) void
#define T_DECR(label, x) void
#define T_SET(label, x) void
#endif
 
static size_t os_page_size = DEFAULT_OS_PAGE_SIZE;

// 9 arena types.
// 4,8,16,32,64,128,256,512,1GB
typedef enum AreaType_t {
    AT_FIXED_4 = 0,
    AT_FIXED_8 = 1,
    AT_FIXED_16 = 2,
    AT_FIXED_32 = 3,
    AT_FIXED_64 = 4,
    AT_FIXED_128 = 5,
    AT_FIXED_256 = 6,
    AT_FIXED_512 = 7,
    AT_FIXED_1024 = 8,
    AT_VARIABLE = 9
} AreaType;

static cache_align const uintptr_t area_type_to_exponent[] = {
    22, // 2^22 == 4MB
    23, // 2^23 == 8MB
    24, // 2^24 == 16MB
    25, // 2^25 == 32MB
    26, // 2^26 == 64MB
    27, // 2^27 == 128MB
    28  // 2^28 == 256MB
};

static const uintptr_t partition_type_to_exponent[] = {
    28, // 256 MB
    29, // 512 MB
    30, // 1024 MB
    31, // 2048 MB
    32, // 4096 MB
    33, // 8192 MB
    34, // 16384 MB
    35, // 32GB
    36, // 64GB
    37, // 128GB
};

static const uintptr_t area_type_to_size[] = {
    AREA_SIZE_SMALL>>3, AREA_SIZE_SMALL>>2, AREA_SIZE_SMALL>>1,
    AREA_SIZE_SMALL, AREA_SIZE_MEDIUM, AREA_SIZE_LARGE,
    AREA_SIZE_HUGE, AREA_SIZE_HUGE<<1, AREA_SIZE_HUGE<<2};

static inline uint64_t area_size_from_partition_id(uint8_t pid) { return BASE_AREA_SIZE << (pid%64); }

static inline int32_t partition_id_from_addr(uintptr_t p)
{
    intptr_t ptrp = p - (1ULL << 40);
    if(ptrp < 0)
    {
        return -1;
    }
    else
    {
        return 32 - clz(ptrp >> 39);
    }
}

static inline int32_t partition_allocator_from_addr_and_part(uintptr_t p, uint32_t at)
{
    size_t offset = BASE_ADDR(at);
    const ptrdiff_t diff = (uint8_t *)p - (uint8_t *)offset;
    return (uint32_t)(((size_t)diff) >> partition_type_to_exponent[at]);
}

static inline int32_t partition_allocator_from_addr(uintptr_t p)
{
    int32_t at = partition_id_from_addr(p);
    if(at < 0)
    {
        return -1;
    }
    return partition_allocator_from_addr_and_part(p, at);
}

static inline uint64_t area_size_from_addr(uintptr_t p) { return area_size_from_partition_id(partition_id_from_addr(p)); }


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

// lockless message queue
typedef struct MessageHeader_t
{
    uint16_t idx;
    uint16_t count;
    uint32_t next;
} MessageHeader;


// lockless message queue
typedef struct AtomicMessage_t
{
    _Atomic(uintptr_t) next; // next message idx
} AtomicMessage;

typedef struct AtomicQueue_t
{
    uintptr_t head;
    _Atomic(uintptr_t) tail;
} AtomicQueue;


typedef struct Heap_t
{
    Block* free;
    _Atomic(uintptr_t) thread_free_counter;
    size_t block_size;
    void *prev;
    void *next;
} Heap;

static inline void init_heap(Heap*f)
{
    f->free = NULL;
    f->thread_free_counter = 0;
    f->prev = NULL;
    f->next = NULL;
}

uint32_t deferred_move_thread_free(Heap* d);
void deferred_thread_enqueue_ptr(Heap *d, uintptr_t msg);
void deferred_thread_enqueue_message(Heap *d, uintptr_t msg, uintptr_t header);

// to infer between a pool and an arena.
// the first bit after the heap header:
//  1 for an arena.
//  0 for a pool.
typedef struct Pool_t
{
    // 56 byte header
    Block* deferred_free;
    _Atomic(uintptr_t) thread_free_counter;
    size_t block_size;
    void *prev;
    void *next;
    
    // 32 byte body
    int32_t idx;        // index in the parent section. Shifted up by one to keep the lowest bit zero.
    uint32_t block_idx; // index into the pool queue. What size class do you belong to.
    
    int32_t num_used;
    
    int32_t num_committed;
    int32_t num_available;

    uint32_t alignment;
    
    Block* free;
    void*  _mask; // just a placeholder. Will be positioned 8 bytes before the first block.
} Pool;

typedef struct HeapBlock_t
{
    uint8_t *data;
} HeapBlock;

typedef struct QNode_t
{
    void* prev;
    void* next;
} QNode;

typedef struct Arena_t
{
    Block* deferred_free;
    _Atomic(uintptr_t) thread_free_counter;
    
    size_t block_size;
    
    void *prev;
    void *next;
    
    // 40
    uint64_t  allocations;
    uint64_t  ranges;
    uint64_t  zero;
    
    uint32_t partition_id;
    uint8_t container_exponent;
    
} Arena; // 64 bytes

typedef struct Arena_alloc_t
{
    Heap* heap;
    uint32_t block_idx;
} Arena_alloc;

typedef struct Partition_t
{
    _Atomic(uint64_t) area_mask;     // which parts have been allocated.
    _Atomic(uint64_t) range_mask;    // the extends for each part.
    _Atomic(uint64_t) zero_mask;     // which parts have been initilized.
    _Atomic(uint64_t) full_mask;     // which parts have free internal memory.
    _Atomic(uint64_t) commit_mask;
} Partition;

struct mutex_t;
typedef struct PartitionAllocator_t
{
    int64_t idx;
    Partition area[NUM_AREA_PARTITIONS];
    struct mutex_t * part_lock;
    struct PartitionAllocator_t *prev;
    struct PartitionAllocator_t *next;

} PartitionAllocator;

typedef enum slot_type_t
{
    SLOT_NONE = 0,
    SLOT_POOL = 1,
    SLOT_ARENA = 2,
    SLOT_COUNTER = 3,
    SLOT_AREA = 4,
} slot_type;

typedef struct alloc_slot_t
{
    uintptr_t header;
    
    int32_t end;        // end of contiguous block
    int32_t offset;     // current start of free memory
    
    int32_t start;      // the offset where we start counting from
    int32_t block_size;

    int32_t alignment;
    int32_t counter;    // the number of addresses handed out to users
    
    int32_t req_size;   // current requested size
    int32_t pend;       // parents contiguous block end
    
    uintptr_t pheader;  // parents contiguos header
    
} alloc_slot;

typedef struct deferred_free_t
{
    Block items;
    uint32_t num;
    uint32_t owned;
    uintptr_t start;
    uintptr_t end;
    size_t block_size;
} deferred_free;

static inline int32_t is_arena_type(Heap* h)
{
    uintptr_t hptr = (uintptr_t)h + sizeof(Heap);
    uint8_t val = *(uint8_t*)hptr;
    return val & 0x1;
}

static inline void deferred_add(deferred_free*c, void* p)
{
    ((Block*)p)->next = c->items.next;
    c->items.next =  p;
    c->num++;
}

typedef struct Allocator_t
{
    int32_t idx;
    int64_t prev_size;
    PartitionAllocator *part_alloc;
    
    // free pools of various sizes.
    Queue *pools;
    Queue* arenas;
    
    // per allocator lookup structures
    alloc_slot c_slot;        // allocation cache structure.
    deferred_free c_deferred;  // release cache structure.
    
    Queue partition_allocators;
} Allocator;

typedef struct Allocator_param_t
{
    uintptr_t thread_id;
    size_t size;
    size_t alignment;
    bool zero;
} Allocator_param;

void deferred_init(Allocator* a, void*p);
void deferred_release(Allocator* a, void* p);
Allocator *get_instance(uintptr_t tid);
void *alloc_memory_aligned(void *base, uintptr_t end, size_t size, size_t alignment);

static inline uint32_t partition_allocator_get_partition_idx(PartitionAllocator* pa, Partition* queue)
{
    uintptr_t delta = (uintptr_t)queue - (uintptr_t)&pa->area[0];
    return  (uint32_t)(delta / sizeof(Partition));
}

static inline uint32_t partition_allocator_get_arena_idx_from_queue(PartitionAllocator *pa, Arena *arena, Partition *partition)
{
    AreaType at = (AreaType)partition_allocator_get_partition_idx(pa, partition);
    size_t base_size = BASE_AREA_SIZE * 64 << (uint64_t)at;
    size_t offset = BASE_ADDR(at);
    size_t start_addr = (pa->idx)*base_size + offset;
    const ptrdiff_t diff = (uint8_t *)arena - (uint8_t *)start_addr;
    return (uint32_t)(((size_t)diff) >> arena->container_exponent);
}
// list utilities
static inline bool qnode_is_connected(QNode* n)
{
    return (n->prev != 0) || (n->next != 0);
}
static inline bool heap_is_connected(Heap *p) { return p->prev != NULL || p->next != NULL; }

static inline void _list_append(void *queue, void *node, size_t head_offset, size_t prev_offset)
{
    Queue *tq = (Queue *)((uint8_t *)queue + head_offset);
    if (tq->tail != 0) {
        QNode *tn = (QNode *)((uint8_t *)node + prev_offset);
        tn->next = 0;
        tn->prev = tq->tail;
        QNode *temp = (QNode *)((uint8_t *)tq->tail + prev_offset);
        temp->next = tq->tail = node;
    } else {
        tq->tail = tq->head = node;
    }
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
#define list_append(q, n) _list_append(q, n, offsetof(__typeof__(*q), head), offsetof(__typeof__(*n), prev))
#define list_enqueue(q, n) _list_enqueue(q, n, offsetof(__typeof__(*q), head), offsetof(__typeof__(*n), prev))
#define list_remove(q, n) _list_remove(q, n, offsetof(__typeof__(*q), head), offsetof(__typeof__(*n), prev))


static inline int32_t find_first_nones(uintptr_t x, int64_t n, uint32_t step_exp)
{
    if(n > 64)
    {
        return -1;
    }
    
    if(x == 0)
    {
        return -1;
    }
    
    if(step_exp > 0)
    {
        uint64_t submask = (1ULL << n) - 1;
        uint32_t width = 1U << step_exp;
        uint32_t steps = 64 >> step_exp;
        uint32_t tz = __builtin_ctzll(x);
        int32_t i = (tz >> step_exp);
        i += (tz & (1 << step_exp) - 1) ? 1 : 0;
        for(; i < steps; i++)
        {
            uint64_t _x = x >> i*width;
            if((submask & _x) == submask)
            {
                return i*width;
            }
            else if(_x == 0)
            {
                break;
            }
        }
        return -1;
    }
    else
    {
        int64_t s;
        while (n > 1) {
            s = n >> 1;
            x = x & (x >> s);
            n = n - s;
        }
    
        return x == 0 ? -1 : __builtin_ctzll(x);
    }
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

static inline uint32_t num_consecutive_zeros(uint64_t test)
{
    if(test == 0)
    {
        return 64;
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
    
    while (test >= (1ULL << mz))
    {
        tz = __builtin_ctzll(test);
        mz = mz ^ ((mz ^ tz) & -(mz < tz));
        test = test >> (tz + 1);
        test = test >> __builtin_ctzll(~test);
    }
    return mz;
}

#endif /* callocator_inl */
