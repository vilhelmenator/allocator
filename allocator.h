
/*
    incrementing hints is atomic.

    base reference heap is in static. 2 - 24tb
    base large reference heap is also static. 24 - 30tb
    populate the base area pools with static thread_count * area_size;
    each thread, asks for an area if size is less than 32 megs - area header
   size;

    else you just call virtual alloc with a base address of larger than
   0x00001e0000000000.

    each thread, tests to see if there are any free areas in its local space, if
   not. then one of the base areas is reserverd for it.

    fetch the base area and commit the memory.

    each thread is free to decommit its areas, when they become empty.

    reserve base areas * num_threads;
 */

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
uint64_t rdtsc() {
  unsigned int lo, hi;
  __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}
#endif
#include <atomic>
#include <thread>

#define WSIZE 4 /* Word size in bytes */
#define DSIZE 8 /* Double word size in bytes */
#define SZ_KB 1024ULL
#define SZ_MB (SZ_KB * SZ_KB)
#define SZ_GB (SZ_MB * SZ_MB)

#define max(x, y) ((x) > (y) ? (x) : (y))
#define min(x, y) ((x) < (y) ? (x) : (y))

#define memory_block(x) memory_block_##(x)
#define memory_block_0(x) (_MemoryBlock(x);)
#define memory_block_1(x) (_MemoryBlock(alloca(x), x);)

#define MIN_ALLOC_SIZE sizeof(void *)
#define MAX_SMALL_SIZE 128 * MIN_ALLOC_SIZE
#define NUM_FREE_FAST_PAGES MAX_SMALL_SIZE / MIN_ALLOC_SIZE
#define MAX_RAW_ALLOC_SIZE 256 * MIN_ALLOC_SIZE

#define DEFAULT_OS_PAGE_SIZE 4096ULL
#define SMALL_OBJECT_SIZE DEFAULT_OS_PAGE_SIZE * 4    // 16k
#define DEFAULT_PAGE_SIZE SMALL_OBJECT_SIZE * 4       // 64kb
#define MEDIUM_OBJECT_SIZE DEFAULT_PAGE_SIZE * 4      // 128k
#define DEFAULT_MID_PAGE_SIZE MEDIUM_OBJECT_SIZE * 4  // 512kb
#define LARGE_OBJECT_SIZE DEFAULT_MID_PAGE_SIZE * 4   // 2 megs
#define DEFAULT_LARGE_PAGE_SIZE LARGE_OBJECT_SIZE * 2 // 4meg

#define BASE_PAGE_EXPONENT 16
#define BASE_PAGE_EXPONENT_INCR 3

// larger than 2 meg objects go into 1 section, the same size as the object.
#define SECTION_SIZE (1ULL << 22ULL)

#define CACHE_LINE 64
#ifdef WINDOWS
#define cache_align __declspec(align(CACHE_LINE))
#else
#define cache_align __attribute__((aligned(CACHE_LINE)))
#endif

#define AREA_SIZE (SECTION_SIZE * 8ULL)
#define MASK_FULL 0xFFFFFFFFFFFFFFFF
#define HINT_BASE ((uintptr_t)2 << 40) // 2TiB start
#define HINT_MAX                                                               \
  ((uintptr_t)30                                                               \
   << 40) // wrap after 30TiB (area after 32TiB is used for huge OS pages)

// 4 bits to numtz bits
const uint8_t lt_bit_count[16][2] = {
    {4, 4}, // 0000
    {3, 0}, // 0001
    {2, 1}, // 0010
    {2, 0}, // 0011
    {1, 2}, // 0100
    {1, 0}, // 0101
    {1, 1}, // 0110
    {1, 0}, // 0111
    {0, 3}, // 1000
    {0, 0}, // 1001
    {0, 1}, // 1010
    {0, 0}, // 1011
    {0, 2}, // 1100
    {0, 0}, // 1101
    {0, 1}, // 1110
    {0, 0}, // 1111
};

const uint8_t bit_count[16][2] = {
    {0, 4}, // 0000
    {1, 3}, // 0001
    {1, 3}, // 0010
    {2, 2}, // 0011
    {1, 3}, // 0100
    {2, 2}, // 0101
    {2, 2}, // 0110
    {3, 1}, // 0111
    {1, 3}, // 1000
    {2, 2}, // 1001
    {2, 2}, // 1010
    {3, 1}, // 1011
    {2, 2}, // 1100
    {3, 1}, // 1101
    {3, 1}, // 1110
    {4, 0}, // 1111
};

// bit counting
inline uint8_t cz8(uint8_t c) {
  return c & 0xf ? bit_count[c & 0xf][0] : 4 + bit_count[c & 0xf0][0];
}

inline uint8_t co8(uint8_t c) {
  return c & 0xf0 ? bit_count[c & 0xf0][1] : 4 + bit_count[c & 0xf][1];
}

inline uint8_t cz16(uint16_t c) {
  return c & 0xff ? cz8((uint8_t)(0x00ff & c)) : 8 + cz8((uint8_t)(c >> 8));
}

inline uint8_t co16(uint16_t c) {
  return c & 0xff00 ? co8((uint8_t)(c >> 8)) : 8 + co8((uint8_t)(0x00ff & c));
}

inline uint8_t cz32(uint32_t c) {
  return c & 0xffff ? cz16((uint16_t)(0x0000ffff & c))
                    : 16 + cz16((uint16_t)(c >> 16));
}

inline uint8_t co32(uint32_t c) {
  return c & 0xffff0000 ? co16((uint16_t)(c >> 16))
                        : 16 + co16((uint16_t)(0x0000ffff & c));
}

inline uint8_t cz64(uint64_t c) {
  return c & 0xffffffff ? cz32((uint32_t)(0x00000000ffffffff & c))
                        : 32 + cz32((uint32_t)(c >> 32));
}

inline uint8_t co64(uint64_t c) {
  return c & 0xffffffff00000000 ? co32((uint32_t)(c >> 32))
                                : 32 + co32((uint32_t)(0x00000000ffffffff & c));
}

