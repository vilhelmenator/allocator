
#include "callocator.inl"
#include "../cthread/cthread.h"
#include "allocator.h"
#include "area.h"
#include "os.h"
#include "partition_allocator.h"

extern PartitionAllocator *partition_allocators[MAX_THREADS];
extern int64_t partition_owners[MAX_THREADS];
extern Allocator *allocator_list[MAX_THREADS];
static mutex_t allocation_mutex = {0};
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

uintptr_t main_thread_id;
Allocator *main_instance;

const Allocator default_alloc = {-1, -1, NULL, {0,0,0,0,0,0,0,0,0,0,0,0}, {{NULL, NULL}, 0, 0, 0, 0}, NULL, {NULL, NULL}};
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
    Allocator *new_alloc = allocator_aquire(idx);
    new_alloc->part_alloc = partition_allocators[idx];
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
    if (ti == &default_alloc) {
        init_thread_instance();
    }
    return thread_instance;
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

static inline bool safe_to_aquire(void *base, void *ptr, size_t size, uintptr_t end)
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


void *alloc_memory_aligned(void *base, uintptr_t end, size_t size, size_t alignment)
{
    // base must be aligned to alignment
    // size must be at least the size of an os page.
    // alignment is smaller than a page size or not a power of two.
    if (!safe_to_aquire(base, NULL, size, end)) {
        return NULL;
    }
    mutex_lock(&allocation_mutex);
    // this is not frequently called
    void *ptr = alloc_memory(base, size, false);
    if (ptr == NULL) {
        goto err;
    }
start:
    if (!safe_to_aquire(base, ptr, size, end)) {
        free_memory(ptr, size);
        ptr = NULL;
        goto err;
    }

    if ((((uintptr_t)ptr & (alignment - 1)) != 0)) {
        // this should happen very rarely, if at all.
        // we at first just allocate the missing part towards the target alignment of the end.
        // be nice to any other thread that comes through here and hope that they at least find this without any problems.
        //
        // release our failed attempt.
        // allocate next part to our unaligned part.
        // if that is successfull,
        //
        void *aligned_p = (void *)(((uintptr_t)ptr + (alignment - 1)) & ~(alignment - 1));
        free_memory(ptr, size);
        
        // Now we attempt to overallocate
        size_t adj_size = size + alignment;
        ptr = alloc_memory(base, adj_size, false);
        if (ptr == NULL) {
            goto err;
        }

        // if the new ptr is not in our current partition set
        if (!safe_to_aquire(base, ptr, adj_size, end)) {
            free_memory(ptr, adj_size);
            ptr = NULL;
            goto err;
        }

        // if we got our aligned memory
        if (((uintptr_t)ptr & (alignment - 1)) != 0) {
            goto success;
        }
        // we are still not aligned, but we have an address that is aligned.
        free_memory(ptr, adj_size);

        aligned_p = (void *)(((uintptr_t)ptr + (alignment - 1)) & ~(alignment - 1));
        // get our aligned address
        ptr = alloc_memory(aligned_p, size, false);
        if (ptr != aligned_p) {
            // Why would this fail?
            free_memory(ptr, size);
            ptr = NULL;
            goto err;
        }
    }
success:
    if (!commit_memory(ptr, size)) {
        // something is greatly foobar.
        // not allowed to commit the memory we reserved.
        free_memory(ptr, size);
        ptr = NULL;
    }
err:
    mutex_unlock(&allocation_mutex);
    return ptr;
}
static int allocator_destroy()
{
    tls_set(_thread_key, NULL);
    tls_delete(_thread_key);
    return 0;
}

static int allocator_init()
{
    static bool init = false;
    if (init)
        return 0;
    init = true;

    for(int32_t i = 0; i < MAX_THREADS; i++)
    {
        partition_allocators[i] = NULL;
        allocator_list[i] = NULL;
        partition_owners[i] = -1;
    }

    // reserve the first allocator for the main thread.
    mutex_init(&allocation_mutex);
    main_thread_id = get_thread_id();
    os_page_size = get_os_page_size();
    tls_create(&_thread_key, &thread_done);
    
    
    PartitionAllocator* part_alloc = partition_allocator_init_default();
    uintptr_t end = ((uintptr_t)part_alloc + ALIGN_CACHE(sizeof(PartitionAllocator)));
    uintptr_t alloc_addr = end - DEFAULT_OS_PAGE_SIZE;
    Allocator *new_alloc = (Allocator *)alloc_addr;
    allocator_set_counter_slot(new_alloc, (void*)end, DEFAULT_OS_PAGE_SIZE, 0, 0);
    allocator_list[0] = new_alloc;
    partition_owners[0] = 0;
    new_alloc->part_alloc = part_alloc;
    list_enqueue(&new_alloc->partition_allocators, part_alloc);
    
    main_instance = new_alloc;
    thread_instance = main_instance;
    return 0;
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
    Allocator_param params = {get_thread_id(), s, sizeof(intptr_t), false};
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

void *cmalloc_arena(size_t s, size_t partition_idx)
{
    if (partition_idx > (NUM_AREA_PARTITIONS - 1)) {
        return NULL;
    }
    Allocator *alloc = get_thread_instance();
    int32_t area_idx = -1;
    void *partition = partition_allocator_get_free_area(alloc->part_alloc, partition_idx, area_type_to_size[partition_idx], &area_idx, false);
    if(partition == NULL)
    {
        return NULL;
    }
    void* arena = partition_allocator_area_at_idx(alloc->part_alloc, partition, area_idx);
    Arena *header = (Arena *)(uintptr_t)arena;
    header->partition_id = (uint32_t)alloc->part_alloc->idx;

    return arena;
}

void cfree_arena(void* p)
{
    int8_t pid = partition_id_from_addr((uintptr_t)p);
    if (pid >= 0 && pid < NUM_AREA_PARTITIONS) {
        Arena *arena = (Arena *)(uintptr_t)p;
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

void *crealloc(void *p, size_t s)
{
    size_t csize = allocator_get_allocation_size(get_thread_instance(), p);
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
static uintptr_t os_alloc_hint = 1ULL << 39;
void *cmalloc_os(size_t size)
{
    // align size to page size
    size += CACHE_LINE;
    size = (size + (os_page_size - 1)) & ~(os_page_size - 1);
    mutex_lock(&allocation_mutex);
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
    mutex_unlock(&allocation_mutex);

    // write the allocation header
    uint64_t *_ptr = (uint64_t *)ptr;
    *_ptr = (uint64_t)(uintptr_t)ptr + CACHE_LINE;
    *(++_ptr) = size;

    return (void *)((uintptr_t)ptr + CACHE_LINE);
}
