
#include "callocator.inl"
#include "../cthread/cthread.h"
#include "allocator.h"
#include "area.h"
#include "os.h"
#include "partition_allocator.h"

extern PartitionAllocator **partition_allocators;
extern int64_t *partition_owners;
extern Allocator **allocator_list;
static uintptr_t main_thread_id;
extern int32_t multi_threaded;
static _Atomic(size_t) num_threads = ATOMIC_VAR_INIT(1);
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

static Allocator *main_instance = NULL;
static const Allocator default_alloc = {-1, NULL, NULL, {NULL, NULL}, 0, 0, 0};
static __thread Allocator *thread_instance = (Allocator *)&default_alloc;
static tls_t _thread_key = (tls_t)(-1);
static void thread_done(void *a)
{
    if (a != NULL) {
        Allocator *alloc = (Allocator *)a;
        release_partition_set((int32_t)alloc->idx);
        decr_thread_count();
    }
}

static Allocator *init_thread_instance(void)
{
    int32_t idx = reserve_any_partition_set();
    Allocator *new_alloc = allocator_list[idx];
    new_alloc->part_alloc = partition_allocators[idx];
    thread_instance = new_alloc;
    tls_set(_thread_key, new_alloc);
    incr_thread_count();
    multi_threaded = 1;
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

void list_remove32(void *queue, void *node, void* base)
{
    Queue32 *tq = (Queue32 *)queue;
    QNode32 *tn = (QNode32 *)node;
    
    if(tq->head == tq->tail)
    {
        tq->head = 0xFFFFFFFF;
        tq->tail = 0xFFFFFFFF;
    }
    else
    {
        if (tn->prev != 0xFFFFFFFF) {
            QNode32 *temp = (QNode32 *)((uint8_t *)base + tn->prev);
            temp->next = tn->next;
        }
        if (tn->next != 0xFFFFFFFF) {
            QNode32 *temp = (QNode32 *)((uint8_t *)base + tn->next);
            temp->prev = tn->prev;
        }
        if (node == (void*)((uint8_t *)base + tq->head)) {
            tq->head = tn->next;
        }
        else if (node == (void*)((uint8_t *)base + tq->tail)) {
            tq->tail = tn->prev;
        }
    }
    
    tn->next = 0xFFFFFFFF;
    tn->prev = 0xFFFFFFFF;
}

static inline bool is_main_thread(void)
{
    if (get_thread_id() == main_thread_id) {
        return true;
    } else {
        return false;
    }
}

static inline Allocator *get_thread_instance(void)
{
    if (is_main_thread()) {
        return main_instance;
    } else {
        Allocator *ti = thread_instance;
        if (ti == &default_alloc) {
            init_thread_instance();
        }
        return thread_instance;
    }
}

static Allocator *reserve_allocator()
{
    int32_t idx = reserve_any_partition_set();
    Allocator *new_alloc = allocator_aquire(idx);
    new_alloc->part_alloc = partition_allocators[idx];
    list_enqueue(&new_alloc->partition_allocators, new_alloc->part_alloc);
    return new_alloc;
}

static void allocator_destroy()
{
    tls_set(_thread_key, NULL);
    tls_delete(_thread_key);
}

static void allocator_init()
{
    static bool init = false;
    if (init)
        return;
    init = true;

    size_t size = sizeof(void *) * MAX_THREADS * 3;
    uintptr_t thr_mem = (uintptr_t)cmalloc_os(size);
    partition_allocators = (PartitionAllocator **)thr_mem;
    thr_mem += ALIGN_CACHE(sizeof(PartitionAllocator *) * MAX_THREADS);
    partition_owners = (int64_t *)thr_mem;
    for (int i = 0; i < MAX_THREADS; i++) {
        partition_owners[i] = -1;
    }
    thr_mem += ALIGN_CACHE(sizeof(int64_t *) * MAX_THREADS);
    allocator_list = (Allocator **)thr_mem;

    // reserve the first allocator for the main thread.
    main_thread_id = get_thread_id();
    os_page_size = get_os_page_size();
    tls_create(&_thread_key, &thread_done);
    main_instance = reserve_allocator();
    thread_instance = main_instance;
}

#if defined(__cplusplus)
struct thread_init
{
    thread_init() { allocator_init(); }
    ~thread_init() { allocator_destroy(); }
};
static thread_init init;
#elif defined(WINDOWS)
// set section property.
typedef void cb(void);

#pragma section(".CRT$XIU", long, read)
#pragma data_seg(".CRT$XIU")
static cb *autostart[] = {allocator_init};

#pragma data_seg(".CRT$XPU")
static cb *autoexit[] = {allocator_destroy};

#pragma data_seg() // reset data-segment
#elif defined(__GNUC__) || defined(__clang__)
static void __attribute__((constructor)) library_init(void) { allocator_init(); }
static void __attribute__((destructor)) library_destroy(void) { allocator_destroy(); }
#endif

void __attribute__((malloc)) *cmalloc(size_t s) {
    
    if(get_thread_count() == 1)
    {
        return allocator_malloc(main_instance, s);
    }
    else
    {
        return allocator_malloc(get_thread_instance(), s);
    }
}

void cfree(void *p)
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

void *cmalloc_arena(size_t s, size_t partition_idx)
{
    if (partition_idx > (NUM_AREA_PARTITIONS - 1)) {
        return NULL;
    }
    Allocator *alloc = get_thread_instance();
    void *arena = partition_allocator_get_free_area(alloc->part_alloc, s, (AreaType)partition_idx);
    Arena *header = (Arena *)((uintptr_t)arena + sizeof(Arena_L2));
    Partition* partition = &alloc->part_alloc->area[partition_idx];
    header->partition_id = (uint32_t)partition->partition_id;
    if (arena == NULL) {
        return NULL;
    }
    return arena;
}

void cfree_arena(void* p)
{
    int8_t pid = partition_from_addr((uintptr_t)p);
    if (pid >= 0 && pid < NUM_AREA_PARTITIONS) {
        Arena *arena = (Arena *)((uintptr_t)p + sizeof(Arena_L2));
        const uint32_t part_id = arena->partition_id;
        PartitionAllocator *_part_alloc = partition_allocators[part_id];
        
        partition_allocator_free_area(_part_alloc, (Area*)arena);
    } else {
        free_extended_part(pid, p);
    }
}

void *cmalloc_area(size_t s, size_t partition_idx)
{
    Area *area = cmalloc_arena(sizeof(Area) + s, partition_idx);
    if (area == NULL) {
        return NULL;
    }
    area_reserve_all(area);
    area_set_container_type(area, CT_SLAB);
    return (void *)((uintptr_t)area + sizeof(Area));
}

bool callocator_release(void)
{
    Allocator *alloc = get_thread_instance();
    if (alloc->idx >= 0) {
        return allocator_release_local_areas(alloc);
    }
    return false;
}

bool callocator_reset(void)
{
    /*
    void* blocks = (uint8_t*)p + sizeof(Pool);
    int32_t psize = (1 << size_clss_to_exponent[section->type]);
    const size_t block_memory = psize - sizeof(Pool) - sizeof(uintptr_t);
    const uintptr_t section_end = ((uintptr_t)p + (SECTION_SIZE - 1)) & ~(SECTION_SIZE - 1);
    const size_t remaining_size = section_end - (uintptr_t)blocks;
    const size_t the_end = MIN(remaining_size, block_memory);
    const uintptr_t start_addr = (uintptr_t)blocks;
    const uintptr_t first_page = (start_addr + (os_page_size - 1)) & ~(os_page_size - 1);
    const size_t mem_size = the_end - first_page;
    reset_memory((void*)first_page, mem_size);
    */
    return true;
}

void *cmalloc_from_heap(size_t size) { return allocator_malloc_heap(get_thread_instance(), size); }

void *crealloc(void *p, size_t s)
{
    size_t csize = allocator_get_allocation_size(get_thread_instance(), p);
    // if was in a pool. move it to a heap where it is cheaper to resize.
    // heap can grow.
    size_t min_size = MIN(csize, s);
    void *new_ptr = cmalloc(s);
    memcpy(p, new_ptr, min_size);
    cfree(p);

    return new_ptr;
}

// Allocating memory from the OS that will not conflict with any memory region
// of the allocator.
static spinlock os_alloc_lock = {0};
static uintptr_t os_alloc_hint = 0;
void *cmalloc_os(size_t size)
{
    // align size to page size
    size += CACHE_LINE;
    size = (size + (os_page_size - 1)) & ~(os_page_size - 1);

    // look the counter while fetching our os memory.
    spinlock_lock(&os_alloc_lock);
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
    spinlock_unlock(&os_alloc_lock);

    // write the allocation header
    uint64_t *_ptr = (uint64_t *)ptr;
    *_ptr = (uint64_t)(uintptr_t)ptr + CACHE_LINE;
    *(++_ptr) = size;

    return (void *)((uintptr_t)ptr + CACHE_LINE);
}