// leading trailing bit counts
inline uint8_t clz8(uint8_t c) {
  return c & 0xf ? lt_bit_count[c & 0xf][1] : 4 + lt_bit_count[c & 0xf0][1];
}

inline uint8_t ctz8(uint8_t c) {
  return c & 0xf0 ? lt_bit_count[c & 0xf0][0] : 4 + lt_bit_count[c & 0xf][0];
}

inline uint8_t clz16(uint16_t c) {
  return c & 0xff ? clz8((uint8_t)(0x00ff & c)) : 8 + clz8((uint8_t)(c >> 8));
}

inline uint8_t ctz16(uint16_t c) {
  return c & 0xff00 ? ctz8((uint8_t)(c >> 8)) : 8 + ctz8((uint8_t)(0x00ff & c));
}

inline uint8_t clz32(uint32_t c) {
  return c & 0xffff ? clz16((uint16_t)(0x0000ffff & c))
                    : 16 + clz16((uint16_t)(c >> 16));
}

inline uint8_t ctz32(uint32_t c) {
  return c & 0xffff0000 ? ctz16((uint16_t)(c >> 16))
                        : 16 + ctz16((uint16_t)(0x0000ffff & c));
}

inline uint8_t clz64(uint64_t c) {
  return c & 0xffffffff ? clz16((uint32_t)(0x00000000ffffffff & c))
                        : 32 + clz32((uint32_t)(c >> 32));
}

inline uint8_t ctz64(uint64_t c) {
  return c & 0xffffffff00000000
             ? ctz32((uint32_t)(c >> 32))
             : 32 + ctz32((uint32_t)(0x00000000ffffffff & c));
}

typedef union bitmask {
  uint64_t whole;
  uint32_t _w32[2];
  uint16_t _w16[4];
  uint8_t _w8[8];

  bool isFull() { return whole == 0xFFFFFFFFFFFFFFFF; }

  bool isEmpty() { return whole == 0; }

  void reserve(uint8_t bit) { whole |= (1 << bit); }

  int8_t firstFree() { return clz64(~whole); }

  int8_t allocate_bits(uint8_t numBits) {
    auto fidx = freeIdx(numBits);
    if (fidx != -1) {
      uint64_t subMask = ((1 << numBits) - 1) << fidx;
      whole |= subMask;
    }
    return fidx;
  }

  int8_t freeIdx(uint8_t numBits) {
    if (numBits == 0)
      return -1;
    if (!isFull()) {
      const int max_bits = sizeof(whole) * CHAR_BIT;
      auto firstZero = firstFree();
      if ((max_bits - firstZero) < numBits) {
        return -1;
      }
      if (numBits == 1) {
        return firstZero;
      }
      const int num_bytes = numBits / CHAR_BIT;
      while (firstZero < max_bits) {
        switch (num_bytes) {
        case 0:
          if (clz8(0xff & (uint8_t)(whole >> firstZero)) >= numBits) {
            return firstZero;
          }
        case 1:
          if (clz16(0xffff & (uint16_t)(whole >> firstZero)) >= numBits) {
            return firstZero;
          }
        case 2:
        case 3:
          if (clz32(0xffffffff & (uint32_t)(whole >> firstZero)) >= numBits) {
            return firstZero;
          }
        default:
          if (clz64((whole >> firstZero)) >= numBits) {
            return firstZero;
          }
        }
        firstZero += numBits;
      }
      if ((max_bits - firstZero) < numBits) {
        return firstZero;
      }
      return -1;
    } else {
      return -1;
    }
  }
} mask;

