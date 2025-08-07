
#include "callocator.inl"
#include "allocator.h"
#include "os.h"
#include "partition_allocator.h"

extern PartitionAllocator *partition_allocator;
static _Atomic(size_t) num_threads = (1);
static inline size_t  get_thread_count(void) {
    return atomic_load_explicit(&num_threads, memory_order_relaxed);
}
static inline void incr_thread_count(void)
{
    atomic_fetch_add_explicit(&num_threads,1,memory_order_relaxed);
}
static inline void decr_thread_count(void)
{
    atomic_fetch_sub_explicit(&num_threads,1,memory_order_relaxed);
}

cache_align uintptr_t main_thread_id;
cache_align Allocator *main_instance;

__thread Allocator *thread_instance = NULL;
static tls_t _thread_key = (tls_t)(-1);
#define MAIN_ALLOCATOR_SIZE (sizeof(Allocator) + sizeof(Queue) * (POOL_BIN_COUNT + PARTITION_COUNT + ARENA_BIN_COUNT))
static uint8_t main_allocator_buffer[MAIN_ALLOCATOR_SIZE] __attribute__((aligned(64)));

static void allocator_thread_detach(Allocator* alloc)
{
    for(int32_t i = 0; i < ARENA_BIN_COUNT; i++){
        Queue* queue = &alloc->arenas[i];
        Arena* start = queue->head;
        while (start) {
            Arena* next = start->next;
            
            // 1. Remove from thread-local queue (no lock needed)
            list_remove(queue, start);
            
            // 2. ATOMICALLY: Mark as orphaned FIRST
            atomic_store_explicit(&start->thread_id, -1, memory_order_release);
            
            // 3. Recheck in_use AFTER orphaning (prevents race)
            if (atomic_load_explicit(&start->in_use, memory_order_acquire) == 0) {
                // Safe to reclaim (no outstanding frees)
                //partition_reclaim_arena(thread->partition, arena);
            }
            // Else: Arena will be adopted by thread calling free()
            
            start = next;
        }
    }

}
static void thread_done(void *a)
{
    if (a != NULL) {
        Allocator *alloc = (Allocator *)a;
        allocator_thread_detach(alloc);
        free_memory(alloc, os_page_size);
        decr_thread_count();
    }
}

static Allocator *init_thread_instance(uintptr_t tid)
{
    uintptr_t thr_mem = (uintptr_t)alloc_memory((void*)BASE_OS_ALLOC_ADDRESS, os_page_size, true);
    Allocator *new_alloc = allocator_aquire(tid, thr_mem);
    
    thread_instance = new_alloc;
    tls_set(_thread_key, new_alloc);
    incr_thread_count();
    return new_alloc;
}


void _list_remove(void *queue, void *node, size_t head_offset, size_t prev_offset)
{
    Queue *tq = (Queue *)((uint8_t *)queue + head_offset);
    QNode *tn = (QNode *)((uint8_t *)node + prev_offset);
    
    if(tq->head == tq->tail)
    {
        tq->head = NULL;
        tq->tail = NULL;
    }
    else
    {
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
        else if (node == tq->tail) {
            tq->tail = tn->prev;
        }
    }
    
    tn->next = NULL;
    tn->prev = NULL;
}


static inline bool is_main_thread(void)
{
    if (get_thread_id() == main_thread_id) {
        return true;
    } else {
        return false;
    }
}

Allocator *get_instance(uintptr_t tid)
{
    Allocator *ti = thread_instance;
    if (ti == NULL) {
        init_thread_instance(tid);
    }
    return thread_instance;
}

static inline Allocator *get_thread_instance(void)
{
    if (is_main_thread()) {
        return main_instance;
    } else {
        Allocator *ti = thread_instance;
        if (ti == NULL) {
            init_thread_instance(get_thread_id());
        }
        return thread_instance;
    }
}


static int allocator_destroy(void)
{
    tls_set(_thread_key, NULL);
    tls_delete(_thread_key);
    return 0;
}

static int allocator_init(void)
{
    static bool init = false;
    if (init)
        return 0;
    init = true;

    main_thread_id = get_thread_id();
    os_page_size = get_os_page_size();
    tls_create(&_thread_key, &thread_done);
    
    partition_allocator = partition_allocator__create();
    Allocator *new_alloc = allocator_aquire(main_thread_id, (uintptr_t)main_allocator_buffer);
    
    main_instance = new_alloc;
    thread_instance = main_instance;
    return 0;
}

// just like mi_malloc.
// This needs to get started as soon as the process starts up.
// same tricks applied.
#if defined(__cplusplus)
struct thread_init
{
    thread_init() { allocator_init(); }
    ~thread_init() { allocator_destroy(); }
};
static thread_init init;
#elif defined(WINDOWS)
// set section property.
typedef int cb(void);
#if defined(_M_X64) || defined(_M_ARM64)
__pragma(comment(linker, "/include:" "autostart"))
__pragma(comment(linker, "/include:" "autoexit"))
#pragma section(".CRT$XIU", long, read)
#else
__pragma(comment(linker, "/include:"
                         "autostart"))
