

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

#ifndef Malloc_h
#define Malloc_h

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef WINDOWS
#include <intrin.h>
#include <memoryapi.h>
uint64_t rdtsc() { return __rdtsc(); }
#else
#include <sys/mman.h>
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

#define SIZE_CLS_1 17 // 2 ^ 17   // 16k
#define SIZE_CLS_2 19 //          // 128k
#define SIZE_CLS_3 22 //          // 4Mb
#define SIZE_CLS_4 25 //          // 32Mb
#define SIZE_CLS_5 27 //          // 128Mb
#define SIZE_CLS_6 28 //          // 256Mb

#define SECTION_SIZE (1ULL << 22ULL)

#define CACHE_LINE 64
#ifdef WINDOWS
#define cache_align __declspec(align(CACHE_LINE))
#else
#define cache_align __attribute__((aligned(CACHE_LINE)))
#endif

#define AREA_SIZE_SMALL (SECTION_SIZE * 8ULL)
#define AREA_SIZE_LARGE (SECTION_SIZE * 32ULL)
#define AREA_SIZE_HUGE (SECTION_SIZE * 64ULL)
#define MASK_FULL 0xFFFFFFFFFFFFFFFF

#define CACHED_POOL_COUNT 64
#define POOL_BIN_COUNT 17 * 8 + 2
#define HEADER_SIZE 64

#define AREA_SMALL_MAX_MEMORY AREA_SIZE_SMALL - HEADER_SIZE
#define AREA_LARGE_MAX_MEMORY AREA_SIZE_LARGE - HEADER_SIZE
#define SECTION_MAX_MEMORY SECTION_SIZE - HEADER_SIZE
#define PAGE_MAX_MEMORY AREA_SIZE_SMALL * 2 - HEADER_SIZE

#define MAX_SMALL_AREAS 192 // 32Mb over 6Gb
#define MAX_LARGE_AREAS 64 // 128Mb over 8Gb
#define MAX_HUGE_AREAS 64 // 256Mb over 16Gb

#define MAX_THREADS 1024 // don't change this...

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

const uint8_t partition_count = 7;
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
    return si.dwPageSize);
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

// if we allocate sections on 4mb alignment
struct heap_node
{
    size_t *_prev;
    size_t *_next;
    inline void setNext(heap_node *p) { _next = (size_t *)p; }
    inline void setPrev(heap_node *p) { _prev = (size_t *)p; }
    inline heap_node *getNext() { return (heap_node *)_next; }
    inline heap_node *getPrev() { return (heap_node *)_prev; }
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

static inline size_t align(size_t n)
{
    return (n + sizeof(intptr_t) - 1) & ~(sizeof(intptr_t) - 1);
}

union Block
{
    uint8_t *data;
    Block *next;
};

enum AllocSize {
    SMALL = 0, // < 64k
    MEDIUM, // < 512k
    LARGE // < 4megs
};

enum Exponent {
    EXP_PUNY = SIZE_CLS_1,
    EXP_SMALL = SIZE_CLS_2,
    EXP_MEDIUM = SIZE_CLS_3,
    EXP_LARGE = SIZE_CLS_4,
    EXP_HUGE = SIZE_CLS_5,
    EXP_GIGANTIC = SIZE_CLS_6
};

enum ContainerType {
    PAGE,
    POOL,
    SLAB,
};

inline int32_t getContainerExponent(size_t s, ContainerType t)
{
    if (t == PAGE) {
        if (s < SMALL_OBJECT_SIZE) {
            return SIZE_CLS_3;
        } else if (s < MEDIUM_OBJECT_SIZE) {
            return SIZE_CLS_4;
        } else if (s < HUGE_OBJECT_SIZE) {
            return SIZE_CLS_5;
        } else {
            return SIZE_CLS_6;
        }
    } else {
        if (s < SMALL_OBJECT_SIZE) {
            return SIZE_CLS_1;
        } else if (s < MEDIUM_OBJECT_SIZE) {
            return SIZE_CLS_2;
        } else {
            return SIZE_CLS_3;
        }
    }
}

inline AllocSize getAllocSize(size_t s)
{
    if (s < SMALL_OBJECT_SIZE) {
        return SMALL;
    } else if (s < MEDIUM_OBJECT_SIZE) {
        return MEDIUM;
    } else {
        return LARGE;
    }
}

// list functions
template <typename L>
inline bool list_isEmpty(L &list)
{
    return list.first == NULL;
}

template <typename L, typename T>
inline void list_remove(L &list, T *a)
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
inline void list_enqueue(L &list, T *a)
{
    a->setNext(NULL);
    a->setPrev(list.tail);
    if (list.tail != NULL) {
        list.tail->setNext(a);
        list.tail = a;
    } else {
        list.tail = list.head = a;
    }
}

template <typename L, typename T>
inline void list_insert_sort(L &list, T *a)
{
    if (list.tail == NULL) {
        list.tail = list.head = a;
        return;
    }

    if (list.tail < a) {
        return list_enqueue(list, a);
    }

    T *current = list.head;
    while (current->getNext() != NULL && current->getNext() < a) {
        current = current->getNext();
    }
    a->setPrev(current->getPrev());
    a->setNext(current->getNext());
    current->setPrev(a);
}

struct Area
{
    // The area is overlapping with the first section. And they share some
    // attributes.
    static const uintptr_t small_area_mask = 0xff;
    static const uintptr_t large_area_mask = 0xffff;
    static const uintptr_t ptr_mask = 0x0000ffffffffffff;
    static const uintptr_t inv_ptr_mask = 0xffff000000000000;
    struct List
    {
        Area *head;
        Area *tail;
        uint32_t thread_id;
        uintptr_t base_addr;
        AreaType type;
        size_t area_count;
        Area *previous_area;
    };

