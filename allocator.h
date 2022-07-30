

#include "callocator.inl"

int8_t reserve_any_partition_set(void);
int8_t reserve_any_partition_set_for(const int32_t midx);
bool reserve_partition_set(const int32_t idx, const int32_t midx);
void release_partition_set(const int32_t idx);

Allocator *allocator_aquire(size_t idx);

void *allocator_malloc(Allocator *a, size_t s);
void *allocator_malloc_heap(Allocator *a, size_t s);
bool allocator_release_local_areas(Allocator *a);
void allocator_free(Allocator *a, void *p);
void allocator_free_th(Allocator *a, void *p);
size_t allocator_get_allocation_size(Allocator *a, void *p);