#endif
#pragma data_seg(".CRT$XIU")
cb *autostart[] = {&allocator_init};
cb *autoexit[] = {&allocator_destroy};
#pragma data_seg() 

#elif defined(__GNUC__) || defined(__clang__)
static void __attribute__((constructor)) library_init(void) { allocator_init(); }
static void __attribute__((destructor)) library_destroy(void) { allocator_destroy(); }
#endif

extern inline void __attribute__((malloc)) *cmalloc(size_t size) {
    
    if(size == 0)
    {
        return NULL;
    }
    Allocator_param params = {get_thread_id(), size, sizeof(intptr_t), false};
    return allocator_malloc(&params);
}

extern inline void __attribute__((malloc)) *caligned_alloc(size_t alignment, size_t size)
{
    if(size == 0)
    {
        return NULL;
    }
    
    // alignment needs to be a power of 2.
    if(!POWER_OF_TWO(alignment))
    {
        return NULL;
    }
    
    if(alignment > os_page_size)
    {
        // we don't care for large alignment asks.
        return NULL;
    }
    
    // you are doing something wrong, but we will give you a pass
    if(size < alignment)
    {
        size = alignment;
    }
    else
    {
        // the size needs to be a multiple of the alignment.
        size_t rem = size % alignment;
        size += rem;
    }
    
    Allocator_param params = {get_thread_id(), size, alignment, false};
    return allocator_malloc(&params);
}

extern inline void __attribute__((malloc)) *zalloc( size_t num, size_t size )
{
    size_t s = num*size;
    if(s == 0)
    {
        return NULL;
    }
    Allocator_param params = {get_thread_id(), s, sizeof(intptr_t), true};
    return allocator_malloc(&params);
}
extern inline void cfree(void *p)
{
    if(p == NULL)
    {
        return;
    }
    
    if (is_main_thread()) {
        allocator_free(main_instance, p);
    } else {
        allocator_free_th(thread_instance, p);
    }
}


//
// Allocating memory at very particular vm addresses.
// Such as for memory mapped objects on disk. Given that
// they where mapped to these ranges.
// Any internal ptr object within those items would be
// ignored since anything that can be released would be the
// initial address defined by the header.
void *cmalloc_at(size_t size, uintptr_t vm_addr)
{
    // we only support custom vm allocation from the 32 - 64 terabyte range.
    if (vm_addr >= (((uintptr_t)32 << 40) + CACHE_LINE) && vm_addr < (((uintptr_t)64 << 40) - (2 * CACHE_LINE))) {
        if ((vm_addr & (os_page_size - 1)) != 0) {
            // vm address must be a multiple of os page
            return NULL;
        }

        // the header is an os_page in size.
        // align size to page size
        uintptr_t header = vm_addr - CACHE_LINE;
        size += CACHE_LINE;
        size = (size + (os_page_size - 1)) & ~(os_page_size - 1);
        //
        void *ptr = alloc_memory((void *)header, size, true);
        if ((uintptr_t)ptr == header) {
            // we were able to allocate the memory, .. yay!
            uint64_t *_ptr = (uint64_t *)ptr;
            // write the address into the header and the size.
            *_ptr = (uint64_t)(uintptr_t)ptr + CACHE_LINE;
            *(++_ptr) = size;
            return (void *)vm_addr;
        }
        if (ptr != NULL) {
            free_memory(ptr, size);
        }
    }
    return NULL;
}

bool callocator_release(void)
{
    Allocator *alloc = get_thread_instance();
    return allocator_release_local_areas(alloc);
}

void *crealloc(void *p, size_t s)
{
    size_t csize = 0;
    // if was in a pool. move it to a heap where it is cheaper to resize.
    // heap can grow.
    size_t min_size = MIN(csize, s);
    void *new_ptr = cmalloc(s);
    memcpy(new_ptr, p, min_size);
    cfree(p);

    return new_ptr;
}

// Allocating memory from the OS that will not conflict with any memory region
// of the allocator.
static uintptr_t os_alloc_hint = BASE_OS_ALLOC_ADDRESS;
void *cmalloc_os(size_t size)
{
    // align size to page size
    size += CACHE_LINE;
    size = (size + (os_page_size - 1)) & ~(os_page_size - 1);
    
    // look the counter while fetching our os memory.
    void *ptr = alloc_memory((void *)os_alloc_hint, size, true);
    if((uintptr_t)ptr == os_alloc_hint)
    {
        os_alloc_hint = (uintptr_t)ptr + size;
    }
    else
    {
        os_alloc_hint = (uintptr_t)ptr;
    }
    
    if (os_alloc_hint >= (uintptr_t)127 << 40) {
        // something has been running for a very long time!
        os_alloc_hint = 0;
    }

    // write the allocation header
    uint64_t *_ptr = (uint64_t *)ptr;
    *_ptr = (uint64_t)(uintptr_t)ptr + CACHE_LINE;
    *(++_ptr) = size;

    return (void *)((uintptr_t)ptr + CACHE_LINE);
}