    bitmask mask;
    size_t thread_id;

private:
    // these members are shared with the first section in the memory block. so,
    // the first high 16 bits are reserved by the section.
    size_t size;
    Area *prev;
    Area *next;

    int32_t release_state;
    int32_t misc; // index in parent area.
public:
    inline bool isEmpty() { return mask.isEmpty_hi(); }
    inline bool isFree() { return isEmpty() && release_state; }
    inline void FreeIdx(uint8_t i)
    {
        mask.freeIdx_hi(i);
    }
    inline void ReserveIdx(uint8_t i) { mask.reserve_hi(i); }

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

    int8_t claimSection()
    {
        auto idx = mask.allocate_bit_hi();
        if (idx >= 0) {
            uint32_t numSet = countBits(mask.whole >> 32);
            auto area_size = getSize();
            if (area_size == AREA_SIZE_SMALL) {
                if (numSet >= 4) {
                    release_state = 1;
                }
            } else if (area_size == AREA_SIZE_LARGE) {
                if (numSet >= 16) {
                    release_state = 1;
                }
            } else {
                release_state = 1;
            }
            return idx;
        }
        return -1;
    }
    void claimAll()
    {
        release_state = 1;
        mask.reserveAll();
    }
    bool isFull()
    {
        if (mask.isFull_hi()) {
            return true;
        }
        if (getSize() == AREA_SIZE_SMALL) {
            return ((mask.whole >> 32) & small_area_mask) == small_area_mask;
        } else {
            return ((mask.whole >> 32) & large_area_mask) == large_area_mask;
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

    inline size_t getSize() { return (size & 0xffffffff00000000) >> 32; }
    inline void setSize(size_t s) { size |= s << 32; }

    static Area *fromAddr(uintptr_t p)
    {
        if (p < partitions_offsets[2]) {
            return (Area *)(p & ~(AREA_SIZE_SMALL - 1));
        } else if (p < partitions_offsets[3]) {
            return (Area *)(p & ~(AREA_SIZE_LARGE - 1));
        } else if (p < partitions_offsets[4]) {
            return (Area *)(p & ~(AREA_SIZE_HUGE - 1));
        } else {
            return (Area *)p;
        }
    }
};

struct Section
{
    struct Queue
    {
        Section *head;
        Section *tail;
    };
    // 24 bytes as the section header
    bitmask mask; // 32 pages bit per page.   // lower 32 bits per section/ high
                  // bits are for area
    size_t thread_id;

private:
    // An area and section can overlap, and the prev next pointer of an area will
    // always be under the 32tb range. top 16 bits area always zero.
    size_t asize; // lower 32 bits per section/ high bits are for area
    size_t container_type; // top 16 bits.
    size_t container_exponent; // top 16 bits.
public:
    int32_t misc;
    int32_t idx; // index in parent area.

    // links to sections.
    Section *prev;
    Section *next;

    uint8_t collections[];
    inline bool isConnected() { return prev != NULL || next != NULL; }

    uint8_t get_collection_count()
    {
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

    inline void FreeIdx(uint8_t i)
    {
        mask.freeIdx_lo(i);
        auto section_empty = mask.isEmpty_lo();
        if (section_empty) {
            // what partition are we in.
            auto area = Area::fromAddr((uintptr_t)this);
            switch (getContainerType()) {
            case POOL: {
                area->FreeIdx(idx);
                break;
            }
            case PAGE: {
                auto cexp = getContainerExponent();
                // if small page
                if (cexp != EXP_MEDIUM) {
                    area->mask.freeAll();
                } else {
                    area->FreeIdx(idx);
                }
                break;
            }
            default: // SLAB
                area->mask.freeAll();
                break;
            }
        }
    }

    inline void ReserveIdx(uint8_t i)
    {
        mask.reserve_lo(i);
        auto area = Area::fromAddr((uintptr_t)this);
        area->ReserveIdx(idx);
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
    inline size_t getSectionSize() const { return asize & 0xffffffff; };
    inline size_t getSize() const { return asize; }

    inline bool isFull() const
    {
        switch (getContainerExponent()) {
        case Exponent::EXP_PUNY: {
            return (mask.whole & 0xffffffff) == 0xffffffff;
        }
        case Exponent::EXP_SMALL: {
            return (mask.whole & 0xff) == 0xff;
        }
        default: {
            return (mask.whole & 0x1) == 0x1;
        }
        }
    }

    inline void *findCollection(void *p) const
    {
        const ptrdiff_t diff = (uint8_t *)p - (uint8_t *)&collections[0];
        const auto exp = (container_exponent & 0xffff000000000000) >> 48;
        const auto page_size = 1 << exp;
        const int32_t idx = (int32_t)((size_t)diff >> exp);
        return (void *)((uint8_t *)&collections[0] + page_size * idx);
    }

    inline uintptr_t getCollection(int8_t idx, Exponent exp) const
    {
        switch (exp) {
        case Exponent::EXP_PUNY: {
            return (uintptr_t)((uint8_t *)&collections[0] + (1 << Exponent::EXP_PUNY) * idx);
        }
        case Exponent::EXP_SMALL: {
            return (uintptr_t)((uint8_t *)&collections[0] + (1 << Exponent::EXP_SMALL) * idx);
        }
        default: {
            return (uintptr_t)((uint8_t *)&collections[0] + (1 << exp) * idx);
        }
        }
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
            section->FreeIdx(idx);
        }
    }

    void init(int8_t pidx, uint32_t blockSize, Exponent partition)
    {
        auto psize = 1 << partition;

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
            section->ReserveIdx(idx);
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
                uintptr_t start = (uintptr_t)&blocks[0];
                uintptr_t end = (uintptr_t)((uint8_t *)start + (num_committed * block_size));

                if (start != end) {
                    start = end;
                }
                free = (Block *)start;
                free->next = NULL;
                num_committed++;
                if ((num_available - num_committed) > 0 && block_size < os_page_size) {
                    if (start != end) {
                        start = end;
                    }
                    uintptr_t page_end = align_up(start, os_page_size);
                    auto offset = page_end - start;
                    auto steps = min(offset / block_size, num_available - num_committed);
                    Block *block = free;
                    for (uint32_t i = 1; i <= steps; i++) {
                        uintptr_t block_addre = (uintptr_t)((uint8_t *)block + block_size);
                        Block *next = (Block *)block_addre;
                        block->next = next;
                        block = next;
                        num_committed++;
                    }
                    block->next = NULL;
                }
                return true;
            }
        }
        return false;
    }

public:
    uint8_t blocks[];
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
        if ((used_memory + s) > total_memory) {
            return false;
        }
        if (s <= max_block && s >= min_block) {
            return true;
        }
        return false;
    }

