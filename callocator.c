
#include "callocator.inl"
#include "allocator.h"
#include "os.h"
#include "partition_allocator.h"
#include "implicit_list.h"
#include <stdatomic.h>

extern PartitionAllocator *partition_allocator;
static _Atomic(size_t) num_threads = (1);
/*
static inline size_t get_thread_count(void) {
    return atomic_load_explicit(&num_threads, memory_order_relaxed);
}*/
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
    // disconnect all the pools.
    for (int i = 0; i < POOL_BIN_COUNT; i++) {
        Queue* queue = &alloc->pools[i];
        Pool* start = queue->head;
        while (start) {
            Pool* next = start->next;
            start->next = NULL;
            start->prev = NULL;
            start = next;
        }
    }
    // free the arenas.
    for(int32_t i = 0; i < ARENA_BIN_COUNT; i++){
        Queue* queue = &alloc->arenas[i];
        Arena* start = queue->head;
        while (start) {
            Arena* next = start->next;
            
            // Remove from thread-local queue (no lock needed)
            list_remove(queue, start);
            
            // if the arena is empty, we remove it from memory
            if (atomic_load_explicit(&start->in_use, memory_order_acquire) == 0) {
                // Safe to reclaim (no outstanding frees)
                partition_allocator_free_blocks(partition_allocator, start, true);
            }
            else {
                // else we mark it as abandoned.
                partition_allocator_abandon_blocks(partition_allocator, start);
                atomic_store_explicit(&start->thread_id, -1, memory_order_release);
            }

            start = next;
        }
    }
    
    // free the implicits
    for(int32_t i = 0; i < ARENA_BIN_COUNT; i++){
        Queue* queue = &alloc->implicit[i];
        ImplicitList* start = queue->head;
        while (start) {
            ImplicitList* next = start->next;
            
            // Remove from thread-local queue (no lock needed)
            list_remove(queue, start);
            
            implicitList_move_deferred(start);
            
            if(start->num_allocations == 0)
            {
                // Safe to reclaim (no outstanding frees)
                partition_allocator_free_blocks(partition_allocator, start, true);
            }
            else {
                // else we mark it as abandoned.
                partition_allocator_abandon_blocks(partition_allocator, start);
                atomic_store_explicit(&start->thread_id, -1, memory_order_release); 
            }
            
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

#if defined(__cplusplus)
namespace {
    class ThreadInitializer {
    public:
        ThreadInitializer() { allocator_init(); }
        ~ThreadInitializer() { allocator_destroy(); }
    };
    ThreadInitializer init;
}
#else
    // For C, we use constructor and destructor attributes to ensure the allocator is initialized and destroyed
    #include <stddef.h>
    #include <stdint.h>
    #include <stdbool.h>    
    #if defined(__GNUC__) || defined(__clang__)
        __attribute__((constructor)) static void library_init(void) { allocator_init(); }
        __attribute__((destructor)) static void library_destroy(void) { allocator_destroy(); }
    #elif defined(_WIN32)
        /* Windows-specific initialization */
        #pragma section(".CRT$XIU", read)
        __declspec(allocate(".CRT$XIU")) void (*autostart)(void) = allocator_init;
        #pragma section(".CRT$XPU", read)
        __declspec(allocate(".CRT$XPU")) void (*autoexit)(void) = allocator_destroy;
    #else
        #error "Platform not supported for automatic initialization"
    #endif
#endif // __cplusplus

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
    // realloc is a bit tricky, we need to allocate a new memory
    // and copy the old memory into the new memory.
    if (p == NULL) {
        return cmalloc(s);
    }
    if (s == 0) {
        cfree(p);
        return NULL;
    }   
    // we need query the size of the old memory.
    size_t old_size = allocator_get_size(p);
    if (old_size == 0) {
        // we don't know the size of the old memory, so we cannot realloc.
        return NULL;
    }
    if (s <= old_size) {
        // we can just return the old memory, since it is large enough.
        return p;
    }
    void *new_ptr = cmalloc(s);
    if (new_ptr == NULL) {
        // we were not able to allocate the new memory.
        return NULL;
    }
    // if our memory was in the OS slot, we might be able to remap it.
    if ((uintptr_t)p > BASE_OS_ALLOC_ADDRESS && (uintptr_t)p < OS_ALLOC_END)
    {
        // lets try to remap the memory.
        if (remap_memory(p, new_ptr, s)) {
            // we were able to remap the memory, so we can just return the new address.
            // but we need to update the header.
            uint64_t *header = (uint64_t *)((uintptr_t)p - os_page_size);
            *(++header) = s;
            return p;
        }
    }
    // we were not able to remap the memory
    memcpy(new_ptr, p, old_size);
    cfree(p);
    
    return new_ptr;
}

// Allocating memory from the OS that will not conflict with any memory region
// of the allocator.
static _Atomic(uintptr_t) os_alloc_hint = BASE_OS_ALLOC_ADDRESS;
void *cmalloc_os(size_t size)
{
    // align size to page size
    size += os_page_size;
    size = (size + (os_page_size - 1)) & ~(os_page_size - 1);
    
    // we will allocate memory from the OS, so we can use the hint.
    uintptr_t alloc_hint = atomic_load_explicit(&os_alloc_hint, memory_order_relaxed);
    if (alloc_hint == 0) {
        alloc_hint = BASE_OS_ALLOC_ADDRESS;
    }
    void *ptr = alloc_memory((void *)alloc_hint, size, true);
    if((uintptr_t)ptr != 0)
    {
        // we were able to allocate the memory, .. yay!
        // update the hint.
        alloc_hint = (uintptr_t)ptr + size;
        if (alloc_hint >= (uintptr_t)127 << 40) {
            // something has been running for a very long time!
            alloc_hint = BASE_OS_ALLOC_ADDRESS;
        }
        
        atomic_store_explicit(&os_alloc_hint, alloc_hint, memory_order_relaxed);
    }
    else
    {
        return NULL;
    }

    // write the allocation header
    uint64_t *_ptr = (uint64_t *)ptr;
    *_ptr = (uint64_t)(uintptr_t)ptr + os_page_size;
    *(++_ptr) = size;

    return (void *)((uintptr_t)ptr + os_page_size);
}

void cfree_os(void* ptr)
{
    // we need to free the memory that was allocated by the OS.
    if (ptr == NULL) {
        return;
    }
    if (ptr < (void *)BASE_OS_ALLOC_ADDRESS || ptr >= (void *)OS_ALLOC_END) {
        // this is not a valid OS allocated memory.
        return;
    }
    // we need to free the memory.
    uint64_t *header = (uint64_t *)((uintptr_t)ptr - os_page_size);
    size_t size = *(header + 1);
    if (size < os_page_size) {
        // invalid size, we cannot free this memory.
        return;
    }
    free_memory((void *)header, size);
}