static size_t os_num_hardware_threads = std::thread::hardware_concurrency();
static inline size_t get_os_page_size() {
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

static inline uintptr_t align_up(uintptr_t sz, size_t alignment) {
  uintptr_t mask = alignment - 1;
  uintptr_t sm = (sz + mask);
  if ((alignment & mask) == 0) {
    return sm & ~mask;
  } else {
    return (sm / alignment) * alignment;
  }
}

struct spinlock {
  std::atomic<bool> lock_ = {0};

  void lock() noexcept {
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

  bool try_lock() noexcept {
    // First do a relaxed load to check if lock is free in order to prevent
    // unnecessary cache misses if someone does while(!try_lock())
    return !lock_.load(std::memory_order_relaxed) &&
           !lock_.exchange(true, std::memory_order_acquire);
  }

  void unlock() noexcept { lock_.store(false, std::memory_order_release); }
};

// if we allocate sections on 4mb alignment
struct heap_node {
  size_t *_prev;
  size_t *_next;
  inline void set_next(heap_node *p) { _next = (size_t *)p; }
  inline void set_prev(heap_node *p) { _prev = (size_t *)p; }
  inline heap_node *next() { return *(heap_node **)_next; }
  inline heap_node *prev() { return *(heap_node **)_prev; }
};

struct heap_block {
  uint8_t *data;

  inline unsigned int get_header() {
    return *(unsigned int *)((char *)&data - WSIZE);
  }

  inline unsigned int get_footer() {
    unsigned int size = *(unsigned int *)((char *)&data - WSIZE) & ~0x7;
    return *(unsigned int *)((char *)&data + size - DSIZE);
  }

  inline unsigned int get_Alloc(unsigned int v) { return v & 0x1; }

  inline unsigned int get_Size(unsigned v) { return v & ~0x7; }

  inline void set_header(unsigned int s, unsigned int v) {
    *(unsigned int *)((char *)&data - WSIZE) = (s | v);
  }

  inline void set_footer(unsigned int s, unsigned int v) {
    unsigned int size = (*(unsigned int *)((char *)&data - WSIZE)) & ~0x7;
    *(unsigned int *)((char *)(&data) + size - DSIZE) = (s | v);
  }

  inline heap_block *next() {
    unsigned int size = *(unsigned int *)((char *)&data - WSIZE) & ~0x7;
    return (heap_block *)((char *)&data + size);
  }

  inline heap_block *prev() {
    unsigned int size = *(unsigned int *)((char *)&data - DSIZE) & ~0x7;
    return (heap_block *)((char *)&data - size);
  }
};

static inline size_t align(size_t n) {
  return (n + sizeof(intptr_t) - 1) & ~(sizeof(intptr_t) - 1);
}

union Block {
  uint8_t *data;
  Block *next;
};

enum Partition {
  SMALL = 0, // < 64k
  MEDIUM,    // < 512k
  LARGE      // < 4megs
};

enum Exponent { EXP_SMALL = 16, EXP_MEDIUM = 19, EXP_LARGE = 22 };

inline int16_t getPageExponent(size_t s) {
  if (s < SMALL_OBJECT_SIZE) {
    return BASE_PAGE_EXPONENT;
  } else if (s < MEDIUM_OBJECT_SIZE) {
    return BASE_PAGE_EXPONENT + BASE_PAGE_EXPONENT_INCR;
  } else {
    return BASE_PAGE_EXPONENT + (BASE_PAGE_EXPONENT_INCR * 2);
  }
}

inline Partition getPartition(size_t s) {
  if (s < SMALL_OBJECT_SIZE) {
    return SMALL;
  } else if (s < MEDIUM_OBJECT_SIZE) {
    return MEDIUM;
  } else {
    return LARGE;
  }
}
inline Partition getPartitionFromExponent(size_t exp) {
  if (exp == BASE_PAGE_EXPONENT) {
    return SMALL;
  } else if (exp == (BASE_PAGE_EXPONENT + 3)) {
    return MEDIUM;
  } else {
    return LARGE;
  }
}
inline int32_t getPartitionSize(Partition p) {
  switch (p) {
  case SMALL:
    return DEFAULT_PAGE_SIZE;
  case MEDIUM:
    return DEFAULT_MID_PAGE_SIZE;
  default:
    return DEFAULT_LARGE_PAGE_SIZE;
  }
}
inline int8_t getPageCount(Partition p) {
  switch (p) {
  case SMALL:
    return 64;
  case MEDIUM:
    return 8;
  default:
    return 1;
  }
}

enum ContainerType {
  PAGE,
  POOL,
};

template <typename T> struct MemQueue {
  T *first;
  T *last;
  int32_t size;
  bool isEmpty() { return first == NULL; }
  void remove(T *a) {
    if (a->getPrev() != NULL)
      a->getPrev()->setNext(a->getNext());
    if (a->getNext() != NULL)
      a->getNext()->setPrev(a->getPrev());
    if (a == first)
      first = a->getNext();
    if (a == last)
      last = a->getPrev();
    a->setNext(NULL);
    a->setPrev(NULL);
  }
  void enqueue(T *a) {
    a->setNext(NULL);
    a->setPrev(last);
    if (last != NULL) {
      last->setNext(a);
      last = a;
    } else {
      last = first = a;
    }
  }
};

struct Pool {
  typedef MemQueue<Pool> Queue;

  // 64k (64), 512k(8), 4mb(1)  if larger. Just a Section per allocation.
  uint8_t idx;           // what index in the parent section are we stored.
  int32_t block_size;    //
  int32_t num_available; // num of available blocks. // num of committed blocks.
  int32_t num_committed; // the number of free blocks. page_size/block_size
  Block
      *free; // the start of the free list. extend free by 1 os page at a time.
  Pool *prev;
  Pool *next;

  bool isFull() { return num_committed >= num_available; }

  void freeBlock(void *p) {
    Block *new_free = (Block *)p;
    new_free->next = free;
    free = new_free;
  }

  Block *getFreeBlock() {
    extendPool();
    if (free) {
      auto res = free;
      free = res->next;
      return res;
    }
    return NULL;
  }

  bool extendPool() {
    if (free == NULL) {
      if (!isFull()) {
        uintptr_t start = (uintptr_t)&blocks[0];
        uintptr_t end =
            (uintptr_t)((uint8_t *)start + (num_committed * block_size));
        if (start != end) {
          start = end;
        }
        free = (Block *)start;
        free->next = NULL;
        num_committed++;
        if (block_size < os_page_size) {
          if (start != end) {
            start = end;
          }
          uintptr_t page_end = align_up(start, os_page_size);
          auto offset = page_end - start;
          auto steps = min(offset / block_size, num_available - num_committed);
          Block *block = free;
          for (int i = 1; i < steps; i++) {
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

struct Page {
  typedef MemQueue<Page> Queue;
  // 64k(64) per segment.
  uint8_t idx;
  int32_t num_allocations;
  uint8_t *start;
  uint8_t *seek;
  Page *prev;
  Page *next;

  inline void *coalesce(void *bp) {
    heap_block *hb = (heap_block *)bp;
    auto size = hb->get_header() & ~0x7;
    int prev_header = hb->prev()->get_header();
    int next_header = hb->next()->get_header();

    size_t prev_alloc = prev_header & 0x1;
    size_t next_alloc = next_header & 0x1;
    if (prev_alloc && next_alloc) {
      return bp;
    }
    size_t prev_size = prev_header & ~0x7;
    size_t next_size = next_header & ~0x7;

    if (prev_alloc && !next_alloc) {
      size += next_size;
      hb->set_header(size, 0);
      hb->set_footer(size, 0);
      seek = (uint8_t *)bp;
    } else if (!prev_alloc && next_alloc) {
      size += prev_size;
      hb->set_footer(size, 0);
      hb->prev()->set_header(size, 0);
      bp = (void *)hb->prev();
      seek = (uint8_t *)bp;
    } else {
      size += prev_size + next_size;
      hb->prev()->set_header(size, 0);
      hb->next()->set_footer(size, 0);
      bp = (void *)hb->prev();
      seek = (uint8_t *)bp;
    }
    return bp;
  }

  inline void place(void *bp, int asize, int list) {
    heap_block *hb = (heap_block *)bp;
    auto csize = hb->get_header() & ~0x7;

    if ((csize - asize) >= (2 * DSIZE)) {
      hb->set_header(asize, 1);
      hb->set_footer(asize, 1);
      hb = hb->next();
      hb->set_header(csize - asize, 0);
      hb->set_footer(csize - asize, 0);
      seek = (uint8_t *)hb;
    } else {
      hb->set_header(csize, 1);
      hb->set_footer(csize, 1);
      seek = (uint8_t *)hb->next();
    }
  }

  inline void *find_fit(int asize) {
    void *oldrover = seek;
    heap_block *hb = (heap_block *)seek;
    auto bsize = hb->get_header() & ~0x7;
    // Search from the rover to the end of list
    for (; bsize > 0; hb = hb->next(), bsize = hb->get_header() & ~0x7) {
      auto free = !(hb->get_header() & 0x1);
      if (free)
        seek = (uint8_t *)hb;
      if (free && (asize <= bsize)) {
        return hb;
      }
    }

    seek = start;
    hb = (heap_block *)seek;
    // search from start of list to old rover
    for (; hb < oldrover; hb = hb->next(), bsize = hb->get_header() & ~0x7) {
      auto free = !(hb->get_header() & 0x1);
      if (free)
        seek = (uint8_t *)hb;
      if (free && (asize <= bsize)) {
        return hb;
      }
    }
    seek = start;
    return NULL;
  }

public:
  uint8_t blocks[];
  inline Page *getPrev() const { return prev; }
  inline Page *getNext() const { return next; }
  inline void setPrev(Page *p) { prev = p; }
  inline void setNext(Page *n) { next = n; }
};

struct Section {
  typedef MemQueue<Section> Queue;
  // 24 bytes as the section header
  bitmask mask; // 64 pages bit per page.
  size_t thread_id;
  size_t size;
  // An area and section can overlap, and the prev next pointer of an area will
  // always be under the 32tb range. top 16 bits area always zero.
private:
  size_t container_type;     // top 16 bits.
  size_t container_exponent; // top 16 bits.
public:
  size_t misc; // reserved.

  // links to sections.
  Section *prev;
  Section *next;

  uint8_t pages[];

  inline Section *getPrev() const { return prev; }
  inline Section *getNext() const { return next; }
  inline void setPrev(Section *p) { prev = p; }
  inline void setNext(Section *n) { next = n; }
  inline ContainerType getContainerType() const {
    return (
        ContainerType)((uint16_t)((container_type & 0xffff000000000000) >> 48));
  }
  inline Exponent getContainerExponent() const {
    return (Exponent)(uint16_t)((container_exponent & 0xffff000000000000) >>
                                48);
  }
  inline void setContainerType(ContainerType pt) {
    container_type |= ((uint64_t)pt << 48);
  }
  inline void setContainerExponent(uint16_t prt) {
    container_exponent |= ((uint64_t)prt << 48);
  }

  uintptr_t allocate_pool(int16_t blockSize) {
    auto pageIdx = mask.firstFree();
    mask.reserve(pageIdx);
    uintptr_t page = getPool(pageIdx);
    init_pool(page, pageIdx, blockSize);
    return page;
  }

  inline Pool *findPool(void *p) const {
    ptrdiff_t diff = (uint8_t *)p - (uint8_t *)this;
    switch (getContainerExponent()) {
    case Exponent::EXP_SMALL: {
      return (Pool *)((uint8_t *)&pages[0] +
                      (1 << Exponent::EXP_SMALL) *
                          ((size_t)diff >> Exponent::EXP_SMALL));
    }
    case Exponent::EXP_MEDIUM: {
      return (Pool *)((uint8_t *)&pages[0] +
                      (1 << Exponent::EXP_MEDIUM) *
                          ((size_t)diff >> Exponent::EXP_MEDIUM));
    }
    default: {
      return (Pool *)((uint8_t *)&pages[0] +
                      (1 << Exponent::EXP_LARGE) *
                          ((size_t)diff >> Exponent::EXP_LARGE));
    }
    }
  }

private:
  inline uintptr_t getPool(int8_t idx) const {
    switch (getContainerExponent()) {
    case Exponent::EXP_SMALL: {
      return (uintptr_t)((uint8_t *)&pages[0] +
                         (1 << Exponent::EXP_SMALL) * idx);
    }
    case Exponent::EXP_MEDIUM: {
      return (uintptr_t)((uint8_t *)&pages[0] +
                         (1 << Exponent::EXP_MEDIUM) * idx);
    }
    default: {
      return (uintptr_t)((uint8_t *)&pages[0] +
                         (1 << Exponent::EXP_LARGE) * idx);
    }
    }
  }

  void init_pool(uintptr_t paddr, int8_t pidx, int16_t blockSize) {
    auto last_page = false;
    auto partition = getContainerExponent();
    auto psize = 1 << partition;
    if ((partition == EXP_SMALL && pidx == 63) ||
        (partition == EXP_MEDIUM && pidx == 7) || (partition == EXP_LARGE)) {
      last_page = true;
    }
    Pool *pool = (Pool *)paddr;
    pool->idx = pidx;
    pool->block_size = blockSize;
    pool->num_available = (psize - sizeof(Pool)) / blockSize;
    pool->num_committed = 0;
    pool->next = NULL;
    pool->prev = NULL;
    pool->extendPool();
  }
};

struct RawSection {
  size_t addr_id;
  uint8_t section_size; // 1 - 8* 4megs
  size_t buff_size;
  uint8_t buff[];
};

struct Area // 32 megs of memory.
{
  // The area is overlapping with the first section. And they share some
  // attributes.
  static const uintptr_t ptr_mask = 0x0000ffffffffffff;
  static const uintptr_t inv_ptr_mask = 0xffff000000000000;
  typedef MemQueue<Area> Queue;
  bitmask mask;
  size_t thread_id;
  size_t size;

private:
  // these members are shared with the first section in the memory block. so,
  // the first high 16 bits are reserved by the section.
  Area *prev;
  Area *next;

public:
  inline Area *getPrev() const {
    return (Area *)((uintptr_t)prev & ptr_mask);
  } // remove the top 16 bits
  inline Area *getNext() const {
    return (Area *)((uintptr_t)next & ptr_mask);
  } // remove the top 16 bits
  inline void setPrev(Area *p) {
    prev = (Area *)(((inv_ptr_mask & (uintptr_t)prev) | (uintptr_t)p));
  };
  inline void setNext(Area *n) {
    next = (Area *)(((inv_ptr_mask & (uintptr_t)next) | (uintptr_t)n));
  };
};

static inline bool commit_memory(void *base, size_t size) {
#ifdef WINDOWS
  return VirtualAlloc(base, size, MEM_COMMIT, PAGE_READWRITE) == base;
#else
  return (mprotect(base, size, (PROT_READ | PROT_WRITE)) == 0);
#endif
}

static inline bool decommit_memory(void *base, size_t size) {
#ifdef WINDOWS
  return VirtualFree(base, size, MEM_DECOMMIT);
#else
  return (mmap(base, size, PROT_NONE,
               (MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE), -1,
               0) == base);
#endif
}

static inline bool free_memory(void *ptr, size_t size) {
#ifdef WINDOWS
    return VirtualFree(ptr, 0, MEM_RELEASE) == 0);
#else
  return (munmap(ptr, size) == -1);
#endif
}

static inline bool release_memory(void *ptr, size_t size, bool commit) {
  if (commit) {
    return decommit_memory(ptr, size);
  } else {
    return free_memory(ptr, size);
  }
}

static inline void *alloc_memory(void *base, size_t size, bool commit) {
#ifdef WINDOWS
  int flags = commit ? MEM_RESERVE | MEM_COMMIT : MEM_RESERVE;
  return VirtualAlloc(base, size, flags, PAGE_READWRITE);
#else
  int flags = commit ? (PROT_WRITE | PROT_READ) : PROT_NONE;
  return mmap(base, size, flags, (MAP_PRIVATE | MAP_ANONYMOUS), -1, 0);
#endif
}

static uintptr_t aligned_heap_base = HINT_BASE;
static std::atomic<int32_t> global_thread_idx = {-1};
std::mutex windows_align_mutex;
struct myAllocator {
private:
  static thread_local size_t _thread_id;
  static thread_local int32_t _thread_idx;
  uintptr_t local_heap_base;
  Area *previous_area;

  static void *alloc_memory_aligned(void *base, size_t size, size_t alignment,
                                    bool commit) {
    // alignment is smaller than a page size or not a power of two.
    if (!(alignment >= get_os_page_size() && POWER_OF_TWO(alignment)))
      return NULL;
    size = align_up(size, get_os_page_size());
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
      {
        std::unique_lock<std::mutex> lock(windows_align_mutex);
        release_memory(ptr, over_size, commit);
        ptr = alloc_memory(aligned_p, size, commit);
      }
    }

    return ptr;
  }

  typedef union {
    uint64_t whole;
    struct {
      uint32_t low;
      uint32_t high;
    } word;
  } ui64words;

  static int8_t size_to_mask(size_t s, int8_t *count) {
    if (s > SECTION_SIZE * 8) {
      *count = 0;
      return 0;
    } else {
      auto parts = (s / SECTION_SIZE) + 1;
      *count = parts;
      return (int8_t)(1 << parts) - 1;
    }
  }

  static int32_t find_free_slot_in_mask(uint64_t mask, int8_t submask) {
    ui64words words;
    words.whole = mask;

    return 0;
  }

  static int get_mask_and_offset() {
    //
    return 0;
  }

  bool alloc_area(Area **area) {
    Area *newArea = (Area *)alloc_memory_aligned(align_base(AREA_SIZE),
                                                 AREA_SIZE, AREA_SIZE, true);
    if (newArea == NULL) {
      return false;
    }
    *area = newArea;
    newArea->mask = {0};
    newArea->thread_id = _thread_idx;
    newArea->setNext(NULL);
    newArea->setPrev(NULL);
    return true;
  }

  Area *get_area(size_t size, int32_t *outSectionIdx,
                 int32_t *outSectionCount) {
    // the areas are empty
    Area *newArea = NULL;
    auto p = getPartition(size);
    if (previous_area != NULL) {
      Area *cur_area = previous_area;
      //
      while (cur_area != NULL) {
        auto idx = cur_area->mask.allocate_bits(1);
        if (idx != -1) {
          newArea = cur_area;
          *outSectionIdx = idx;
          break;
        }
        cur_area = cur_area->getNext();
      }
    }
    if (newArea == NULL) {
      if (!alloc_area(&newArea)) {
        return NULL;
      }

      if (p == MEDIUM) {
        *outSectionIdx = 0;
        newArea->mask.reserve(1);
      } else {
        *outSectionIdx = 1;
        newArea->mask.reserve(2);
      }
      newArea->size = AREA_SIZE;
    }

    return newArea;
  }

  Section *alloc_section(size_t size) {
    int32_t section_idx;
    int32_t section_count;
    auto new_area = get_area(size, &section_idx, &section_count);
    if (new_area == NULL) {
      return NULL;
    }
    local_areas.enqueue(new_area);
    previous_area = new_area;
    uintptr_t section_addr = (uintptr_t)new_area + SECTION_SIZE * section_idx;
    return (Section *)section_addr;
  }

  void free_section(Section *s) { free_memory(s, SECTION_SIZE); }

public:
  Area::Queue local_areas;       // all local areas
  Section::Queue local_sections; // all local sections

  Page free_pages;   //
  Page::Queue pages; // each block has a header and footer labelling size and
                     // allocation state.

  Pool free_small_pools[NUM_FREE_FAST_PAGES]; // 8 - 256
  Pool::Queue pools;

  static inline size_t thread_id() { return (size_t)&_thread_id; }

  myAllocator() {
    local_heap_base = HINT_BASE;
    previous_area = NULL;
    auto nidx = std::atomic_fetch_add_explicit(&global_thread_idx, 1,
                                               std::memory_order_acq_rel);
    _thread_idx = nidx + 1;
    local_heap_base = 0;
    free_pages = {0, NULL, NULL, NULL, NULL};
    pages = {NULL, NULL};
    local_sections = {NULL, NULL};
    local_areas = {NULL, NULL};
    pools = {NULL, NULL, 8};
  }

  void *align_base(size_t size) {
    if ((size % SECTION_SIZE) != 0)
      return NULL;
    if (size > AREA_SIZE)
      return NULL;

    uintptr_t hint = local_heap_base;
    if (hint == 0 || hint > HINT_MAX) {
      uintptr_t init =
          aligned_heap_base + os_num_hardware_threads * _thread_idx;
      hint = init;
    }
    if (hint % SECTION_SIZE != 0)
      return NULL;
    return (void *)hint;
  }

  void *malloc(size_t s) {
    auto asize = align(s);
    switch (getPartition(asize)) {
    case SMALL: {
      auto small_page = free_small_pool(asize);
      return small_page->getFreeBlock();
    }
    case MEDIUM: {
      auto small_page = free_pool(asize);
      return small_page->getFreeBlock();
    }
    case LARGE: {
      auto small_page = free_pool(asize);
      return small_page->getFreeBlock();
    }
    default: {
      // [ ] - allocate large memory objects  ... see it allocate different page
      // sizes. [ ] - free large memory objects.     ... see the areas get
      // released back to the os. [ ] - ensure that it works with multiple
      // threads. offset and areas. [ ] - structures, memory_block, raw_areas
      // and pages. [ ] - allocation between 2 && 32 megs, go into raw areas
      // with implicit lists. [ ] - perf tests and mimalloc tests. [ ] - memory
      // stats and leak sanity test.å [ ] - integrate with allot library. [ ] -
      // -
      //
      // address counter per thread. much like an index in a flat tree.
      //  base address from what thread id.
      //  subsequent address fields are derived from rules.
      //

      //
      // alloc::pool_slots;
      // alloc::misc_slots; ->ui threads, misc thread workers that are not part
      // of the pool. auto thread_id = alloc.init_thread();
      // alloc.release_thread();  // release allocation slot for thread.
      //
      //

      //
      //  alloc.push_arena() -> push work into the high address space.
      //      do work.
      //  void*p = alloc->pop_arena(); -> pop work back into the previous
      //  address space.
      //      move the parts from the arena that we want to keep.
      //  alloc->release_arena(p);
      //

      //
      // base memory hierarchy
      //
      // 32, 4 gigs per thred. high order area. 4tb / num threads. 64 gigs per
      // thread. resource area. reserved for main thread. resource thread.
      //

      //
      // 32*32    1gig    1 gig
      // 32*256           8 gigs
      // 32*1gb           32 gigs
      // 32*4gb           128 gigs
      //

      // 32  2gigs   128 * 32
      //    2gigs    128gigs
      // [32 megs] [2gigs] [128 gigs]
      // 32*64 [ 2gigs ] [ 128 gigs ]
      // base area 32 megs. [32*1024] 3.2gig [1gb per thread] [1tb]
      //
      // extended area 1 gig per thread.
      // allocate a section 32 megs aligned in the special part of vm.
      // fit to the size... so area header plus the actual allocation size.
      //
      break;
    }
    }
    return NULL;
  }

  inline void free(void *p) {
    if (p == NULL)
      return;
    Area *area = (Area *)((uintptr_t)p & ~(AREA_SIZE - 1));
    size_t size = area->size;
    if (size <= AREA_SIZE) {
      Section *section = (Section *)((uintptr_t)p & ~(SECTION_SIZE - 1));
      if (_thread_idx == section->thread_id) {
        section->findPool(p)->freeBlock(p);
      }
    } else {
      // large object allocation.
      if (!free_memory(p, size)) {
        // raize an error
      }
    }
  }

  // this hands out a node inside of a raw page.
  // it is assumed that the called knows how to
  inline void *raw_malloc(size_t s) {
    int asize = align(s);
    // find a free page
    // if no free page... get from system.
    //      add to free lists.
    // find a free block
    return NULL;
  }

  inline void raw_free(void *bp) {
    //
  }

  inline int raw_resize(int incr) {
    // check next block in list.
    // if able to resize, return true
    return 0;
  }

private:
  Pool *free_small_pool(size_t s) {
    // FixedPage* start = NULL;
    // auto bin = s/sizeof(uintptr_t);
    // FixedPage* start = &free_small_pages[bin];
    // while(start != NULL && (start->free != NULL))
    //{
    //     return start;
    // }
    return free_pool(s);
  }

  Pool *free_pool(size_t s) {
    Pool *start = NULL;
    // auto bin = s/sizeof(uintptr_t);
    // FixedPage* start = &free_small_pages[bin];
    // while(start != NULL && (start->free != NULL))
    //{
    //     return start;
    // }
    if (pools.first != NULL) {
      start = pools.first;
      while (start != NULL && start->free == NULL) {
        start = start->next;
      }
      if (start != NULL) {
        return start;
      }
    }
    auto page_exponent = getPageExponent(s);
    auto free_section = local_sections.first;
    if (free_section) {
      // find free section.
      while (free_section != NULL) {
        if (free_section->getContainerExponent() == page_exponent) {
          if (!free_section->mask.isFull()) {
            break;
          }
        }
        free_section = free_section->next;
      }
    }

    if (free_section == NULL) {
      auto new_section = alloc_section(s);
      if (new_section == NULL) {
        return NULL;
      }
      new_section->thread_id = _thread_idx;
      new_section->setContainerExponent(page_exponent);
      new_section->setContainerType(ContainerType::PAGE);
      new_section->next = NULL;
      new_section->prev = NULL;
      local_sections.enqueue(new_section);
      free_section = new_section;
    }
    start = (Pool *)free_section->allocate_pool(s);
    pools.enqueue(start);
    return start;
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
struct _MemoryBlock {
  enum mtype {
    M_STACK, // src memory is on the local stack
    M_HEAP,  // src memory was source from malloc. large requests, or pools
             // unable to hand out memory.
    M_POOL,  // src memory was sourced from a linear thread local pool. small
             // requests.
    M_FIXED_POOL, // src memory was sourced from a fixed pool of medium sized
                  // requests. count limit. falls back on heap.
    M_NULL,       // memory allocation failure.
  };

  void *src_mem;   // pointer to allocated memory.
  mtype src_type;  // where is the current memory sourced from.
  size_t req_size; // size requested.
  size_t src_size; // size returned. Will be >= to requested size.

  _MemoryBlock() {
    src_type = M_NULL;
    req_size = 0;
    src_size = 0;
    src_mem = NULL;
  }

  _MemoryBlock(void *m, size_t s) {
    src_type = M_STACK;
    src_size = s;
    src_mem = m;
    req_size = s;
  }

  _MemoryBlock(size_t s) { alloc(s); }

  ~_MemoryBlock() {
    if (src_type != M_HEAP) {
      // free(src_mem);
    } else if (src_type == M_FIXED_POOL) {
      // Allot::DeallocFixed(src_mem, src_size);
    }
  }

  void *alloc(size_t s) {
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

  void free() {
    if (src_type != M_NULL) {
    }
  }

  void *realloc(size_t s) {
    //
    return NULL;
  }
};

#define MAX_HEAP (20 * (1 << 20)) /* 20 MB */

//
//  memory_block() -> controls where the allocated memory comes from.
//      - otherwise, api needs to be aware of releasing the memory to the
//      correct pool.
//
/*
 * Constants and macros
 */

//#define USE_FREE_LISTS

static const char LogTable256[256] = {
#define LT(n) n, n, n, n, n, n, n, n, n, n, n, n, n, n, n, n
    -1,    0,     1,     1,     2,     2,     2,     2,     3,     3,     3,
    3,     3,     3,     3,     3,     LT(4), LT(5), LT(5), LT(6), LT(6), LT(6),
    LT(6), LT(7), LT(7), LT(7), LT(7), LT(7), LT(7), LT(7), LT(7)};

static inline int log2(unsigned int v) {
  unsigned int t;
  unsigned int tt = v >> 16;

  if (tt) {
    return (t = tt >> 8) ? 24 + LogTable256[t] : 16 + LogTable256[tt];
  } else {
    return (t = v >> 8) ? 8 + LogTable256[t] : LogTable256[v];
  }
}

static const int max_size = 1 << 25;
static const int pow_to_list[25] = {0,  0,  0,  0,  0,  0,  1,  2,  3,
                                    4,  5,  6,  7,  8,  9,  10, 11, 12,
                                    13, 14, 15, 16, 17, 18, 19};

static inline int next_power_of_two(int size) {
  size--;
  size |= size >> 1;
  size |= size >> 2;
  size |= size >> 4;
  size |= size >> 8;
  size |= size >> 16;
  size++;
  return size;
}
static inline int adjusted_size(int size) {
  if (size < DSIZE) {
    return DSIZE;
  }
  /* Adjust block size to include boundary tags and alignment requirements */
  if (size <= DSIZE) {
    return 2 * DSIZE;
  } else {
    return DSIZE * ((size + (DSIZE) + (DSIZE - 1)) / DSIZE);
  }
}

static inline int size_to_power(int asize) {
  if (!(asize && !(asize & (asize - 1)))) {
    asize = next_power_of_two(asize) - 1;
  }
  return log2(asize);
}

class Allocator {
  static const int minimum_allocation_size = 64;
  static const int number_of_segl = 20; // segregated lookup lists.
                                        //
                                        // instanced per thread.
                                        //
private:
  void *free_lists[number_of_segl]; // Array of pointers to segregated free
                                    // lists
  int free_lists_size[number_of_segl];
  uint8_t *prologue_block; /* Pointer to prologue block */
  uint8_t *mem_start_brk;  /* points to first byte of heap */
  uint8_t *mem_brk;        /* points to last byte of heap */
  uint8_t *mem_max_addr;   /* largest legal heap address */
  uint8_t error;
  void *seek_p;
  void *first_free;
  void *last_free;
  void *alloc_memory(size_t size) {
#ifdef WINDOWS
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    return mmap(NULL, size, (PROT_WRITE | PROT_READ),
                (MAP_PRIVATE | MAP_ANONYMOUS), -1, 0);
#endif
  }
  void *mem_sbrk(size_t incr) {
    uint8_t *old_brk = mem_brk;

    if ((incr < 0) || ((mem_brk + incr) > mem_max_addr)) {
      errno = ENOMEM;
      fprintf(stderr, "ERROR: mem_sbrk failed. Ran out of memory...\n");
      return (void *)-1;
    }
    mem_brk += incr;
    return (void *)old_brk;
  }

public:
  int max_iter;
  Allocator() {
    max_iter = 0;

    if ((mem_start_brk = (uint8_t *)alloc_memory(MAX_HEAP)) == NULL) {
      fprintf(stderr, "mem_init_vm: malloc error\n");
      exit(1);
    }

    mem_max_addr = mem_start_brk + MAX_HEAP; /* max legal heap address */
    mem_brk = mem_start_brk;                 /* heap is empty initially */
    uint8_t *heap_start;                     // Pointer to beginning of heap

    /* Initialize array of pointers to segregated free lists */
    for (int list = 0; list < number_of_segl; list++) {
      free_lists[list] = NULL;
      free_lists_size[list] = 0;
    }

    /* Allocate memory for the initial empty heap */
    if ((long)(heap_start = (uint8_t *)mem_sbrk(4 * WSIZE)) == -1) {
      error = -1;
      return;
    }

    *heap_start = 0;
    *(heap_start + WSIZE) = DSIZE | 1;   /* Prologue header */
    *(heap_start + DSIZE) = (DSIZE | 1); /* Prologue footer */
    *(heap_start + WSIZE + DSIZE) = 1;   /* Epilogue header */
    prologue_block = heap_start + DSIZE;
    seek_p = prologue_block;

    first_free = prologue_block;
    last_free = prologue_block;

    /* Extend the empty heap */
    if (extend_heap(os_page_size * 40) == NULL) {
      error = -1;
      return;
    }

    /* Variables for checking function
    line_count = LINE_OFFSET;
    skip = 0;
    */
    error = 0;
  }

  void *mm_malloc(unsigned int size) {
    //
    //  allocate from a size class.
    //  what list stores free nodes of certain size.
    //
    unsigned int asize = adjusted_size(size);
    unsigned int extendsize;
    void *ptr = NULL;
    int list = 0;

    if (size == 0 || size > max_size) {
      return NULL;
    }

#ifdef USE_FREE_LISTS
    int pow = size_to_power(size);
    list = pow_to_list[pow];
    ptr = free_lists[list];
#else

    if ((ptr = (char *)find_fit(asize)) != NULL) {
      place(ptr, asize, list);
      return ptr;
    }
#endif
    if (ptr == NULL) {
      int incr = (asize / os_page_size + 1);
      extendsize = os_page_size * incr;
      if ((ptr = extend_heap(extendsize)) == NULL)
        return NULL;
    }

    place(ptr, asize, list);

    return ptr;
  }

  void mm_free(void *bp) {
    if (bp == 0)
      return;

    heap_block *hb = (heap_block *)bp;
    auto size = hb->get_header() & ~0x7;
    hb->set_header(size, 0);
    hb->set_footer(size, 0);

    coalesce(bp);
  }

  inline void *coalesce(void *bp) {
    heap_block *hb = (heap_block *)bp;
    auto size = hb->get_header() & ~0x7;

    size_t prev_alloc = hb->prev()->get_header() & 0x1;
    size_t next_alloc = hb->next()->get_header() & 0x1;

    if (prev_alloc && next_alloc) {

      return bp;
    }
    size_t prev_size = hb->prev()->get_header() & ~0x7;
    size_t next_size = hb->next()->get_header() & ~0x7;
    if ((size + next_size) > prev_size) {
      return bp;
    }
    if (prev_alloc && !next_alloc) {
      size += next_size;
      hb->set_header(size, 0);
      hb->set_footer(size, 0);
    } else if (!prev_alloc && next_alloc) {
      size += prev_size;
      hb->set_footer(size, 0);
      hb->prev()->set_header(size, 0);
      bp = (void *)hb->prev();
    } else {
      size += prev_size + next_size;
      hb->prev()->set_header(size, 0);
      hb->next()->set_footer(size, 0);
      bp = (void *)hb->prev();
    }

    return bp;
  }

  void *mm_realloc(void *ptr, int size) {
    int oldsize;
    void *newptr;

    if (size == 0) {
      mm_free(ptr);
      return 0;
    }

    if (ptr == NULL) {
      return mm_malloc(size);
    }
    newptr = mm_malloc(size);

    if (!newptr) {
      return 0;
    }
    heap_block *hb = (heap_block *)ptr;
    oldsize = hb->get_header() & ~0x7;
    if (size < oldsize)
      oldsize = size;
    memcpy(newptr, ptr, oldsize);

    mm_free(ptr);

    return newptr;
  }
  inline void update_bounds(void *p) {
    if (p > last_free) {
      last_free = p;
    } else if (p < first_free) {
      first_free = p;
    }
  }
  inline void *extend_heap(int words) {
    char *bp;
    int size = (words % 2) ? (words + 1) * WSIZE : words * WSIZE;
    if ((long)(bp = (char *)mem_sbrk(size)) == -1)
      return NULL;

    heap_block *hb = (heap_block *)bp;
    hb->set_header(size, 0);
    hb->set_footer(size, 0);
    hb->next()->set_header(0, 1);

    return coalesce(bp);
  }

  inline void place(void *bp, int asize, int list) {
    heap_block *hb = (heap_block *)bp;
    auto csize = hb->get_header() & ~0x7;

    if ((csize - asize) >= (2 * DSIZE)) {
      hb->set_header(asize, 1);
      hb->set_footer(asize, 1);
      hb = hb->next();
      hb->set_header(csize - asize, 0);
      hb->set_footer(csize - asize, 0);

      // seek_p = hb;
    } else {
      hb->set_header(csize, 1);
      hb->set_footer(csize, 1);
      // seek_p = hb->next();
    }
  }

  inline void *find_fit(int asize) {
    // if num_free == 0
    //  return null
    // if(asize > max_free_size) NULL
    // if(asize < min_free_size) NULL;
    // if(num_free > 2)
    //      find_between min and max address.
    // else if(num_free <= 2)
    //      if 1:
    //          if(min is free) return min;
    //          if(max is free) return max;
    //          else
    //
    // start form min and walk until max.
    //
    void *oldrover = seek_p;
    heap_block *hb = (heap_block *)seek_p;
    auto bsize = hb->get_header() & ~0x7;
    // Search from the rover to the end of list
    for (; bsize > 0; hb = hb->next(), bsize = hb->get_header() & ~0x7) {
      auto free = !(hb->get_header() & 0x1);
      if (free)
        seek_p = hb;
      if (free && (asize <= bsize)) {
        update_bounds(hb);
        return hb;
      }
    }

    seek_p = prologue_block;
    hb = (heap_block *)seek_p;
    // search from start of list to old rover
    for (; hb < oldrover; hb = hb->next(), bsize = hb->get_header() & ~0x7) {
      auto free = !(hb->get_header() & 0x1);
      if (free)
        seek_p = hb;
      if (free && (asize <= bsize)) {
        update_bounds(hb);
        return hb;
      }
    }
    seek_p = prologue_block;
    return NULL;
  }
};

#endif /* Malloc_h */
