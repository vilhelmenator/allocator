
#include "../cthread/cthread.h"

#include "allocator.inl"

void _list_enqueue(void *queue, void *node, size_t head_offset, size_t prev_offset)
{
    Queue *tq = (Queue *)((uint8_t *)queue + head_offset);
    QNode *tn = (QNode *)((uint8_t *)node + prev_offset);
    tn->next = tq->head;
    tn->prev = NULL;
    if (tq->head != NULL) {
        QNode *temp = (QNode *)((uint8_t *)tq->head + prev_offset);
        temp->prev = node;
        tq->head = node;
    } else {
        tq->tail = tq->head = node;
    }
}

void _list_remove(void *queue, void *node, size_t head_offset, size_t prev_offset)
{
    Queue *tq = (Queue *)((uint8_t *)queue + head_offset);
    QNode *tn = (QNode *)((uint8_t *)node + prev_offset);
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
    if (node == tq->tail) {
        tq->tail = tn->prev;
    }
    tn->next = NULL;
    tn->prev = NULL;
}

Allocator *get_thread_instance(void)
{
    if (get_thread_id() == main_thread_id) {
        return main_instance;
    } else {
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
    uintptr_t thr_mem = (uintptr_t)alloc_memory((void *)partitions_offsets[5], size, true);
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

void *cmalloc(size_t s)
{
    if (get_thread_id() == main_thread_id) {
        return allocator_malloc(main_instance, s);
    } else {
        return allocator_malloc_th(thread_instance, s);
    }
}

void cfree(void *p)
{
    if (get_thread_id() == main_thread_id) {
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
    if (vm_addr >= (((uintptr_t)32 << 40) + os_page_size) && vm_addr < (((uintptr_t)64 << 40) - (2 * os_page_size))) {
        if (vm_addr % os_page_size != 0) {
            // vm address must be a multiple of os page
            return NULL;
        }

        // the header is an os_page in size.
        // align size to page size
        uintptr_t header = vm_addr - os_page_size;
        size += os_page_size;
        size = (size + (os_page_size - 1)) & ~(os_page_size - 1);
        //
        void *ptr = alloc_memory((void *)header, size, true);
        if ((uintptr_t)ptr == header) {
            // we were able to allocate the memory, .. yay!
            uint64_t *_ptr = (uint64_t *)ptr;
            // write the address into the header and the size.
            *_ptr = (uint64_t)(uintptr_t)ptr + os_page_size;
            *(++_ptr) = size;
            return (void *)vm_addr;
        }
        if (ptr != NULL) {
            free_memory(ptr, size);
        }
    }
    return NULL;
}

void *cmalloc_area(size_t s, size_t partition_idx)
{
    if (partition_idx > (NUM_AREA_PARTITIONS - 1)) {
        return NULL;
    }
    Allocator *alloc = get_thread_instance();
    if (alloc->idx < 0) {
        alloc = init_thread_instance();
    }

    const size_t totalSize = sizeof(Area) + s;
    Area *area = partition_allocator_get_free_area(alloc->part_alloc, totalSize, (AreaType)partition_idx);
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
    if (alloc->idx < 0) {
        alloc = init_thread_instance();
    }
    return allocator_release_local_areas(alloc);
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

void *cmalloc_from_heap(size_t size)
{
    if (get_thread_id() == main_thread_id) {
        return allocator_malloc_heap(main_instance, size);
    } else {
        return allocator_malloc_heap_th(thread_instance, size);
    }
}

void *crealloc(void *p, size_t s)
{
    /*
    size_t csize = allocator_get_allocation_size(p);
    if(csize < os_page_size)
    {
        // just allocate the new size and memcpy
    }
    else
    {
        if(allocator_remap_supported())
        {
            // how many full pages can we remap.
            //
        }
        else
        {
            // just memcpy
        }
    }
    // memcpy remainder size.
     */
    return NULL;
}
