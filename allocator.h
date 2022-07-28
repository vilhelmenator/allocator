

#include "callocator.inl"

int8_t reserve_any_partition_set(void);
int8_t reserve_any_partition_set_for(const int32_t midx);
bool reserve_partition_set(const int32_t idx, const int32_t midx);
void release_partition_set(const int32_t idx);
Allocator *allocator_aquire(size_t idx);

void allocator_set_cached_pool(Allocator *a, Pool *p);

void allocator_thread_enqueue(message_queue *queue, message *first, message *last);

void allocator_flush_thread_free(Allocator *a);

void allocator_thread_free(Allocator *a, void *p, const uint64_t pid);

void allocator_free_from_section(Allocator *a, void *p, Section *section, uint32_t part_id);

void allocator_free_from_container(Allocator *a, void *p, const size_t area_size);
Section *allocator_get_free_section(Allocator *a, const size_t s, SectionType st);

void *allocator_alloc_from_heap(Allocator *a, const size_t s);

void *allocator_alloc_slab(Allocator *a, const size_t s);

Pool *allocator_alloc_pool(Allocator *a, const uint32_t idx, const uint32_t s);

void *allocator_alloc_from_pool(Allocator *a, const size_t s);

void allocator_thread_dequeue_all(Allocator *a, message_queue *queue);

void *allocator_malloc(Allocator *a, size_t s);

void *allocator_malloc_heap(Allocator *a, size_t s);
bool allocator_release_local_areas(Allocator *a);

void allocator_free(Allocator *a, void *p);
void allocator_free_th(Allocator *a, void *p);
size_t allocator_get_allocation_size(Allocator *a, void *p);

