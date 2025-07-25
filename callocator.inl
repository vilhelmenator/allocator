
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

#define PARTITION_COUNT 9
#define BASE_ADDRESS (2ULL * 1024 * 1024 * 1024 * 1024) // 2TB
#define BASE_OS_ALLOC_ADDRESS (12ULL * 1024 * 1024 * 1024 * 1024) // 12TB
#define PARTITION_SIZE_EXP (40) // 1TB
#define PARTITION_SIZE (1ULL << PARTITION_SIZE_EXP)
#define BASE_REGION_SIZE (1ULL << 22ULL)

#define ARENA_LEVELS 3
#define POOL_BIN_COUNT 80
#define ARENA_BIN_COUNT PARTITION_COUNT
#define IMPLICIT_LIST_BIN_COUNT 4


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


#define DEFAULT_ALIGNMENT sizeof(intptr_t)
#define NEXT_POWER_OF_TWO(x) (1ULL << ((63 - __builtin_clzll(x)) + 1))
#define PREV_POWER_OF_TWO(x) (1ULL << (63 - __builtin_clzll(x)))
#define IS_ALIGNED(x,y)(((uintptr_t)x & ((uintptr_t)y - 1)) == 0)

//



#define clz(x) (x == 0? 32:__builtin_clz(x))
 
/*

Partition (64GB - 128GB)
│
├── Region/Zone (256MB/512MB/...)
│   │
│   ├── Arena (256MB)
│   │   │
│   │   ├── Chunk/Segment (4MB)
│   │   │   │
│   │   │   └── Pool (optional)
│   │   │       │
│   │   │       └── Block (e.g., 16KB)
│   │   │
│   │   └── ... (other Chunks)
│   │
│   └── ... (other Arenas)
│
└── ... (other Regions)
*/

static size_t os_page_size = DEFAULT_OS_PAGE_SIZE;

// 9 arena types.
// 4,8,16,32,64,128,256,512,1GB
typedef enum  {
    AT_FIXED_4 = 0,
    AT_FIXED_8 = 1,
    AT_FIXED_16 = 2,
    AT_FIXED_32 = 3,
    AT_FIXED_64 = 4,
    AT_FIXED_128 = 5,
    AT_FIXED_256 = 6,
    AT_FIXED_512 = 7,
    AT_FIXED_1024 = 8,
} RegionType;

static cache_align const uintptr_t region_type_to_exponent[] = {
    22, // 2^22 == 4MB
    23, // 2^23 == 8MB
    24, // 2^24 == 16MB
    25, // 2^25 == 32MB
    26, // 2^26 == 64MB
    27, // 2^27 == 128MB
    28, // 2^28 == 256MB
    29, // 2^29 == 512MB
    30, // 2^30 == 1024MB
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
};

static const uintptr_t region_type_to_size[] = {
    1 << region_type_to_exponent[0],
    1 << region_type_to_exponent[1],
    1 << region_type_to_exponent[2],
    1 << region_type_to_exponent[3],
    1 << region_type_to_exponent[4],
    1 << region_type_to_exponent[5],
    1 << region_type_to_exponent[6],
    1 << region_type_to_exponent[7],
    1 << region_type_to_exponent[8],
};

static inline uint64_t region_size_from_partition_id(uint8_t pid) { return BASE_REGION_SIZE << (pid%64); }

static inline int32_t partition_id_from_addr(uintptr_t p) {
    
    uintptr_t offset = p - BASE_ADDRESS;

    // Check if the address is outside the allocator's range.
    if (p < BASE_ADDRESS || offset >= (PARTITION_COUNT * PARTITION_SIZE)) {
        return -1;  // Invalid address.
    }

    // Compute partition ID: offset / 64GiB.
    return (int32_t)(offset / PARTITION_SIZE);
}

static inline uint64_t area_size_from_addr(uintptr_t p) { return region_size_from_partition_id(partition_id_from_addr(p)); }


typedef struct Block_t
{
    struct Block_t* next;
} Block;

typedef struct Queue_t
{
    void* head;
    void* tail;
} Queue;


typedef struct
{
    _Atomic(uintptr_t) thread_free_counter;
    Block* free;
    size_t block_size;
    void *prev;
    void *next;
    
    int32_t parent_idx;     // index in the parent container.
                            // Shifted up by one to keep the lowest bit zero.
                            // the lower bit is a type indicator.
                            // 1 for arena, 0 for pool.
    uint32_t local_idx;     // index in the local group.
    
    // 64 bytes
    // contiguos cache data
    int32_t start;      // the offset where we start counting from
    int32_t end;        // end of contiguous block
    int32_t offset;     // current start of free memory
    
} AllocatorContext;

static inline void init_context(AllocatorContext*f)
{
    f->free = NULL;
    f->thread_free_counter = 0;
    f->prev = NULL;
    f->next = NULL;
    f->parent_idx = 0;
    f->local_idx = 0;
    // contiguous
    f->start = 0;      // the offset where we start counting from
    f->end = 0;        // end of contiguous block
    f->offset = 0;     // current start of free memory
}