    inline void *getBlock(uint32_t s)
    {
        void *ptr = NULL;
        if ((ptr = (char *)find_fit(s)) != NULL) {
            place(ptr, s);
        }
        used_memory += s;
        if (num_allocations == 0) {
            Section *section = (Section *)((uintptr_t)this & ~(SECTION_SIZE - 1));
            section->ReserveIdx(idx);
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
            section->FreeIdx(idx);
        }
    }

    inline void extend()
    {
        *start = 0;
        *(start + WSIZE) = DSIZE | 1; /* Prologue header */
        *(start + DSIZE) = (DSIZE | 1); /* Prologue footer */
        *(start + WSIZE + DSIZE) = 1; /* Epilogue header */
        start = start + DSIZE * 2;

        heap_block *hb = (heap_block *)start;
        list_enqueue(free_nodes, (heap_node *)start);
        hb->set_header(total_memory, 0);
        hb->set_footer(total_memory, 0);
        hb->next()->set_header(0, 1);
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
        list_remove(free_nodes, (heap_node *)bp);
        if ((csize - asize) >= 16) {
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
            auto bsize = header & ~0x7;
            if (asize <= bsize) {
                list_remove(free_nodes, current);
                return current;
            }
            current = current->getNext();
        }
        return NULL;
    }

