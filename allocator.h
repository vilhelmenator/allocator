

#include "callocator.inl"
typedef void* (*internal_alloc) (Allocator *a, const size_t as);
int8_t reserve_any_partition_set(void);
int8_t reserve_any_partition_set_for(const int32_t midx);
bool reserve_partition_set(const int32_t idx, const int32_t midx);
void release_partition_set(const int32_t idx);

Allocator *allocator_aquire(size_t idx);
internal_alloc allocator_set_counter_slot(Allocator *a, void *p, uint32_t slot_size);
void *allocator_malloc(Allocator *a, size_t s);
void *allocator_malloc_heap(Allocator *a, size_t s);
bool allocator_release_local_areas(Allocator *a);
void allocator_free(Allocator *a, void *p);
void allocator_free_th(Allocator *a, void *p);
size_t allocator_get_allocation_size(Allocator *a, void *p);
void free_extended_part(size_t pid, void *p);

void* test_allocator_malloc(Allocator* a, size_t s);
void test_allocator_free(Allocator* a, void* s);
