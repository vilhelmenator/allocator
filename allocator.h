
// [ ] thread_free
// [ ] thread_setup
// [ ] stats.
// [ ] tests.
//    - ordered allocations.
//    - unordered allocations. causing fragmentation.
//    - writing into allocated memory, ensuring that writing into the bounds
//    does not cause issues.
//    - writing and deleting memory from threads.
//    - comparing against other allocators performance.

//
typedef enum t_desc_type {
    dt_none = -1, //  null
    dt_builtin = 0, //  local type
    dt_struct, //  local struct
    dt_array, //  local array
    dt_struct_ref, //  heap struct
    dt_builtin_ref, //  heap builtin
    dt_array_ref, //  heap array
} desc_type;

//
typedef struct t_desc_define
{
    const char *name; // who
    desc_type dtype; // what
    uint32_t size; //
    uint32_t offset; // where
    const t_desc_define *schema; // how
} desc_define;

// terminal placement.
const desc_define end_desc = { NULL, dt_none, 0, 0, NULL };

// define struct
const desc_define sub_type[] {
    { "thing", dt_none, 4, 0, NULL },
    end_desc
};

const desc_define my_type[] {
    { "thing", dt_none, 4, 0, sub_type },
    end_desc
};
//
// the allocator can remove a ptr network.
//      - plucks free memory into a deferred list for each owner thread.
//      - can more effectively release memory.
//      - can pluck items into a deferred list per thread.
// the allocator can move a ptr network.
//      - copy any item of memory that is not local to the current thread.
// the allocator can copy a ptr network.
//      - make a new copy of a network in a single contiguous buffer.
//
void test_a(void *src, const desc_define *t)
{
    // free a network of objects.
    // does the type have desc objects.
    // are they all pointing to the same one.
    //  - then it is just collecting the offsets and traversing those.
    //  - for every type of equal size. if the ptrs are all just within the same thread area.
    //      we can simply string the ptrs together like a
    int i = 0;
    do {
        //
        //
        //
    } while (t[i++].name != NULL);
}

void test()
{
    test_a(NULL, my_type);
}

/*

 Another random thought.
    - if a complicated structure is something that the allocator understands. Such as a tree, or a graph.
        : Can the allocator be used like a persistence util to traverse through the pointers and release them.
        : A user app could collect all pointers into an array and call free for the whole buffer.
        : Calling a destructor on an object would cause the system to defer free all the items until the destructor is done.
        : allot::destruct( obj ) -> any nested calls to destruct would cause a deferred free operation.
        : pass in a pointer to a struct. pass in a struct to describe the navigation path of a pointer tree.
        : traverse the network of pointers and pass them to their correct pools or pages.
        : destruct( ptr, schema ) -> no nested destruct calls.
        : construct( ptr, schema ) ->
 1.
 [ ] random allocations sizes.
    - within a pool size class.
    - within a partition size class.
    - mix pool size classes.
    - mix partition size classes.
    - when allocator is empty.
    - when allocator is getting half full and bleeding over partitions.
    - when allocator is getting full and running out of memory.
2.
 [ ] add thread free for pools.
 [ ] add thread free for pages.
 [ ] test thread free performance from multiple threads.
 [ ] random allocations sizes.
    - same as previous allocation test, but with multiple threads.
    - all memory in separate threads.
    - distribute memory among threads so that each thread is freeing memory into other threads and its own.
    - distribute memory among threads so that each thread is only freeing memory into other threads.
4.
 [ ] improve page allocations. ordererd lists. double free tests.
 [ ] memory API. alloc and string functions.
5.
 [ ] add memory block objects and implicit list alocation support.
 [ ] stats. leaks.
 [ ] integrate with ALLOT
 [ ] test with builder. DONE!
 */
#ifndef _alloc_h_
#define _alloc_h_
/*

 */
#include "clogger.h"
#if defined(CLOGGING)
CLOGGER(_alloc_h_, 4096)
#endif