    void init(int8_t pidx, Exponent partition)
    {
        auto psize = 1 << partition;
        uintptr_t section_end = align_up((uintptr_t)this, psize);
        auto remaining_size = section_end - (uintptr_t)&blocks[0];

        auto block_memory = psize - sizeof(Page) - sizeof(Area);
        auto header_footer_offset = sizeof(uintptr_t) * 5;

        idx = pidx;
        used_memory = 0;
        total_memory = (uint32_t)((min(remaining_size, block_memory)) - header_footer_offset);
        max_block = total_memory;
        min_block = sizeof(uintptr_t);
        num_allocations = 0;
        start = &blocks[0];
        next = NULL;
        prev = NULL;
        extend();
    }

public:
    uint8_t blocks[];
    inline Page *getPrev() const { return prev; }
    inline Page *getNext() const { return next; }
    inline void setPrev(Page *p) { prev = p; }
    inline void setNext(Page *n) { next = n; }
};

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
    return VirtualFree(ptr, 0, MEM_RELEASE) == 0);
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
    if (p == start && start != NULL) {
        VirtualUnlock(start, csize); // VirtualUnlock after MEM_RESET removes the memory from the working set
    }
    if (p != start)
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

static void *alloc_memory_aligned(void *base, size_t size, size_t alignment,
    bool commit)
{
    // alignment is smaller than a page size or not a power of two.
    if (!(alignment >= os_page_size && POWER_OF_TWO(alignment)))
        return NULL;
    size = align_up(size, os_page_size);
    if (size >= (SIZE_MAX - alignment))
        return NULL;

    void *ptr = alloc_memory(base, size, commit);
    if (ptr == NULL)
        return NULL;

    if (((uintptr_t)ptr % alignment != 0)) {
        release_memory(ptr, size, commit);
        size_t over_size = size + alignment;
        ptr = alloc_memory(base, over_size, commit);
        if (ptr == NULL)
            return NULL;

        if (((uintptr_t)ptr % alignment) == 0) {
            decommit_memory((uint8_t *)ptr + size, over_size - size);
            return ptr;
        }
        void *aligned_p = (void *)align_up((uintptr_t)ptr, alignment);
        release_memory(ptr, over_size, commit);
        ptr = alloc_memory(aligned_p, size, commit);
    }

    return ptr;
}

struct Partition
{
    Area::List area_01;
    Area::List area_2;
    Area::List area_3;
    static Area::List area_4;
    static Area::List area_5;

    void *get_next_address(Area::List *area_queue, uint64_t size,
        uint64_t alignment)
    {
        if (area_queue->head == NULL) {
            return (void *)area_queue->base_addr;
        }
        size = align_up(size, alignment);
        uint64_t delta = (uint64_t)((uint8_t *)area_queue->base_addr - (uint8_t *)area_queue->tail);
        if (delta < size && (area_queue->tail != area_queue->head)) {
            Area *current = area_queue->head;
            while (current != area_queue->tail) {
                size_t c_size = current->getSize();
                size_t c_end = c_size + (size_t)(uint8_t *)current;
                Area *next = current->getNext();
                delta = (uint64_t)((uint8_t *)next - c_end);
                if (delta >= size) {
                    return (void *)c_end;
                }
                current = next;
            }
            return NULL;
        } else {
            return (void *)align_up((uintptr_t)(((uint8_t *)area_queue->tail) + area_queue->tail->getSize()),
                alignment);
        }
    }