// to infer between a pool and an arena.
// the first bit after the heap header:
//  1 for an arena.
//  0 for a pool.
typedef struct Pool_t
{
    // 56 byte header
    _Atomic(uintptr_t) thread_free_counter;
    Block* deferred_free;
    size_t block_size;
    void *prev;
    void *next;
    
    
    int32_t idx;        // index in the parent section. Shifted up by one to keep the lowest bit zero.
    uint32_t block_idx; // index into the pool queue. What size class do you belong to.
    
    // 64
    int32_t start;      // the offset where we start counting from
    int32_t end;        // end of contiguous block
    int32_t offset;     // current start of free memory
    
    // 32 byte body
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

// Boundary tag allocation structure
typedef struct ImplicitList_t
{
    // 56 byte header
    _Atomic(uintptr_t) thread_free_counter;
    Block* deferred_free;
    size_t block_size;
    void *prev;
    void *next;
    
    int32_t idx;            // index into the parent chunk
    int32_t block_idx;      // index into the region of the arena

    // 64 byte 
    uint32_t total_memory; // how much do we have available in total
    uint32_t used_memory;  // how much have we used
    uint32_t min_block;    // what is the minum size block available;
    uint32_t max_block;    // what is the maximum size block available;
    uint32_t num_allocations;

    Queue free_nodes;

} ImplicitList;

typedef struct QNode_t
{
    void* prev;
    void* next;
} QNode;

typedef struct Arena_t
{
    // 64 byte base heap header
    _Atomic(uintptr_t) thread_free_counter;
    Block* deferred_free;
    size_t block_size;
    
    void *prev;
    void *next;
    
    int32_t  idx;           // index of the partition region
    uint32_t partition_id;  // index of the partition
    
    // 64 byte cache line and arena body.
    int32_t start;      // the offset where we start counting from
    int32_t end;        // end of contiguous block
    int32_t offset;     // current start of free memory
    
    
    _Atomic(uint64_t)  in_use;
    _Atomic(uint64_t)  retained_but_free;
    _Atomic(uint64_t)  ranges;
    _Atomic(uint64_t)  zero;
    
    uintptr_t thread_id;
} Arena; // 128 bytes

typedef struct Arena_alloc_t
{
    AllocatorContext* heap;
    uint32_t block_idx;
} Arena_alloc;


typedef struct {
    _Atomic(uint64_t) reserved;     // which parts have been reserved.
    _Atomic(uint64_t) committed;    // which parts have committed virtual mem
    _Atomic(uint64_t) ranges;       // the extends for each part.
    
    _Atomic(uint64_t) pending_release;  // which parts can be reclaimed or decommitted
    _Atomic(uint64_t) retained_but_free; // which parts are sitting in cache and unused
} PartitionMasks;

typedef struct {
    PartitionMasks* blocks; // Array of allocated blocks
    size_t num_blocks;
    size_t blockSize;          // Fixed block size for this partition
} Partition;

typedef struct {
    Partition partitions[PARTITION_COUNT];
    size_t totalMemory;        // Total memory managed by the allocator
} PartitionAllocator;

struct mutex_t;


typedef enum slot_type_t
{
    SLOT_NONE = 0,
    SLOT_POOL = 1,
    SLOT_ARENA = 2,
    SLOT_COUNTER = 3,
    SLOT_REGION = 4,
} slot_type;


typedef struct deferred_free_t
{
    Block items;
    uint32_t num;
    uint32_t owned;
    uintptr_t start;
    uintptr_t end;
    size_t block_size;
} deferred_free;

static inline int32_t is_arena_type(AllocatorContext* h)
{
    return h->parent_idx & 0x1;
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
    
    // free pools of various sizes.
    Queue *pools;
    Queue* arenas;
    Queue* implicit_lists;
    
    // per allocator lookup structures
    AllocatorContext* c_slot;        // allocation cache structure.
    deferred_free c_deferred;  // release cache structure.
    
    uintptr_t thread_id;
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

// list utilities
static inline bool qnode_is_connected(QNode* n)
{
    return (n->prev != 0) || (n->next != 0);
}
static inline bool is_connected(AllocatorContext *p) { return p->prev != NULL || p->next != NULL; }

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


static inline int32_t find_first_nzeros(uint64_t x, int64_t n, uint32_t step_exp) {
    if (n <= 0 || n > 64) return -1;
    if (x == 0) return 0;  // All zeros case

    const uint64_t mask = (1ULL << n) - 1;
    const uint32_t width = (step_exp > 0) ? (1U << step_exp) : 1;
    const uint32_t steps = 64 / width;

    for (uint32_t i = 0; i < steps; i++) {
        const uint32_t pos = i * width;
        if (pos + n > 64) break;

        // Check for n consecutive zeros starting at pos
        const uint64_t window = (x >> pos) & mask;
        if (window == 0) {
            return pos;
        }
    }
    return -1;
}

static inline int32_t find_first_nones(uintptr_t x, int64_t n, int32_t exp) {
    // Special case: if x is all 0s, the first n zeros start at bit 0
    if (x == ~0ULL) return (n <= 64) ? 0 : -1;
    
    return find_first_nzeros(~x, n, exp);
}
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
    if (~test == 0) return 64;  // All zeros
    if (test == 0) return 64;   // All ones (just for symmetry)

    uint32_t max_gap = 0;
    uint32_t current_gap = 0;
    
    // Unroll the loop for better performance
    for (int i = 0; i < 64; i++) {
        if ((test & (1ULL << i)) == 0) {
            current_gap++;
        } else {
            if (current_gap > max_gap) {
                max_gap = current_gap;
            }
            current_gap = 0;
        }
    }
    
    // Check if the longest gap ends at the MSB
    if (current_gap > max_gap) {
        max_gap = current_gap;
    }
    
    return max_gap;
}

static uint64_t count_ones(uint64_t x) {
    x = (x & 0x5555555555555555) + ((x >> 1) & 0x5555555555555555);
    x = (x & 0x3333333333333333) + ((x >> 2) & 0x3333333333333333);
    x = (x & 0x0F0F0F0F0F0F0F0F) + ((x >> 4) & 0x0F0F0F0F0F0F0F0F);
    return (x * 0x0101010101010101) >> 56;
}
#endif /* callocator_inl */