#if defined(_MSC_VER)
#define WINDOWS
#endif

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef WINDOWS
#include <intrin.h>
#include <memoryapi.h>
#include <windows.h>
uint64_t rdtsc()
{
    return __rdtsc();
}
#else
#include <sys/mman.h>
#include <unistd.h>
#include <x86intrin.h>
uint64_t rdtsc()
{
    unsigned int lo, hi;
    __asm__ __volatile__("rdtsc"
                         : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
#endif
#include <atomic>
#include <thread>

#define WSIZE 4 /* Word size in bytes */
#define DSIZE 8 /* Double word size in bytes */
#define SZ_KB 1024ULL
#define SZ_MB (SZ_KB * SZ_KB)
#define SZ_GB (SZ_MB * SZ_KB)

#define max(x, y) ((x) > (y) ? (x) : (y))
#define min(x, y) ((x) < (y) ? (x) : (y))

#define memory_block(x) memory_block_##(x)
#define memory_block_0(x) (_MemoryBlock(x);)
#define memory_block_1(x) (_MemoryBlock(alloca(x), x);)

#define MIN_ALLOC_SIZE sizeof(void *)
#define MAX_SMALL_SIZE 128 * MIN_ALLOC_SIZE
#define MAX_RAW_ALLOC_SIZE 256 * MIN_ALLOC_SIZE

#define DEFAULT_OS_PAGE_SIZE 4096ULL

#define SMALL_OBJECT_SIZE DEFAULT_OS_PAGE_SIZE * 4 // 16k
#define DEFAULT_PAGE_SIZE SMALL_OBJECT_SIZE * 8 // 128kb
#define MEDIUM_OBJECT_SIZE DEFAULT_PAGE_SIZE // 128kb
#define DEFAULT_MID_PAGE_SIZE MEDIUM_OBJECT_SIZE * 4 // 512kb
#define LARGE_OBJECT_SIZE DEFAULT_MID_PAGE_SIZE * 4 // 2Mb
#define DEFAULT_LARGE_PAGE_SIZE LARGE_OBJECT_SIZE * 2 // 4Mb
#define HUGE_OBJECT_SIZE DEFAULT_LARGE_PAGE_SIZE * 8 // 32Mb

#define SECTION_SIZE (1ULL << 22ULL)

#define CACHE_LINE 64
#ifdef WINDOWS
#define cache_align __declspec(align(CACHE_LINE))
#else
#define cache_align __attribute__((aligned(CACHE_LINE)))
#endif

#define AREA_SIZE_SMALL (SECTION_SIZE * 8ULL) // 32Mb
#define AREA_SIZE_LARGE (SECTION_SIZE * 32ULL) // 128Mb
#define AREA_SIZE_HUGE (SECTION_SIZE * 64ULL) // 256Mb
#define MASK_FULL 0xFFFFFFFFFFFFFFFF

#define CACHED_POOL_COUNT 64
#define POOL_BIN_COUNT 17 * 8 + 2
#define HEADER_SIZE 64
#define PAGE_BIN_COUNT 3

#define AREA_SMALL_MAX_MEMORY AREA_SIZE_SMALL - HEADER_SIZE
#define AREA_LARGE_MAX_MEMORY AREA_SIZE_LARGE - HEADER_SIZE
#define SECTION_MAX_MEMORY SECTION_SIZE - HEADER_SIZE
#define PAGE_MAX_MEMORY AREA_SIZE_SMALL * 2 - HEADER_SIZE

#define MAX_SMALL_AREAS 192 // 32Mb over 6Gb
#define MAX_LARGE_AREAS 64 // 128Mb over 8Gb
#define MAX_HUGE_AREAS 64 // 256Mb over 16Gb

#define MAX_THREADS 1024 // don't change this...

cache_align const uintptr_t size_clss_to_exponent[] {
    17, // 128k
    19, // 512k
    22, // 4Mb
    25, // 32Mb
    27, // 128Mb
    28 // 256Mb
};

enum AreaType {
    AT_FIXED_32, //  containes small allocations
    AT_FIXED_128, //  reserved for a particular partition, collecting object
                  //  between 4 - 32 megs.
    AT_FIXED_256, //  reserved for a particular partition, collecting object
                  //  between 32 - 128 megs.
    AT_VARIABLE, //  for objects, where we want a single allocatino per item in a
                 //  dedicated partition.
};
//
// partition counter
//
cache_align const uintptr_t partitions_offsets[] {
    // TERABYTES
    ((uintptr_t)2 << 40), // allocations smaller than SECTION_MAX_MEMORY
    ((uintptr_t)4 << 40),
    ((uintptr_t)8 << 40), // SECTION_MAX_MEMORY < x < AREA_MAX_MEMORY
    ((uintptr_t)16 << 40), // AREA_MAX_MEMORY < x < 1GB
    ((uintptr_t)32 << 40), // resource allocations.
    ((uintptr_t)64 << 40), // Huge allocations
    ((uintptr_t)128 << 40), // end
};

cache_align const uintptr_t partitions_size[] {
    // GIGABYTES
    ((uintptr_t)6 << 30), // the first two partitions are merged
    ((uintptr_t)6 << 30), //
    ((uintptr_t)8 << 30),
    ((uintptr_t)16 << 30),
    ((uintptr_t)32 << 30),
    ((uintptr_t)64 << 30),
    ((uintptr_t)128 << 30),
};

const uint8_t partition_count = 7;

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
    if (pidx < 0) {
        return -1;
    }
    const ptrdiff_t diff = (uint8_t *)p - (uint8_t *)partitions_offsets[pidx];
    return (uint32_t)(((size_t)diff) / partitions_size[pidx]);
}

static inline int16_t partition_id_from_addr(uintptr_t p)
{
    auto pidx = partition_from_addr(p);
    return partition_id_from_addr_and_partition(p, pidx);
}

int countBits(uint32_t v)
{
    uint32_t c = 0;
    for (; v; c++) {
        v &= v - 1; // clear the least significant bit set
    }
    return c;
}

typedef union bitmask
{
    uint64_t whole;
    uint32_t _w32[2];

    inline bool isSet_hi(uint8_t bit) { return _w32[1] & ((uint32_t)1 << bit); }
    inline bool isSet_lo(uint8_t bit) { return _w32[0] & ((uint32_t)1 << bit); }

    inline bool isFull_hi() { return _w32[1] == 0xFFFFFFFF; }
    inline bool isFull_lo() { return _w32[0] == 0xFFFFFFFF; }
    inline bool isEmpty_hi() { return _w32[1] == 0; }
    inline bool isEmpty_lo() { return _w32[0] == 0; }

    inline void reserveAll() { whole = 0xFFFFFFFFFFFFFFFF; }

    inline void reserve_hi(uint8_t bit) { _w32[1] |= ((uint32_t)1 << bit); }
    inline void reserve_lo(uint8_t bit) { _w32[0] |= ((uint32_t)1 << bit); }

    inline void freeAll() { whole = 0; }
    inline void freeIdx_hi(uint8_t bit) { _w32[1] &= ~((uint32_t)1 << bit); }
    inline void freeIdx_lo(uint8_t bit) { _w32[0] &= ~((uint32_t)1 << bit); }

    inline int8_t firstFree_hi()
    {
        auto m = ~_w32[1];
        return __builtin_ctz(m);
    }

    inline int8_t firstFree_lo()
    {
        auto m = ~_w32[0];
        return __builtin_ctz(m);
    }

    inline int8_t allocate_bit_hi()
    {
        if (isFull_hi()) {
            return -1;
        }
        auto fidx = firstFree_hi();
        reserve_hi(fidx);
        return fidx;
    }
    inline int8_t allocate_bit_lo()
    {
        if (isFull_lo()) {
            return -1;
        }
        auto fidx = firstFree_lo();
        reserve_lo(fidx);
        return fidx;
    }
} mask;

static size_t os_num_hardware_threads = std::thread::hardware_concurrency();
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
static size_t os_page_size = get_os_page_size();

#define POWER_OF_TWO(x) ((x & (x - 1)) == 0)

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

static inline uintptr_t align_down(uintptr_t sz, size_t alignment)
{
    uintptr_t mask = alignment - 1;
    if ((alignment & mask) == 0) {
        return (sz & ~mask);
    } else {
        return ((sz / alignment) * alignment);
    }
}

struct spinlock
{
    std::atomic<bool> lock_ = { 0 };

    void lock() noexcept
    {
        for (;;) {
            // Optimistically assume the lock is free on the first try
            if (!lock_.exchange(true, std::memory_order_acquire)) {
                return;
            }
            // Wait for lock to be released without generating cache misses
            while (lock_.load(std::memory_order_relaxed)) {
                // Issue X86 PAUSE or ARM YIELD instruction to reduce contention between
                // hyper-threads
                __builtin_ia32_pause();
            }
        }
    }

    bool try_lock() noexcept
    {
        // First do a relaxed load to check if lock is free in order to prevent
        // unnecessary cache misses if someone does while(!try_lock())
        return !lock_.load(std::memory_order_relaxed) && !lock_.exchange(true, std::memory_order_acquire);
    }

    void unlock() noexcept { lock_.store(false, std::memory_order_release); }
};

struct heap_block
{
    uint8_t *data;

    inline uint32_t get_header()
    {
        return *(uint32_t *)((uint8_t *)&data - WSIZE);
    }

    inline uint32_t get_footer()
    {
        uint32_t size = *(uint32_t *)((uint8_t *)&data - WSIZE) & ~0x7;
        return *(uint32_t *)((uint8_t *)&data + (size)-DSIZE);
    }

    inline uint32_t get_Alloc(uint32_t v) { return v & 0x1; }

    inline uint32_t get_Size(uint32_t v) { return v & ~0x7; }

    inline void set_header(uint32_t s, uint32_t v)
    {
        *(uint32_t *)((uint8_t *)&data - WSIZE) = (s | v);
    }

    inline void set_footer(uint32_t s, uint32_t v)
    {
        uint32_t size = (*(uint32_t *)((uint8_t *)&data - WSIZE)) & ~0x7;
        *(uint32_t *)((uint8_t *)(&data) + (size)-DSIZE) = (s | v);
    }

    inline heap_block *next()
    {
        uint32_t size = *(uint32_t *)((uint8_t *)&data - WSIZE) & ~0x7;
        return (heap_block *)((uint8_t *)&data + (size));
    }

    inline heap_block *prev()
    {
        uint32_t size = *(uint32_t *)((uint8_t *)&data - DSIZE) & ~0x7;
        return (heap_block *)((uint8_t *)&data - (size));
    }
};

#define HEAP_NODE_OVERHEAD 8

struct heap_node
{
    size_t *prev;
    size_t *next;
    inline void setNext(heap_node *p)
    {
        next = (size_t *)p;
    }
    inline void setPrev(heap_node *p)
    {
        prev = (size_t *)p;
    }
    inline heap_node *getNext()
    {
        return (heap_node *)next;
    }
    inline heap_node *getPrev()
    {
        return (heap_node *)prev;
    }
};

static inline size_t align(size_t n)
{
    return (n + sizeof(intptr_t) - 1) & ~(sizeof(intptr_t) - 1);
}

struct Block
{
    Block *next;
};

enum Exponent {
    EXP_PUNY = 0,
    EXP_SMALL,
    EXP_MEDIUM,
    EXP_LARGE,
    EXP_HUGE,
    EXP_GIGANTIC
};

enum ContainerType {
    PAGE,
    POOL,
    SLAB,
};

inline int32_t getContainerExponent(size_t s, ContainerType t)
{
    if (t == PAGE) {
        if (s <= MEDIUM_OBJECT_SIZE) { // 16k - 4Mb
            return EXP_LARGE; // 32
        } else if (s <= HUGE_OBJECT_SIZE) { // 4Mb - 32Mb
            return EXP_HUGE; // 128
        } else { // for large than 32Mb objects.
            return EXP_GIGANTIC; // 256
        }
    } else {
        if (s <= SMALL_OBJECT_SIZE) { // 8 - 16k
            return EXP_PUNY; // 128k
        } else if (s <= MEDIUM_OBJECT_SIZE) { // 16k - 128k
            return EXP_SMALL; // 512k
        } else {
            return EXP_MEDIUM; // 4M for > 128k objects.
        }
    }
}

// list functions
template <typename L>
static inline bool list_isEmpty(L &list)
{
    return list.first == NULL;
}

template <typename L, typename T>
static inline void list_remove(L &list, T *a)
{
    if (a->getPrev() != NULL)
        a->getPrev()->setNext(a->getNext());
    if (a->getNext() != NULL)
        a->getNext()->setPrev(a->getPrev());
    if (a == list.head)
        list.head = a->getNext();
    if (a == list.tail)
        list.tail = a->getPrev();
    a->setNext(NULL);
    a->setPrev(NULL);
}

template <typename L, typename T>
static inline void list_enqueue(L &list, T *a)
{
    a->setNext(list.head);
    a->setPrev(NULL);
    if (list.head != NULL) {
        list.head->setPrev(a);
        list.head = a;
    } else {
        list.tail = list.head = a;
    }
}

template <typename L, typename T>
static inline void list_insert_sort(L &list, T *a)
{
    if (list.tail == NULL) {
        list.tail = list.head = a;
        return;
    }

    if (list.tail < a) {
        list.tail->setNext(a);
        a->setNext(NULL);
        a->setPrev(list.tail);
        list.tail = a;
        return;
    }

    T *current = list.head;
    while (current->getNext() != NULL && current->getNext() < a) {
        current = current->getNext();
    }
    a->setPrev(current->getPrev());
    a->setNext(current->getNext());
    current->setPrev(a);
}

template <typename L, typename T>
static inline void list_insert_at(L &list, T *t, T *a)
{
    if (list.tail == NULL) {
        list.tail = list.head = a;
        return;
    }

    a->setNext(t->getNext());
    a->setPrev(t);
    if (t->getNext() != NULL) {
        t->getNext()->setPrev(a);
    } else {
        list.tail = a;
    }
    t->setNext(a);
}

struct Area
{
    // The area is overlapping with the first section. And they share some
    // attributes.
    static const uintptr_t small_area_mask = 0xff;
    static const uintptr_t large_area_mask = 0xffffffff;
    static const uintptr_t ptr_mask = 0x0000ffffffffffff;
    static const uintptr_t inv_ptr_mask = 0xffff000000000000;
    struct List
    {
        Area *head;
        Area *tail;
        uint32_t partition_id;
        uintptr_t start_addr;
        uintptr_t end_addr;
        AreaType type;
        size_t area_count;
        Area *previous_area;
    };

    int64_t partition_id;
    bitmask constr_mask; // containers that have been constructed.
    bitmask active_mask; // containers that can be destructed.

private:
    // these members are shared with the first section in the memory block. so,
    // the first high 16 bits are reserved by the section.
    size_t size;
    Area *prev;
    Area *next;

public:
    inline bool isEmpty() { return active_mask.isEmpty_hi(); }
    inline bool isFree() { return isEmpty(); }
    inline bool isClaimed(uint8_t idx) { return constr_mask.isSet_hi(idx); }

    inline void freeIdx(uint8_t i)
    {
        active_mask.freeIdx_hi(i);
    }

    inline void freeAll()
    {
        active_mask.freeAll();
    }

    inline void reserveIdx(uint8_t i)
    {
        constr_mask.reserve_hi(i);
        active_mask.reserve_hi(i);
    }

    inline void reserveAll()
    {
        constr_mask.reserveAll();
        active_mask.reserveAll();
    }

    int8_t get_section_count()
    {
        auto area_size = getSize();
        if (area_size == AREA_SIZE_SMALL) {
            return 8;
        } else if (area_size == AREA_SIZE_LARGE) {
            return 32;
        } else {
            return 1;
        }
    }

    inline int8_t claimSection()
    {
        return constr_mask.allocate_bit_hi();
    }

    inline void claimAll()
    {
        constr_mask.reserveAll();
    }

    inline void claimIdx(uint8_t idx)
    {
        constr_mask.reserve_hi(idx);
    }

    bool isFull()
    {
        if (active_mask.isFull_hi()) {
            return true;
        }
        if (getSize() == AREA_SIZE_SMALL) {
            return ((active_mask.whole >> 32) & small_area_mask) == small_area_mask;
        } else {
            return ((active_mask.whole >> 32) & large_area_mask) == large_area_mask;
        }
    }
    inline Area *getPrev() const
    {
        return (Area *)((uintptr_t)prev & ptr_mask);
    } // remove the top 16 bits
    inline Area *getNext() const
    {
        return (Area *)((uintptr_t)next & ptr_mask);
    } // remove the top 16 bits
    inline void setPrev(Area *p)
    {
        prev = (Area *)(((inv_ptr_mask & (uintptr_t)prev) | (uintptr_t)p));
    };
    inline void setNext(Area *n)
    {
        next = (Area *)(((inv_ptr_mask & (uintptr_t)next) | (uintptr_t)n));
    };

    inline size_t getSize() { return size; }
    inline void setSize(size_t s) { size = s; }

    static Area *fromAddr(uintptr_t p)
    {
        static const uint64_t masks[] = {
            ~(AREA_SIZE_SMALL - 1),
            ~(AREA_SIZE_SMALL - 1),
            ~(AREA_SIZE_LARGE - 1),
            ~(AREA_SIZE_HUGE - 1),
            0xffffffffffffffff,
            0xffffffffffffffff,
            0xffffffffffffffff
        };

        auto pidx = partition_from_addr(p);
        if (pidx < 0) {
            return NULL;
        }
        return (Area *)(p & masks[pidx]);
    }
};

struct Section
{
    struct Queue
    {
        Section *head;
        Section *tail;
    };
    int64_t partition_id;
    // 24 bytes as the section header
    bitmask constr_mask; // 32 pages bit per page.   // lower 32 bits per section/ high
    bitmask active_mask;

private:
    // An area and section can overlap, and the prev next pointer of an area will
    // always be under the 32tb range. top 16 bits area always zero.
    size_t asize; // lower 32 bits per section/ high bits are for area
    size_t container_type; // top 16 bits.
    size_t container_exponent; // top 16 bits.
public:
    int32_t idx; // index in parent area.

    // links to sections.
    Section *prev;
    Section *next;

    uint8_t collections[1];
    inline bool isConnected() { return prev != NULL || next != NULL; }

    uint8_t get_collection_count()
    {
        if (getContainerType() != POOL) {
            return 1;
        }
        switch (getContainerExponent()) {
        case Exponent::EXP_PUNY: {
            return 32;
        }
        case Exponent::EXP_SMALL: {
            return 8;
        }
        default: {
            return 1;
        }
        }
    }

    inline void freeIdx(uint8_t i)
    {
        active_mask.freeIdx_lo(i);
        auto section_empty = active_mask.isEmpty_lo();
        if (section_empty) {
            // what partition are we in.
            auto area = Area::fromAddr((uintptr_t)this);
            switch (getContainerType()) {
            case POOL: {
                area->freeIdx(idx);
                break;
            }
            default: // SLAB
                area->freeAll();
                break;
            }
        }
    }
    inline bool isClaimed(uint8_t idx) { return constr_mask.isSet_lo(idx); }
    inline void reserveIdx(uint8_t i)
    {
        active_mask.reserve_lo(i);
        auto area = Area::fromAddr((uintptr_t)this);
        area->reserveIdx(idx);
    }

    inline void claimIdx(uint8_t i)
    {
        constr_mask.reserve_lo(i);
        auto area = Area::fromAddr((uintptr_t)this);
        area->claimIdx(idx);
    }

    inline void claimAll()
    {
        constr_mask.reserveAll();
        auto area = Area::fromAddr((uintptr_t)this);
        area->claimIdx(idx);
    }

    inline uint8_t reserveNext()
    {
        auto idx = active_mask.allocate_bit_lo();
        claimIdx(idx);
        return idx;
    }

    inline void reserveAll()
    {
        claimAll();
        auto area = Area::fromAddr((uintptr_t)this);
        area->reserveIdx(idx);
        active_mask.reserveAll();
    }

    inline void freeAll()
    {
        active_mask.freeAll();
        auto area = Area::fromAddr((uintptr_t)this);
        area->freeIdx(idx);
    }

    inline Section *getPrev() const { return prev; }
    inline Section *getNext() const { return next; }
    inline void setPrev(Section *p) { prev = p; }
    inline void setNext(Section *n) { next = n; }
    inline ContainerType getContainerType() const
    {
        return (
            ContainerType)((uint16_t)((container_type & 0xffff000000000000) >> 48));
    }
    inline Exponent getContainerExponent() const
    {
        return (Exponent)(uint16_t)((container_exponent & 0xffff000000000000) >> 48);
    }
    inline void setContainerType(ContainerType pt)
    {
        container_type |= ((uint64_t)pt << 48);
    }
    inline void setContainerExponent(uint16_t prt)
    {
        container_exponent |= ((uint64_t)prt << 48);
    }
    inline size_t getSize() const { return asize; }

    inline bool isFull() const
    {
        switch (getContainerExponent()) {
        case Exponent::EXP_PUNY: {
            return (active_mask.whole & 0xffffffff) == 0xffffffff;
        }
        case Exponent::EXP_SMALL: {
            return (active_mask.whole & 0xff) == 0xff;
        }
        default: {
            return (active_mask.whole & 0x1) == 0x1;
        }
        }
    }

    inline void *findCollection(void *p) const
    {
        const ptrdiff_t diff = (uint8_t *)p - (uint8_t *)&collections[0];
        const auto exp = size_clss_to_exponent[(container_exponent & 0xffff000000000000) >> 48];
        const auto page_size = 1 << exp;
        const int32_t idx = (int32_t)((size_t)diff >> exp);
        return (void *)((uint8_t *)&collections[0] + page_size * idx);
    }

    inline uintptr_t getCollection(int8_t idx, Exponent exp) const
    {
        return (uintptr_t)((uint8_t *)&collections[0] + (1 << size_clss_to_exponent[exp]) * idx);
    }
};

struct Pool
{
    struct Queue
    {
        Pool *head;
        Pool *tail;
        size_t size;
    };

    int32_t idx;
    uint32_t block_size; //
    int32_t num_available; // num of available blocks. // num of committed blocks.
    int32_t num_committed; // the number of free blocks. page_size/block_size
    int32_t num_used;

    Block *free;
    Pool *prev;
    Pool *next;

    inline bool ownsAddr(void *p) const
    {
        uintptr_t start = (uintptr_t)&blocks[0];
        uintptr_t end = (uintptr_t)((uint8_t *)start + (num_available * block_size));
        return (uintptr_t)p >= start && (uintptr_t)p <= end;
    }

    inline bool isEmpty() const { return num_used == 0; }
    inline bool isFull() const { return num_used >= num_available; }
    inline bool isAlmostFull() const { return num_used >= (num_available - 1); }
    inline bool isFullyCommited() const { return num_committed >= num_available; }
    inline bool isConnected() const { return prev != NULL || next != NULL; }

    inline void freeBlock(void *p)
    {
        Block *new_free = (Block *)p;
        new_free->next = free;
        free = new_free;
        num_used--;
        if (num_used == 0) {
            Section *section = (Section *)((uintptr_t)this & ~(SECTION_SIZE - 1));
            section->freeIdx(idx);
        }
    }

    void init(int8_t pidx, uint32_t blockSize, Exponent partition)
    {
        auto psize = 1 << size_clss_to_exponent[partition];

        uintptr_t section_end = align_up((uintptr_t)this, SECTION_SIZE);
        auto remaining_size = section_end - (uintptr_t)&blocks[0];
        auto block_memory = psize - sizeof(Pool);

        idx = pidx;
        block_size = blockSize;
        num_available = (int)(min(remaining_size, block_memory) / blockSize);
        num_committed = 0;
        num_used = 0;
        next = NULL;
        prev = NULL;
        free = NULL;
        extendPool();
    }

    inline void *aquireBlock()
    {
        if (!isFull()) {
            extendPool();
            return getFreeBlock();
        }
        return NULL;
    }

    inline Block *getFreeBlock()
    {
        if (num_used == 0) {
            Section *section = (Section *)((uintptr_t)this & ~(SECTION_SIZE - 1));
            section->reserveIdx(idx);
        }
        num_used++;
        auto res = free;
        free = res->next;
        return res;
    }

    bool extendPool()
    {
        if (free == NULL) {
            if (!isFullyCommited()) {
                uintptr_t start = (uintptr_t)((uint8_t *)&blocks[0] + (num_committed * block_size));
                uint32_t steps = 1;
                uint32_t remaining = (num_available - num_committed);
                if (block_size < os_page_size) {
                    steps = (uint32_t)min(os_page_size / block_size, remaining);
                }

                Block *block = (Block *)start;
                num_committed += steps;
                for (uint32_t i = 0; i < steps - 1; i++) {
                    Block *next = (Block *)((uint8_t *)block + block_size);
                    block->next = next;
                    block = next;
                }
                block->next = NULL;
                free = (Block *)start;
                return true;
            }
        }
        return false;
    }

public:
    uint8_t blocks[1];
    inline Pool *getPrev() const { return prev; }
    inline Pool *getNext() const { return next; }
    inline void setPrev(Pool *p) { prev = p; }
    inline void setNext(Pool *n) { next = n; }
};

struct Page
{
    struct Queue
    {
        Page *head;
        Page *tail;
    };
    struct FreeQueue
    {
        heap_node *head;
        heap_node *tail;
    };
    //
    int32_t idx;
    uint32_t total_memory; // how much do we have available in total
    uint32_t used_memory; // how much have we used
    uint32_t min_block; // what is the minum size block available;
    uint32_t max_block; // what is the maximum size block available;
    uint32_t num_allocations;

    uint8_t *start;
    FreeQueue free_nodes;

    Page *prev;
    Page *next;

    inline bool isConnected() { return prev != NULL || next != NULL; }

    inline bool has_room(size_t s)
    {
        if ((used_memory + s + HEAP_NODE_OVERHEAD) > total_memory) {
            return false;
        }
        if (s <= max_block && s >= min_block) {
            return true;
        }
        return false;
    }

    inline void *getBlock(uint32_t s)
    {
        if (s <= DSIZE * 2) {
            s = DSIZE * 2 + HEAP_NODE_OVERHEAD;
        } else {
            s = DSIZE * ((s + HEAP_NODE_OVERHEAD + DSIZE - 1) / DSIZE);
        }
        void *ptr = find_fit(s);

        used_memory += s;
        if (num_allocations == 0) {
            Section *section = (Section *)((uintptr_t)this & ~(SECTION_SIZE - 1));
            section->reserveAll();
        }
        num_allocations++;
        return ptr;
    }

    void free(void *bp, bool should_coalesce)
    {
        if (bp == 0)
            return;

        heap_block *hb = (heap_block *)bp;
        auto size = hb->get_header() & ~0x7;
        hb->set_header(size, 0);
        hb->set_footer(size, 0);

        if (should_coalesce) {
            coalesce(bp);
        } else {
            list_enqueue(free_nodes, (heap_node *)bp);
        }
        used_memory -= size;
        num_allocations--;
        if (num_allocations == 0) {
            Section *section = (Section *)((uintptr_t)this & ~(SECTION_SIZE - 1));
            section->freeAll();
            reset();
        }
    }

    inline void reset()
    {
        free_nodes = { NULL, NULL };
        heap_block *hb = (heap_block *)start;
        list_enqueue(free_nodes, (heap_node *)start);
        hb->set_header(total_memory, 0);
        hb->set_footer(total_memory, 0);
        hb->next()->set_header(0, 1);
    }

    inline void extend()
    {
        *start = 0;
        *(start + WSIZE) = DSIZE | 1; /* Prologue header */
        *(start + DSIZE) = (DSIZE | 1); /* Prologue footer */
        *(start + WSIZE + DSIZE) = 1; /* Epilogue header */
        start = start + DSIZE * 2;

        reset();
    }

    inline void *coalesce(void *bp)
    {
        heap_block *hb = (heap_block *)bp;
        auto size = hb->get_header() & ~0x7;
        heap_block *prev_block = hb->prev();
        heap_block *next_block = hb->next();
        int prev_header = prev_block->get_header();
        int next_header = next_block->get_header();

        size_t prev_alloc = prev_header & 0x1;
        size_t next_alloc = next_header & 0x1;

        heap_node *hn = (heap_node *)bp;
        if (!(prev_alloc && next_alloc)) {
            size_t prev_size = prev_header & ~0x7;
            size_t next_size = next_header & ~0x7;

            // next is free
            if (prev_alloc && !next_alloc) {
                size += next_size;
                hb->set_header(size, 0);
                hb->set_footer(size, 0);
                heap_node *h_next = (heap_node *)next_block;
                list_remove(free_nodes, h_next);
                list_enqueue(free_nodes, hn);
            } // prev is fre
            else if (!prev_alloc && next_alloc) {
                size += prev_size;
                hb->set_footer(size, 0);
                prev_block->set_header(size, 0);
                bp = (void *)hb->prev();
            } else { // both next and prev are free
                size += prev_size + next_size;
                prev_block->set_header(size, 0);
                next_block->set_footer(size, 0);
                bp = (void *)hb->prev();
                heap_node *h_next = (heap_node *)next_block;
                list_remove(free_nodes, h_next);
            }
        } else {
            list_enqueue(free_nodes, hn);
        }

        return bp;
    }

    inline void place(void *bp, uint32_t asize)
    {
        heap_block *hb = (heap_block *)bp;
        auto csize = hb->get_header() & ~0x7;
        if ((csize - asize) >= (DSIZE + HEAP_NODE_OVERHEAD)) {
            hb->set_header(asize, 1);
            hb->set_footer(asize, 1);
            hb = hb->next();
            hb->set_header(csize - asize, 0);
            hb->set_footer(csize - asize, 0);
            list_enqueue(free_nodes, (heap_node *)hb);
        } else {
            hb->set_header(csize, 1);
            hb->set_footer(csize, 1);
        }
    }

    inline void *find_fit(uint32_t asize)
    {
        auto current = free_nodes.head;
        while (current != NULL) {
            heap_block *hb = (heap_block *)current;
            int header = hb->get_header();
            uint32_t bsize = header & ~0x7;
            if (asize <= bsize) {
                list_remove(free_nodes, current);
                place(current, asize);
                return current;
            }
            current = current->getNext();
        }
        return NULL;
    }

    void init(int8_t pidx, Exponent partition)
    {
        auto psize = 1 << size_clss_to_exponent[partition];
        uintptr_t section_end = align_up((uintptr_t)this, psize);
        auto remaining_size = section_end - (uintptr_t)&blocks[0];

        auto block_memory = psize - sizeof(Page) - sizeof(Section);
        auto header_footer_offset = sizeof(uintptr_t) * 2;
        idx = pidx;
        used_memory = 0;
        total_memory = (uint32_t)((min(remaining_size, block_memory)) - header_footer_offset - HEAP_NODE_OVERHEAD);
        max_block = total_memory;
        min_block = sizeof(uintptr_t);
        num_allocations = 0;
        start = &blocks[0];
        next = NULL;
        prev = NULL;
        extend();
    }

public:
    uint8_t blocks[1];
    inline Page *getPrev() const { return prev; }
    inline Page *getNext() const { return next; }
    inline void setPrev(Page *p) { prev = p; }
    inline void setNext(Page *n) { next = n; }
};

#define Z4(x) x(), x(), x(), x()
#define Z8(x) Z4(x), Z4(x)
#define Z16(x) Z8(x), Z8(x)
#define Z32(x) Z16(x), Z16(x)
#define Z64(x) Z32(x), Z32(x)
#define Z128(x) Z64(x), Z64(x)
#define Z256(x) Z128(x), Z128(x)
#define Z512(x) Z256(x), Z256(x)
#define Z1024(x) Z512(x), Z512(x)

#define zQueue(x)                           \
    {                                       \
        NULL, NULL, sizeof(uintptr_t) * (x) \
    }
#define zPoolQueue                                                                                                                          \
    {                                                                                                                                       \
        zQueue(0), zQueue(1), zQueue(2), zQueue(3), zQueue(4), zQueue(5), zQueue(6), zQueue(7),                                             \
            zQueue(8), zQueue(9), zQueue(10), zQueue(11), zQueue(12), zQueue(13), zQueue(14), zQueue(15),                                   \
            zQueue(16), zQueue(18), zQueue(20), zQueue(22), zQueue(24), zQueue(26), zQueue(28), zQueue(30),                                 \
            zQueue(32), zQueue(36), zQueue(40), zQueue(44), zQueue(48), zQueue(52), zQueue(56), zQueue(60),                                 \
            zQueue(64), zQueue(72), zQueue(80), zQueue(88), zQueue(96), zQueue(104), zQueue(112), zQueue(120),                              \
            zQueue(128), zQueue(144), zQueue(160), zQueue(176), zQueue(192), zQueue(208), zQueue(224), zQueue(240),                         \
            zQueue(256), zQueue(288), zQueue(320), zQueue(352), zQueue(384), zQueue(416), zQueue(448), zQueue(480),                         \
            zQueue(512), zQueue(576), zQueue(640), zQueue(704), zQueue(768), zQueue(832), zQueue(896), zQueue(960),                         \
            zQueue(1024), zQueue(1152), zQueue(1280), zQueue(1408), zQueue(1536), zQueue(1664), zQueue(1792), zQueue(1920),                 \
            zQueue(2048), zQueue(2304), zQueue(2560), zQueue(2816), zQueue(3072), zQueue(3328), zQueue(3584), zQueue(3840),                 \
            zQueue(4096), zQueue(4608), zQueue(5120), zQueue(5632), zQueue(6144), zQueue(6656), zQueue(7168), zQueue(7680),                 \
            zQueue(8192), zQueue(9216), zQueue(10240), zQueue(11264), zQueue(12288), zQueue(13312), zQueue(14336), zQueue(15360),           \
            zQueue(16384), zQueue(18432), zQueue(20480), zQueue(22528), zQueue(24576), zQueue(26624), zQueue(28672), zQueue(30720),         \
            zQueue(32768), zQueue(36864), zQueue(40960), zQueue(45056), zQueue(49152), zQueue(53248), zQueue(57344), zQueue(61440),         \
            zQueue(65536), zQueue(73728), zQueue(81920), zQueue(90112), zQueue(98304), zQueue(106496), zQueue(114688), zQueue(122880),      \
            zQueue(131072), zQueue(147456), zQueue(163840), zQueue(180224), zQueue(196608), zQueue(212992), zQueue(229376), zQueue(245760), \
            zQueue(262144), zQueue(294912), zQueue(327680), zQueue(360448), zQueue(393216), zQueue(425984), zQueue(458752), zQueue(491520), \
            zQueue(524288), zQueue(589824)                                                                                                  \
    }

#define POOL_HEAP() zPoolQueue
#define zPageQueue                                     \
    {                                                  \
        { NULL, NULL }, { NULL, NULL }, { NULL, NULL } \
    }

#define PAGE_HEAP() zPageQueue
#define zSectionQueue \
                      \
    {                 \
        NULL, NULL    \
    }

#define SECTION_HEAP() zSectionQueue

#define TH10(tid, TH)                                                         \
    TH(tid), TH(tid + 1), TH(tid + 2), TH(tid + 3), TH(tid + 4), TH(tid + 5), \
        TH(tid + 6), TH(tid + 7), TH(tid + 8), TH(tid + 9)
#define TH100(tid, x)                                                              \
    TH10(tid * 100, x), TH10(tid * 100 + 10, x), TH10(tid * 100 + 20, x),          \
        TH10(tid * 100 + 30, x), TH10(tid * 100 + 40, x), TH10(tid * 100 + 50, x), \
        TH10(tid * 100 + 60, x), TH10(tid * 100 + 70, x), TH10(tid * 100 + 80, x), \
        TH10(tid * 100 + 90, x)
#define TH1000(x)                                                    \
    TH100(0, x), TH100(1, x), TH100(2, x), TH100(3, x), TH100(4, x), \
        TH100(5, x), TH100(6, x), TH100(7, x), TH100(8, x), TH100(9, x)
#define TH1024(x)                                                          \
    {                                                                      \
        TH1000(x), x(1000), x(1001), x(1002), x(1003), x(1004), x(1005),   \
            x(1006), x(1007), x(1008), x(1009), x(1010), x(1011), x(1012), \
            x(1013), x(1014), x(1015), x(1016), x(1017), x(1018), x(1019), \
            x(1020), x(1021), x(1022), x(1023)                             \
    }

#define NEG() -1
#define ZERO() 0
#define VALUE(x) x
#define EMPTY_OWNERS \
    {                \
        Z1024(NEG)   \
    }
#define DEFAULT_OWNERS TH1024(VALUE)

static cache_align int64_t partition_owners[MAX_THREADS] = EMPTY_OWNERS;

std::mutex partition_mutex;
static inline int8_t reserve_any_partition_set()
{
    std::unique_lock<std::mutex> lock(partition_mutex);
    int8_t reserved_id = -1;
    for (int i = 0; i < 1024; i++) {
        if (partition_owners[i] == -1) {
            partition_owners[i] = i;
            reserved_id = i;
            break;
        }
    }
    return reserved_id;
}
static inline int8_t reserve_any_partition_set_for(int8_t midx)
{
    std::unique_lock<std::mutex> lock(partition_mutex);
    int8_t reserved_id = -1;
    for (int i = 0; i < 1024; i++) {
        if (partition_owners[i] == -1) {
            partition_owners[i] = midx;
            reserved_id = i;
            break;
        }
    }
    return reserved_id;
}
static inline bool reserve_partition_set(int8_t idx, int8_t midx)
{
    std::unique_lock<std::mutex> lock(partition_mutex);
    if (partition_owners[idx] == -1) {
        partition_owners[idx] = midx;
        return true;
    }
    return false;
}

static inline void release_partition_set(int8_t idx)
{
    if (idx >= 0) {
        std::unique_lock<std::mutex> lock(partition_mutex);
        partition_owners[idx] = -1;
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
    return (mmap(base, size, PROT_NONE,
                (MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE), -1,
                0)
        == base);
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
        VirtualUnlock(base, size); // VirtualUnlock after MEM_RESET removes the memory from the working set
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
std::mutex aligned_alloc_mutex;
static void *alloc_memory_aligned(void *base, size_t size, size_t alignment,
    bool commit, uintptr_t end)
{
    // alignment is smaller than a page size or not a power of two.
    if (!(alignment >= os_page_size && POWER_OF_TWO(alignment)))
        return NULL;
    size = align_up(size, os_page_size);
    if (size >= (SIZE_MAX - alignment))
        return NULL;

    std::unique_lock<std::mutex> lock(aligned_alloc_mutex);
    void *ptr = alloc_memory(base, size, commit);
    if (ptr == NULL)
        return NULL;

    if (!safe_to_aquire(base, ptr, size, end)) {
        free_memory(ptr, size);
        return NULL;
    }

    if (((uintptr_t)ptr % alignment != 0)) {
        // this should happen very rarely, if at all.
        // release our failed attempt.
        free_memory(ptr, size);

        // Now we attempt to overallocate
        size_t adj_size = size + alignment;
        ptr = alloc_memory(base, adj_size, commit);
        if (ptr == NULL)
            return NULL;

        // if the new ptr is not in our current partition set
        if (!safe_to_aquire(base, ptr, adj_size, end)) {
            free_memory(ptr, adj_size);
            return NULL;
        }

        // if we got our aligned memory
        if (((uintptr_t)ptr % alignment) == 0) {
            // drop our excess request
            decommit_memory((uint8_t *)ptr + size, alignment);
            return ptr;
        }
        // we are still not aligned, but we have an address that is aligned.
        free_memory(ptr, adj_size);
        //
        void *aligned_p = (void *)align_up((uintptr_t)ptr, alignment);
        // get our aligned address
        ptr = alloc_memory(aligned_p, size, commit);
        if (ptr == NULL) {
            // Why would this fail?
            return NULL;
        }
        if (!safe_to_aquire(base, ptr, size, end)) {
            free_memory(ptr, size);
            return NULL;
        }

        // if the system fails to get memory from the part we just released.
        // there is some other allocator screwing with our assumptions... so FAIL!
        if (((uintptr_t)ptr % alignment) != 0) {
            free_memory(ptr, size);
            return NULL;
        }
    }
    return ptr;
}

cache_align Pool::Queue pool_queues[MAX_THREADS][POOL_BIN_COUNT] = { Z1024(POOL_HEAP) };
cache_align Page::Queue page_queues[MAX_THREADS][3] = { Z1024(PAGE_HEAP) };
cache_align Section::Queue section_queues[MAX_THREADS] = { Z1024(SECTION_HEAP) };

static inline uint8_t sizeToPool(size_t as)
{
    static const int bmask = ~0x7f;
    if ((bmask & as) == 0) {
        return as >> 3;
    } else {
        const int tz = __builtin_clzll(as);
        size_t numWidth = 60 - tz;
        return 8 + ((numWidth - 3) << 3) + ((as >> (numWidth & 0x7)));
    }
}

static inline uint8_t sizeToPage(size_t as)
{
    if (as <= MEDIUM_OBJECT_SIZE) {
        return 0; // 32mb pages
    } else if (as <= AREA_SIZE_SMALL) {
        return 1; // 128Mb pages
    } else {
        return 2; // 256Mb pages
    }
}

struct PartitionAllocator
{
public:
    Area::List area_01;
    Area::List area_2;
    Area::List area_3;
    static Area::List area_4;
    static Area::List area_5;

    // sections local to this thread with free pages or pools
    Section::Queue *sections;
    // free pages that have room for various size allocations.
    Page::Queue *pages;
    // how man pages in total have been allocated.
    uint32_t page_count;

    // free pools of various sizes.
    Pool::Queue *pools;
    // how many pools in total have been allocated.
    uint32_t pool_count;

    bool release_local_areas()
    {
        bool was_released = false;
        if (area_01.area_count) {
            was_released |= release_areas_from_queue(&area_01);
        }
        if (area_2.area_count) {
            was_released |= release_areas_from_queue(&area_2);
        }
        if (area_3.area_count) {
            was_released |= release_areas_from_queue(&area_3);
        }
        return was_released;
    }

    void free_area(Area *area)
    {
        size_t size = area->getSize();
        if (size == AREA_SIZE_SMALL) {
            free_area(area, AreaType::AT_FIXED_32);
        } else if (size == AREA_SIZE_LARGE) {
            free_area(area, AreaType::AT_FIXED_128);
        } else if (size == AREA_SIZE_HUGE) {
            free_area(area, AreaType::AT_FIXED_256);
        } else {
            free_area(area, AreaType::AT_VARIABLE);
        }
    }

    Area::List *get_area_queue(Area *area)
    {
        size_t size = area->getSize();
        if (size == AREA_SIZE_SMALL) {
            return &area_01;
        } else if (size == AREA_SIZE_LARGE) {
            return &area_2;
        } else {
            return &area_3;
        }
    }

    Area *get_area(size_t size, ContainerType t)
    {
        Area *curr_area = NULL;
        uint32_t small_area_limit = LARGE_OBJECT_SIZE;
        if (t == PAGE) {
            small_area_limit = MEDIUM_OBJECT_SIZE;
        }
        if (size <= small_area_limit) {
            curr_area = get_free_area(size, AreaType::AT_FIXED_32);
            if (curr_area == NULL) {
                bool was_released = release_single_area_from_queue(&area_01);
                was_released |= release_single_area_from_queue(&area_2);
                if (was_released) {
                    // try again.
                    curr_area = get_free_area(size, AreaType::AT_FIXED_32);
                }
            }
        } else if (size <= AREA_SIZE_SMALL) {
            curr_area = get_free_area(size, AreaType::AT_FIXED_128);
            if (curr_area == NULL) {
                bool was_released = release_single_area_from_queue(&area_2);
                was_released |= release_single_area_from_queue(&area_3);
                if (was_released) {
                    // try again.
                    curr_area = get_free_area(size, AreaType::AT_FIXED_128);
                }
            }
        } else if (size <= AREA_SIZE_LARGE) {
            curr_area = get_free_area(size, AreaType::AT_FIXED_256);
            if (curr_area == NULL) {
                if (release_single_area_from_queue(&area_3)) {
                    // try again.
                    curr_area = get_free_area(size, AreaType::AT_FIXED_256);
                }
            }
        } else {
            curr_area = get_free_area(size, AreaType::AT_VARIABLE);
            if (curr_area == NULL) {
                if (release_single_area_from_queue(&area_3)) {
                    // try again.
                    curr_area = get_free_area(size, AreaType::AT_VARIABLE);
                }
            }
        }

        return curr_area;
    }

    Section *alloc_section(size_t size, ContainerType t)
    {
        Area *new_area = get_area(size, t);
        if (new_area == NULL) {
            return NULL;
        }

        int32_t section_idx = claim_section(new_area);

        Section *section = (Section *)((uint8_t *)new_area + SECTION_SIZE * section_idx);
        section->constr_mask = { 0 };
        section->active_mask = { 0 };
        section->idx = section_idx;
        section->partition_id = new_area->partition_id;
        return section;
    }

    Area *get_next_area(Area::List *area_queue, uint64_t size,
        uint64_t alignment)
    {
#define get_next_area_assert __LINE__
#define get_next_area_head_is_null __LINE__
#define get_next_area_head_is_not_null __LINE__
#define get_next_area_room_internally __LINE__
#define get_next_area_room_at_the_end __LINE__
#define get_next_area_room_at_the_front __LINE__
#define get_next_area_no_room_left __LINE__

        void *aligned_addr = NULL;
        Area *insert = NULL;
        auto asize = align_up(size, alignment);
        uint64_t delta = (uint64_t)((uint8_t *)area_queue->end_addr - area_queue->start_addr);
        log_assert(_alloc_h_, get_next_area_assert, alignment > size);
        if (area_queue->head == NULL) {
            aligned_addr = (void *)area_queue->start_addr;
            log_define(_alloc_h_, get_next_area_head_is_null);
        } else {
            // is there room at the end
            //
            log_define(_alloc_h_, get_next_area_head_is_not_null);
            uint64_t tail_end = (uint64_t)((uint8_t *)area_queue->tail + area_queue->tail->getSize());
            delta = (uint64_t)((uint8_t *)area_queue->end_addr - tail_end);
            if (delta < size && (area_queue->tail != area_queue->head)) {
                Area *current = area_queue->head;
                while (current != area_queue->tail) {
                    Area *next = current->getNext();
                    size_t c_size = current->getSize();
                    size_t c_end = c_size + (size_t)(uint8_t *)current;
                    delta = (uint64_t)((uint8_t *)next - c_end);
                    if (delta >= asize) {
                        log_define(_alloc_h_, get_next_area_room_internally);
                        insert = current;
                        break;
                    }
                    current = next;
                }
            } else if (delta >= size) {
                log_define(_alloc_h_, get_next_area_room_at_the_end);
                insert = area_queue->tail;
            }

            if (insert == NULL) {
                log_define(_alloc_h_, get_next_area_room_at_the_front);
                delta = (uint64_t)((uint8_t *)area_queue->head - area_queue->start_addr);
            } else {
                auto si = insert->getSize();
                auto offset = (uintptr_t)((uint8_t *)insert + si);
                aligned_addr = (void *)align_up((uintptr_t)offset, alignment);
            }
        }
        if (delta < size) {
            log_define(_alloc_h_, get_next_area_no_room_left);
            return NULL;
        }

        Area *new_area = (Area *)alloc_memory_aligned(aligned_addr, size, alignment, true, area_queue->end_addr);
        if (new_area == NULL) {
            return NULL;
        }
        new_area->active_mask = { 0 };
        new_area->constr_mask = { 0 };
        new_area->partition_id = area_queue->partition_id;
        new_area->setNext(NULL);
        new_area->setPrev(NULL);
        list_insert_at(*area_queue, insert, new_area);
        // list_enqueue(*area_queue, new_area);
        return new_area;
    }

    Area *alloc_area(Area::List *area_queue, uint64_t area_size,
        uint64_t alignment)
    {
        Area *new_area = get_next_area(area_queue, area_size, alignment);
        if (new_area == NULL) {
            return NULL;
        }
        area_queue->previous_area = new_area;
        area_queue->area_count++;
        return new_area;
    }

    void free_area(Area *a, AreaType t)
    {
        switch (t) {
        case AT_FIXED_32: {
            area_01.area_count--;
            list_remove(area_01, a);
            if ((a == area_01.previous_area) || (area_01.area_count == 0)) {
                area_01.previous_area = NULL;
            }

            break;
        }
        case AT_FIXED_128: {
            area_2.area_count--;
            list_remove(area_2, a);
            if ((a == area_2.previous_area) || (area_2.area_count == 0)) {
                area_2.previous_area = NULL;
            }
            break;
        }
        default: {
            area_3.area_count--;
            list_remove(area_3, a);
            if ((a == area_3.previous_area) || (area_3.area_count == 0)) {
                area_3.previous_area = NULL;
            }
            break;
        }
        };
        free_memory(a, a->getSize());
    }

    uint32_t get_max_area_count(AreaType t)
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
    Area::List *get_current_queue(AreaType t, size_t s, size_t *area_size, size_t *alignement)
    {
        switch (t) {
        case AT_FIXED_32: {
            *area_size = AREA_SIZE_SMALL;
            *alignement = *area_size;
            return &area_01;
        }
        case AT_FIXED_128: {
            *area_size = AREA_SIZE_LARGE;
            *alignement = *area_size;
            return &area_2;
        }
        case AT_FIXED_256: {
            *area_size = AREA_SIZE_HUGE;
            *alignement = *area_size;
            return &area_3;
        }
        default: {
            *area_size = s;
            *alignement = AREA_SIZE_HUGE;
            return &area_3;
        }
        };
    }

    Area::List *promote_area(AreaType *t, size_t *area_size, size_t *alignement)
    {
        switch (*t) {
        case AT_FIXED_32: {
            *area_size = AREA_SIZE_LARGE;
            *alignement = *area_size;
            *t = AT_FIXED_128;
            return &area_2;
        }
        case AT_FIXED_128: {
            *area_size = AREA_SIZE_HUGE;
            *alignement = *area_size;
            *t = AT_FIXED_256;
            return &area_3;
        }
        default: {
            return NULL;
        }
        };
    }

    Area *get_free_area_from_queue(Area::List *current_queue)
    {
        // the areas are empty
        Area *new_area = NULL;
        Area *previous_area = current_queue->previous_area;
        if (previous_area != NULL) {
            if (!previous_area->isFull()) {
                new_area = previous_area;
            }
        } else {
            if (current_queue->head != NULL) {
                Area *start = current_queue->head;
                while (start != NULL) {
                    auto next = start->getNext();
                    if (!start->isFull()) {
                        new_area = start;
                        break;
                    }
                    start = next;
                }
            }
        }
        return new_area;
    }

    Area *get_free_area(size_t s, AreaType t)
    {
        size_t area_size = AREA_SIZE_SMALL;
        size_t alignment = area_size;
        Area::List *current_queue = get_current_queue(t, s, &area_size, &alignment);

        // the areas are empty
        Area *new_area = get_free_area_from_queue(current_queue);
        // try promoting first.
        if (new_area == NULL && (get_max_area_count(t) == current_queue->area_count)) {
            current_queue = promote_area(&t, &area_size, &alignment);
            if (current_queue == NULL) {
                return NULL;
            }
            new_area = get_free_area_from_queue(current_queue);
            if (new_area == NULL && (get_max_area_count(t) == current_queue->area_count)) {
                return NULL;
            }
        }

        if (new_area == NULL) {
            if (area_size < os_page_size) {
                area_size = os_page_size;
            }
            new_area = alloc_area(current_queue, area_size, alignment);
            if (new_area == NULL) {
                return NULL;
            }
            new_area->setSize(area_size);
        }

        return new_area;
    }

    uint32_t claim_section(Area *area)
    {
        return area->claimSection();
    }

    bool release_areas_from_queue(Area::List *queue)
    {
        bool was_released = false;
        Area *start = queue->head;
        // find free section.
        // detach all pools/pages/sections.
        while (start != NULL) {
            auto next = start->getNext();
            was_released |= try_release_containers(start);
            start = next;
        }
        start = queue->head;
        while (start != NULL) {
            auto next = start->getNext();
            was_released |= try_release_area(start);
            start = next;
        }
        return was_released;
    }

    bool release_single_area_from_queue(Area::List *queue)
    {
        bool was_released = false;
        Area *start = queue->head;
        // find free section.
        // detach all pools/pages/sections.
        while (start != NULL) {
            auto next = start->getNext();
            was_released |= try_release_containers(start);
            if (was_released) {
                was_released |= try_release_area(start);
                break;
            }
            start = next;
        }
        return was_released;
    }

    bool try_release_containers(Area *area)
    {
        if (area->isFree()) {

            // all sections should be free and very likely in the free sections list.
            int num_sections = area->get_section_count();
            Section *root_section = (Section *)area;
            ContainerType root_ctype = root_section->getContainerType();
            if (root_ctype == PAGE) {
                Exponent exp = root_section->getContainerExponent();
                auto page = (Page *)root_section->getCollection(0, exp);
                auto queue = &pages[root_section->getContainerExponent() - 3];
                list_remove(*queue, page);
                return true;
            }

            for (int i = 0; i < num_sections; i++) {
                Section *section = (Section *)((uint8_t *)area + SECTION_SIZE * i);

                if (!area->isClaimed(i)) {
                    continue;
                }
                int num_collections = section->get_collection_count();
                Exponent exp = section->getContainerExponent();

                for (int j = 0; j < num_collections; j++) {
                    if (!section->isClaimed(j)) {
                        continue;
                    }
                    auto pool = (Pool *)section->getCollection(j, exp);
                    auto queue = &pools[sizeToPool(pool->block_size)];
                    list_remove(*queue, pool);
                }

                list_remove(*sections, section);
            }
            return true;
        }
        return false;
    }

    bool try_release_area(Area *area)
    {
        if (area->isFree()) {

            free_area(area);
            return true;
        }
        return false;
    }
};

// create our 1024 static area containers by macro expansion
#define PARTITION_01 ((uintptr_t)2 << 40)
#define PARTITION_2 ((uintptr_t)8 << 40)
#define PARTITION_3 ((uintptr_t)16 << 40)
#define PARTITION_4 ((uintptr_t)32 << 40)
#define PARTITION_5 ((uintptr_t)64 << 40)
#define PARTITION_6 ((uintptr_t)128 << 40)

#define PARTITION_DEFINE(tid)                                      \
    {                                                              \
        {                                                          \
            NULL,                                                  \
            NULL,                                                  \
            tid,                                                   \
            (tid) * (SZ_GB * 6) + PARTITION_01,                    \
            (tid) * (SZ_GB * 6) + PARTITION_01 + (SZ_GB * 6),      \
            AreaType::AT_FIXED_32,                                 \
            0,                                                     \
            NULL                                                   \
        },                                                         \
            { NULL,                                                \
                NULL,                                              \
                tid,                                               \
                (tid) * (SZ_GB * 8) + PARTITION_2,                 \
                (tid) * (SZ_GB * 8) + PARTITION_2 + (SZ_GB * 8),   \
                AreaType::AT_FIXED_128,                            \
                0,                                                 \
                NULL },                                            \
            { NULL, NULL, tid, (tid) * (SZ_GB * 16) + PARTITION_3, \
                (tid) * (SZ_GB * 16) + PARTITION_3 + (SZ_GB * 16), \
                AreaType::AT_FIXED_256, 0, NULL },                 \
            &section_queues[tid],                                  \
            page_queues[tid],                                      \
            0,                                                     \
            pool_queues[tid],                                      \
            0,                                                     \
    }

#define PARTITION_LAYOUT TH1024(PARTITION_DEFINE)

Area::List PartitionAllocator::area_4 = { NULL, NULL, 0, ((uintptr_t)32 << 40), ((uintptr_t)64 << 40), AreaType::AT_VARIABLE, 0, NULL };
Area::List PartitionAllocator::area_5 = {
    NULL, NULL, 0, ((uintptr_t)64 << 40), ((uintptr_t)128 << 40), AreaType::AT_VARIABLE, 0, NULL
};

static cache_align PartitionAllocator partition_allocators[MAX_THREADS] = PARTITION_LAYOUT;

static std::atomic<int32_t> global_thread_idx = { 0 };

struct thread_init
{
    int8_t base_partition_set;
    // constructor
    thread_init();
    ~thread_init();
};

struct Allocator
{
    thread_local static uint8_t main_index;

    int32_t _thread_idx;
    PartitionAllocator *part_alloc;
    Pool *previous_pool;
    size_t previous_size;
    uintptr_t previous_pool_start;
    uintptr_t previous_pool_end;

private:
    inline void set_previous_pool(Pool *p)
    {
        previous_pool = p;
        previous_pool_start = (uintptr_t)&p->blocks[0];
        previous_pool_end = (uintptr_t)((uint8_t *)previous_pool_start + (p->num_available * p->block_size));
    }

    inline bool check_previous_pool(void *p) const
    {
        if (previous_pool) {
            return (uintptr_t)p >= previous_pool_start && (uintptr_t)p <= previous_pool_end;
        } else {
            return false;
        }
    }

public:
    inline static Allocator &get_thread_instance();

    inline bool is_main() { return _thread_idx == 0; }

    inline int64_t thread_id() { return (int64_t)this; }

    void *malloc(size_t s)
    {
        if (previous_pool != NULL && previous_size == s) {
            if (previous_pool->free != NULL) {
                return previous_pool->getFreeBlock();
            } else {
                previous_pool = NULL;
            }
        } else {
            previous_pool = NULL;
        }
        if (s <= LARGE_OBJECT_SIZE) {
            return alloc_from_pool(s);
        } else if (s <= AREA_SIZE_LARGE) {
            // allocate form the large page
            return alloc_from_page(s);
        } else {
            return alloc_slab(s);
        }
    }

    void *malloc_page(size_t s)
    {
        if (s <= AREA_SIZE_LARGE) {
            // allocate form the large page
            return alloc_from_page(s);
        } else {
            return alloc_slab(s);
        }
    }

    void free(void *p)
    {
        if (p == NULL)
            return;

        if (check_previous_pool(p)) {
            return previous_pool->freeBlock(p);
        } else {
            previous_pool = NULL;
        }
        switch (partition_from_addr((uintptr_t)p)) {
        case 0:
        case 1:
            free_from_section(p, AREA_SIZE_SMALL);
            break;
        case 2:
            free_from_section(p, AREA_SIZE_LARGE);
            break;
        case 3:
            free_huge(p);
            break;
        default:
            break;
        }
    }

    bool release_local_areas()
    {
        previous_pool = NULL;
        bool result = false;
        auto midx = main_index;
        for (int i = 0; i < MAX_THREADS; i++) {
            if (partition_owners[i] == midx) {
                bool was_released = partition_allocators[i].release_local_areas();
                if (midx != i && was_released) {
                    release_partition_set(i);
                }
                result |= was_released;
            }
        }
        return result;
    }

private:
    inline void free_from_section(void *p, size_t area_size)
    {
        Section *section = (Section *)((uintptr_t)p & ~(area_size - 1));
        // if it is page section, free
        if (_thread_idx == partition_owners[section->partition_id]) {
            if (section->getContainerType() == POOL) {
                section = (Section *)((uintptr_t)p & ~(SECTION_SIZE - 1));
                Pool *pool = (Pool *)section->findCollection(p);
                pool->freeBlock(p);
                set_previous_pool(pool);
                if (!pool->isConnected()) {
                    // reconnect
                    auto _part_alloc = &partition_allocators[section->partition_id];
                    auto queue = &_part_alloc->pools[sizeToPool(pool->block_size)];
                    if (queue->head != pool && queue->tail != pool) {
                        list_enqueue(*queue, pool);
                    }
                    auto sections = _part_alloc->sections;
                    if (!section->isConnected()) {
                        if (sections->head != section && sections->tail != section) {
                            list_enqueue(*sections, section);
                        }
                    }
                }
            } else {
                Page *page = (Page *)section->findCollection(p);
                uint32_t pageIdx = section->getContainerExponent() - 3;
                page->free(p, pageIdx > 0);
                // if the free pools list is empty.
                if (!page->isConnected()) {
                    // reconnect
                    auto _part_alloc = &partition_allocators[section->partition_id];
                    auto queue = &_part_alloc->pages[pageIdx];
                    if (queue->head != page && queue->tail != page) {
                        list_enqueue(*queue, page);
                    }
                }
            }
        }
    }

    inline void free_huge(void *p)
    {
        Section *section = (Section *)((uintptr_t)p & ~(AREA_SIZE_HUGE - 1));
        // if it is page section, free
        if (_thread_idx == partition_owners[section->partition_id]) {
            auto _part_alloc = &partition_allocators[section->partition_id];
            if (section->getContainerType() == PAGE) {
                auto page = (Page *)section->findCollection(p);
                page->free(p, true);
                // if the pool is disconnected from the queue
                if (!page->isConnected()) {
                    auto queue = &_part_alloc->pages[section->getContainerExponent() - 3];
                    // reconnect
                    list_enqueue(*queue, page);
                }
            } else // SLAB
            {
                Area *area = (Area *)section;
                _part_alloc->free_area(area);
            }
        }
    }

    Section *get_free_section(size_t s, ContainerType t)
    {
        auto exponent = getContainerExponent(s, t);
        auto free_section = part_alloc->sections->head;

        // find free section.
        while (free_section != NULL) {
            auto next = free_section->next;
            if (free_section->getContainerExponent() == exponent) {
                if (!free_section->isFull()) {
                    break;
                } else {
                    list_remove(*part_alloc->sections, free_section);
                }
            }
            free_section = next;
        }

        if (free_section == NULL) {
            auto new_section = part_alloc->alloc_section(s, t);
            if (new_section == NULL) {
                return NULL;
            }
            new_section->setContainerExponent(exponent);
            new_section->setContainerType(t);

            new_section->next = NULL;
            new_section->prev = NULL;
            list_enqueue(*part_alloc->sections, new_section);

            free_section = new_section;
        }
        return free_section;
    }

    void *alloc_from_page(size_t s)
    {
        Page *start = NULL;
        auto queue = &part_alloc->pages[sizeToPage(s)];
        if (queue->head != NULL) {
            start = queue->head;
            while (start != NULL) {
                auto next = start->next;
                if (start->has_room(s)) {
                    return start->getBlock((uint32_t)s);
                } else {
                    // disconnect full pages
                    list_remove(*queue, start);
                }
                start = next;
            }
        }

        Area *new_area = part_alloc->get_area(s, ContainerType::PAGE);
        if (new_area == NULL) {
            return NULL;
        }

        Section *new_section = (Section *)((uint8_t *)new_area);
        new_section->idx = 0;

        auto exponent = (Exponent)getContainerExponent(s, ContainerType::PAGE);
        new_section->partition_id = new_area->partition_id;
        new_section->setContainerExponent(exponent);
        new_section->setContainerType(ContainerType::PAGE);

        new_section->next = NULL;
        new_section->prev = NULL;

        new_section->claimAll();
        start = (Page *)new_section->getCollection(0, exponent);
        start->init(0, exponent);

        part_alloc->page_count++;
        list_enqueue(*queue, start);
        return start->getBlock((uint32_t)s);
    }

    void *alloc_slab(size_t s)
    {
        auto totalSize = sizeof(Area) + s;
        Area *area = part_alloc->get_area(totalSize, ContainerType::PAGE);
        if (area == NULL) {
            return NULL;
        }
        auto section = (Section *)area;
        area->reserveAll();
        section->reserveAll();
        section->setContainerType(ContainerType::SLAB);
        return &section->collections[0];
    }

    inline Pool *alloc_pool(size_t s)
    {
        auto sfree_section = get_free_section(s, ContainerType::POOL);
        if (sfree_section == NULL) {
            return NULL;
        }

        unsigned int coll_idx = sfree_section->reserveNext();
        auto exp = sfree_section->getContainerExponent();
        Pool *p = (Pool *)sfree_section->getCollection(coll_idx, exp);
        p->init(coll_idx, (uint32_t)s, exp);
        return p;
    }

    inline void *alloc_from_pool(size_t s)
    {
        auto queue = &part_alloc->pools[sizeToPool(s)];
        Pool *start = queue->head;

        if (start != NULL) {
            if (start->free == NULL) {
                if (start->extendPool()) {
                    previous_pool = start;
                    previous_size = s;
                    return start->getFreeBlock();
                }
                list_remove(*queue, start);
            } else {
                previous_pool = start;
                previous_size = s;
                return start->getFreeBlock();
            }
        }

        start = alloc_pool(s);
        if (start == NULL) {
            return NULL;
        }

        part_alloc->pool_count++;
        list_enqueue(*queue, start);
        set_previous_pool(start);
        previous_size = s;
        return start->getFreeBlock();
    }

    inline void *try_alloc_from_pool(size_t s)
    {
        void *ptr = alloc_from_pool(s);
        if (ptr == NULL) {
            previous_pool = NULL;
            // claim a new partition set
            // try again
            auto new_partition_set_idx = reserve_any_partition_set_for(main_index);
            if (new_partition_set_idx != -1) {
                part_alloc = &partition_allocators[new_partition_set_idx];
                ptr = alloc_from_pool(s);
            }
        }
        return ptr;
    }
};

#define ALLOC_DEFINE(tid)                              \
    {                                                  \
        tid, &partition_allocators[tid], NULL, 0, 0, 0 \
    }
#define ALLOC_LAYOUT TH1024(ALLOC_DEFINE)
static cache_align Allocator allocator_list[MAX_THREADS] = ALLOC_LAYOUT;

/*
    how do I attach an allocator to the thread.

 */

// MemoryBlock:
// Buff, Vec, Str -- all have direct access to heap
//  - reallocate. - reserve.
//
// Allocator
//   Section
//      pagesize
//      pages[]
//   Page
//      number of free bytes
//      number of allocated bytes
//      first free offset.
//   Map a requested size to a section.
//      map an address to
// handle memory within scope limits.
struct _MemoryBlock
{
    enum mtype {
        M_STACK, // src memory is on the local stack
        M_HEAP, // src memory was source from malloc. large requests, or pools
                // unable to hand out memory.
        M_POOL, // src memory was sourced from a linear thread local pool. small
                // requests.
        M_FIXED_POOL, // src memory was sourced from a fixed pool of medium sized
                      // requests. count limit. falls back on heap.
        M_NULL, // memory allocation failure.
    };

    void *src_mem; // pointer to allocated memory.
    mtype src_type; // where is the current memory sourced from.
    size_t req_size; // size requested.
    size_t src_size; // size returned. Will be >= to requested size.

    _MemoryBlock()
    {
        src_type = M_NULL;
        req_size = 0;
        src_size = 0;
        src_mem = NULL;
    }

    _MemoryBlock(void *m, size_t s)
    {
        src_type = M_STACK;
        src_size = s;
        src_mem = m;
        req_size = s;
    }

    _MemoryBlock(size_t s) { alloc(s); }

    ~_MemoryBlock()
    {
        if (src_type != M_HEAP) {
            // free(src_mem);
        } else if (src_type == M_FIXED_POOL) {
            // Allot::DeallocFixed(src_mem, src_size);
        }
    }

    void *alloc(size_t s)
    {
        if (src_type == M_NULL) {
            // src_size = s;
            // src_mem = fixed_pool_allocate(s);
            // if(src_mem == NULL)
            // {
            //    src_mem = malloc(size);
            //    src_type = M_HEAP;
            // }
            // else
            // {
            //    src_type = M_FIXED_POOL;
            // }
        } else {
            // memory has not been released .. error
        }
        return NULL;
    }

    void free()
    {
        if (src_type != M_NULL) {
        }
    }

    void *realloc(size_t s)
    {
        //
        return NULL;
    }
};

#endif /* Malloc_h */