    Area *alloc_area(Area::List *area_queue, uint64_t area_size,
        uint64_t alignment)
    {
        void *aligned_addr = get_next_address(area_queue, area_size, alignment);
        Area *new_area = (Area *)alloc_memory_aligned(aligned_addr, area_size, alignment, true);
        if (new_area == NULL) {
            return NULL;
        }
        new_area->mask = { 0 };
        new_area->thread_id = area_queue->thread_id;
        new_area->setNext(NULL);
        new_area->setPrev(NULL);
        list_insert_sort(*area_queue, new_area);
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
            if (a == area_01.previous_area) {
                area_01.previous_area = NULL;
            }

            break;
        }
        case AT_FIXED_128: {
            area_2.area_count--;
            list_remove(area_2, a);
            if (a == area_2.previous_area) {
                area_2.previous_area = NULL;
            }
            break;
        }
        default: {
            area_3.area_count--;
            list_remove(area_3, a);
            if (a == area_3.previous_area) {
                area_3.previous_area = NULL;
            }
            break;
        }
        };
        reset_memory(a, a->getSize());
    }

    Area *get_free_area(size_t s, AreaType t)
    {
        Area::List *current_queue = NULL;
        Area *previous_area = NULL;
        size_t area_size = AREA_SIZE_SMALL;
        size_t alignement = area_size;
        switch (t) {
        case AT_FIXED_32: {
            if (area_01.area_count >= MAX_SMALL_AREAS) {
                if (area_2.area_count >= MAX_LARGE_AREAS) {
                    return NULL;
                }
                previous_area = area_2.previous_area;
                current_queue = &area_2;
                area_size = AREA_SIZE_LARGE;
                alignement = area_size;
            }
            previous_area = area_01.previous_area;
            current_queue = &area_01;
            break;
        }
        case AT_FIXED_128: {
            area_size = AREA_SIZE_LARGE;
            if (area_2.area_count >= MAX_LARGE_AREAS) {
                if (area_3.area_count >= MAX_HUGE_AREAS) {
                    return NULL;
                }
                previous_area = area_3.previous_area;
                current_queue = &area_3;
                area_size = AREA_SIZE_HUGE;
                alignement = area_size;
            }
            previous_area = area_2.previous_area;
            current_queue = &area_2;
            alignement = area_size;
            break;
        }
        case AT_FIXED_256: {
            area_size = AREA_SIZE_HUGE;
            if (area_3.area_count >= MAX_HUGE_AREAS) {
                return NULL;
            }
            previous_area = area_3.previous_area;
            current_queue = &area_3;
            alignement = area_size;
            break;
        }
        default: {
            previous_area = area_3.previous_area;
            current_queue = &area_3;
            area_size = s;
            alignement = AREA_SIZE_HUGE;
            break;
        }
        };

        // the areas are empty
        Area *new_area = NULL;
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

        if (new_area == NULL) {
            if (area_size < os_page_size) {
                area_size = os_page_size;
            }
            new_area = alloc_area(current_queue, area_size, alignement);
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
};

// create our 1024 static area containers by macro expansion
#define PARTITION_01 ((uintptr_t)2 << 40)
#define PARTITION_2 ((uintptr_t)8 << 40)
#define PARTITION_3 ((uintptr_t)16 << 40)

#define P1(tid)                                                  \
    {                                                            \
        {                                                        \
            NULL,                                                \
            NULL,                                                \
            tid,                                                 \
            (tid) * (SZ_GB * 6) + PARTITION_01,                  \
            AreaType::AT_FIXED_32,                               \
            0,                                                   \
            NULL                                                 \
        },                                                       \
            { NULL,                                              \
                NULL,                                            \
                tid,                                             \
                (tid) * (SZ_GB * 8) + PARTITION_2,               \
                AreaType::AT_FIXED_128,                          \
                0,                                               \
                NULL },                                          \
        {                                                        \
            NULL, NULL, tid, (tid) * (SZ_GB * 16) + PARTITION_3, \
                AreaType::AT_VARIABLE, 0, NULL                   \
        }                                                        \
    }

#define P10(tid)                                                              \
    P1(tid), P1(tid + 1), P1(tid + 2), P1(tid + 3), P1(tid + 4), P1(tid + 5), \
        P1(tid + 6), P1(tid + 7), P1(tid + 8), P1(tid + 9)
#define P100(tid)                                                      \
    P10(tid * 100), P10(tid * 100 + 10), P10(tid * 100 + 20),          \
        P10(tid * 100 + 30), P10(tid * 100 + 40), P10(tid * 100 + 50), \
        P10(tid * 100 + 60), P10(tid * 100 + 70), P10(tid * 100 + 80), \
        P10(tid * 100 + 90)
#define P1000                                                               \
    P100(0), P100(1), P100(2), P100(3), P100(4), P100(5), P100(6), P100(7), \
        P100(8), P100(9)
#define PART_LAYOUT                                                               \
    {                                                                             \
        P1000, P1(1000), P1(1001), P1(1002), P1(1003), P1(1004), P1(1005),        \
            P1(1006), P1(1007), P1(1008), P1(1009), P1(1010), P1(1011), P1(1012), \
            P1(1013), P1(1014), P1(1015), P1(1016), P1(1017), P1(1018), P1(1019), \
            P1(1020), P1(1021), P1(1022), P1(1023)                                \
    }

Area::List Partition::area_4 = {
    NULL, NULL, 0, ((uintptr_t)32 << 40), AreaType::AT_VARIABLE, 0, NULL
};
Area::List Partition::area_5 = {
    NULL, NULL, 0, ((uintptr_t)32 << 40), AreaType::AT_VARIABLE, 0, NULL
};

cache_align Partition memory_partitions[MAX_THREADS] = PART_LAYOUT;

static std::atomic<int32_t> global_thread_idx = { -1 };

struct Allocator
{
private:
    static thread_local size_t _thread_id;
    static thread_local int32_t _thread_idx;

    inline bool is_main() { return _thread_idx == 0; }

    static int8_t size_to_mask(size_t s, int8_t *count)
    {
        if (s > SECTION_SIZE * 8) {
            *count = 0;
            return 0;
        } else {
            auto parts = (s / SECTION_SIZE) + 1;
            *count = parts;
            return (int8_t)(1 << parts) - 1;
        }
    }

    void free_area(Area *area)
    {
        size_t size = area->getSize();
        if (size == AREA_SIZE_SMALL) {
            thread_partitions->free_area(area, AreaType::AT_FIXED_32);
        } else if (size == AREA_SIZE_LARGE) {
            thread_partitions->free_area(area, AreaType::AT_FIXED_128);
        } else if (size == AREA_SIZE_HUGE) {
            thread_partitions->free_area(area, AreaType::AT_FIXED_256);
        } else {
            thread_partitions->free_area(area, AreaType::AT_VARIABLE);
        }
    }

    Area::List *get_area_queue(Area *area)
    {
        size_t size = area->getSize();
        if (size == AREA_SIZE_SMALL) {
            return &thread_partitions->area_01;
        } else if (size == AREA_SIZE_LARGE) {
            return &thread_partitions->area_2;
        } else {
            return &thread_partitions->area_3;
        }
    }

    Area *get_area(size_t size)
    {
        Area *curr_area = NULL;
        if (size < SECTION_SIZE - sizeof(Section)) {
            curr_area = thread_partitions->get_free_area(size, AreaType::AT_FIXED_32);
        } else if (size < AREA_SIZE_SMALL - sizeof(Area)) {
            curr_area = thread_partitions->get_free_area(size, AreaType::AT_FIXED_128);
        } else if (size < AREA_SIZE_LARGE - sizeof(Area)) {
            curr_area = thread_partitions->get_free_area(size, AreaType::AT_FIXED_256);
        } else {
            curr_area = thread_partitions->get_free_area(size, AreaType::AT_VARIABLE);
        }

        return curr_area;
    }

    Area *get_area_and_claim(size_t size, int32_t *outSectionIdx)
    {
        Area *curr_area = get_area(size);
        if (curr_area == NULL) {
            return NULL;
        }
        *outSectionIdx = thread_partitions->claim_section(curr_area);
        if (*outSectionIdx < 0) {
            // failed to claim a section from a free area... ???
            return NULL;
        }
        return curr_area;
    }

    Section *alloc_section(size_t size)
    {
        int32_t section_idx;
        auto new_area = get_area_and_claim(size, &section_idx);
        if (new_area == NULL) {
            return NULL;
        }

        Section *section = (Section *)((uint8_t *)new_area + SECTION_SIZE * section_idx);
        section->idx = section_idx;
        return section;
    }

    void free_section(Section *s) { free_memory(s, SECTION_SIZE); }

public:
    Partition *thread_partitions;

    // sections local to this thread with free pages or pools
    Section::Queue sections;
    // free pages that have room for various size allocations.
    Page::Queue pages;
    // how man pages in total have been allocated.
    uint32_t page_count;

    // free pools of various sizes.
    Pool::Queue pools[POOL_BIN_COUNT];
    // how many pools in total have been allocated.
    uint32_t pool_count;

    static inline size_t thread_id() { return (size_t)&_thread_id; }

    Allocator()
    {
        auto nidx = std::atomic_fetch_add_explicit(&global_thread_idx, 1,
            std::memory_order_acq_rel);
        _thread_idx = nidx + 1;
        thread_partitions = &memory_partitions[_thread_idx];

        pages = { NULL, NULL };
        sections = { NULL, NULL };
        pool_count = 0;
        page_count = 0;
        // pools and cached_pool
        init_cache();
    }

    void *malloc(size_t s)
    {
        if (s == 0)
            return NULL;

        auto asize = align(s);
        if (asize < (SECTION_SIZE - 64)) {
            return alloc_from_pool(asize);
        } else if (asize < (AREA_SIZE_LARGE - 64)) {
            // allocate form the large page
            auto page = get_free_page(asize);
            return page->getBlock((uint32_t)asize);
        } else {
            return get_free_huge_block(asize);
        }
    }

    void free(void *p)
    {
        if (p == NULL)
            return;
        if ((uintptr_t)p < partitions_offsets[2]) {
            Section *section = (Section *)((uintptr_t)p & ~(SECTION_SIZE - 1));
            // There are only pools in this area
            if (_thread_idx == section->thread_id) {
                auto pool = (Pool *)section->findCollection(p);
                pool->freeBlock(p);
                if (!pool->isConnected()) {
                    auto queue = &pools[sizeToPool(pool->block_size)];
                    list_enqueue(*queue, pool);
                    if (!section->isConnected()) {
                        list_enqueue(sections, section);
                    }
                }
            }
        } else if ((uintptr_t)p < partitions_offsets[3]) {
            Section *section = (Section *)((uintptr_t)p & ~(AREA_SIZE_LARGE - 1));
            // if it is page section, free
            if (_thread_idx == section->thread_id) {
                if (section->getContainerType() == PAGE) {
                    auto page = (Page *)section->findCollection(p);
                    page->free(p, true);
                    // if the free pools list is empty.
                    if (!page->isConnected()) {
                        // reconnect
                        list_enqueue(pages, page);
                        if (!section->isConnected()) {
                            list_enqueue(sections, section);
                        }
                    }
                } else {
                    auto pool = (Pool *)section->findCollection(p);
                    pool->freeBlock(p);
                    if (!pool->isConnected()) {
                        // reconnect
                        auto queue = &pools[sizeToPool(pool->block_size)];
                        list_enqueue(*queue, pool);
                        if (!section->isConnected()) {
                            list_enqueue(sections, section);
                        }
                    }
                }
            }
        } else if ((uintptr_t)p < partitions_offsets[4]) {
            Section *section = (Section *)((uintptr_t)p & ~(AREA_SIZE_HUGE - 1));
            // if it is page section, free
            if (_thread_idx == section->thread_id) {
                if (section->getContainerType() == PAGE) {
                    auto page = (Page *)section->findCollection(p);
                    page->free(p, true);
                    // if the pool is disconnected from the queue
                    if (!page->isConnected()) {
                        // reconnect
                        list_enqueue(pages, page);
                        if (!section->isConnected()) {
                            list_enqueue(sections, section);
                        }
                    }
                } else // SLAB
                {
                    Area *area = (Area *)section;
                    free_area(area);
                }
            }
        } else {
            // do nothing
        }
    }

    // this hands out a node inside of a raw page.
    // it is assumed that the called knows how to
    inline void *raw_malloc(size_t s)
    {
        int asize = align(s);
        // find a free page
        // if no free page... get from system.
        //      add to free lists.
        // find a free block
        return NULL;
    }

    inline void raw_free(void *bp)
    {
        //
    }

    inline int raw_resize(int incr)
    {
        // check next block in list.
        // if able to resize, return true
        return 0;
    }

private:
    inline int8_t addrToPartition(void *addr) const
    {
        return 22 - __builtin_clzll((uintptr_t)addr);
    }

    inline uint8_t sizeToPool(size_t as)
    {
        const int bmask = ~0x7f;
        if ((bmask & as) == 0) {
            return as >> 3;
        } else {
            const int tz = __builtin_clzll(as);
            size_t numWidth = 60 - tz;
            return 8 + ((numWidth - 3) << 3) + ((as >> (numWidth)&0x7));
        }
    }

    bool collect()
    {
        // for each area this thread has allocated.
        //
        return false;
    }

    bool try_free_area(void *p)
    {
        Area *area = Area::fromAddr((uintptr_t)p);
        if (area->isFree()) {

            // all sections should be free and very likely in the free sections list.
            int num_sections = area->get_section_count();
            /*
            auto queue = get_area_queue(area);
            if(queue->head == queue->tail)
            {
                return false;
            }
             */
            for (int i = 0; i < num_sections; i++) {
                Section *section = (Section *)((uint8_t *)area + SECTION_SIZE * i);
                int num_collections = section->get_collection_count();
                Exponent exp = section->getContainerExponent();
                ContainerType ctype = section->getContainerType();
                if (ctype == POOL) {
                    for (int j = 0; j < num_collections; j++) {
                        auto pool = (Pool *)section->getCollection(j, exp);
                        auto queue = &pools[sizeToPool(pool->block_size)];
                        list_remove(*queue, pool);
                    }
                } else if (ctype == PAGE) {
                    for (int j = 0; j < num_collections; j++) {
                        auto page = (Page *)section->getCollection(j, exp);
                        list_remove(pages, page);
                    }
                }
                list_remove(sections, section);
            }
            free_area(area);
            return true;
        }
        return false;
    }

    Section *get_free_section(size_t s, ContainerType t)
    {
        auto exponent = getContainerExponent(s, t);
        auto free_section = sections.head;
        if (free_section) {
            // find free section.
            while (free_section != NULL) {
                auto next = free_section->next;
                if (free_section->getContainerExponent() == exponent) {
                    if (!free_section->isFull()) {
                        break;
                    } else {
                        list_remove(sections, free_section);
                    }
                }
                free_section = next;
            }
        }

        if (free_section == NULL) {
            auto new_section = alloc_section(s);
            if (new_section == NULL) {
                return NULL;
            }
            new_section->thread_id = _thread_idx;
            new_section->setContainerExponent(exponent);
            new_section->setContainerType(t);
            new_section->next = NULL;
            new_section->prev = NULL;
            list_enqueue(sections, new_section);
            free_section = new_section;
        }
        return free_section;
    }

    Page *get_free_page(size_t s)
    {
        Page *start = NULL;
        if (pages.head != NULL) {
            start = pages.head;
            while (start != NULL) {
                auto next = start->next;
                if (start->has_room(s)) {
                    return start;
                } else {
                    // disconnect full pages
                    list_remove(pages, start);
                }
                start = next;
            }
            if (start != NULL) {
                return start;
            }
        }

        auto sfree_section = get_free_section(s, ContainerType::PAGE);
        if (sfree_section == NULL) {
            return NULL;
        }

        unsigned int coll_idx = sfree_section->mask.firstFree_lo();
        sfree_section->mask.reserve_lo(coll_idx);
        auto exp = sfree_section->getContainerExponent();
        start = (Page *)sfree_section->getCollection(coll_idx, exp);
        start->init(coll_idx, exp);
        if (s >= SECTION_MAX_MEMORY) {
            sfree_section->mask.reserveAll();
        }
        page_count++;
        list_enqueue(pages, start);
        return start;
    }

    void *get_free_huge_block(size_t s)
    {
        auto totalSize = sizeof(Area) + s;
        auto section = (Section *)get_area(totalSize);
        section->mask.reserveAll();
        section->setContainerType(ContainerType::SLAB);
        return &section->collections[0];
    }

    Pool *alloc_pool(size_t s)
    {
        auto sfree_section = get_free_section(s, ContainerType::POOL);
        if (sfree_section == NULL) {
            return NULL;
        }
        // Allocate pool
        unsigned int coll_idx = sfree_section->mask.firstFree_lo();
        sfree_section->mask.reserve_lo(coll_idx);
        auto exp = sfree_section->getContainerExponent();
        Pool *p = (Pool *)sfree_section->getCollection(coll_idx, exp);
        p->init(coll_idx, (uint32_t)s, exp);
        return p;
    }

    void *alloc_from_pool(size_t s)
    {
        auto queue = &pools[sizeToPool(s)];
        Pool *start = queue->tail;

        if (start != NULL) {
            if (start->free == NULL) {
                if (start->extendPool()) {
                    goto fin;
                }
                list_remove(*queue, start);
            } else {
                if (start->isAlmostFull()) {
                    // if the next allocation will cause it to fill
                    // pre-emptively remove it from the free list
                    list_remove(*queue, start);
                }
                goto fin;
            }
        }

        start = alloc_pool(s);
        if (start == NULL) {
            return NULL;
        }

        pool_count++;
        list_enqueue(*queue, start);
    fin:
        start->num_used++;
        auto res = start->free;
        start->free = res->next;

        return res;
    }

    void init_cache()
    {
        //
        for (int i = 0; i < 8; i++) {
            pools[i] = { NULL, NULL, i * 8UL };
        }
        size_t base_nums[8] = { 0b1000000, 0b1001000, 0b1010000, 0b1011000,
            0b1100000, 0b1101000, 0b1110000, 0b1111000 };

        for (int i = 0; i < 16; i++) {
            int base = 8 + i * 8;
            pools[base] = { NULL, NULL, base_nums[0] };
            pools[base + 1] = { NULL, NULL, base_nums[1] };
            pools[base + 2] = { NULL, NULL, base_nums[2] };
            pools[base + 3] = { NULL, NULL, base_nums[3] };
            pools[base + 4] = { NULL, NULL, base_nums[4] };
            pools[base + 5] = { NULL, NULL, base_nums[5] };
            pools[base + 6] = { NULL, NULL, base_nums[6] };
            pools[base + 7] = { NULL, NULL, base_nums[7] };
            for (int j = 0; j < 8; j++) {
                base_nums[j] = base_nums[j] << 1;
            }
        }
        pools[POOL_BIN_COUNT - 2] = { NULL, NULL, base_nums[0] };
        pools[POOL_BIN_COUNT - 1] = { NULL, NULL, base_nums[1] };
    }
};

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
