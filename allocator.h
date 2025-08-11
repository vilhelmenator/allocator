

#include "callocator.inl"
typedef void* (*internal_alloc) (Allocator *a, const size_t as);
Allocator *allocator_aquire(uintptr_t thread_id, uintptr_t thr_mem);
void *allocator_malloc(Allocator_param *prm);
bool allocator_release_local_areas(Allocator *a);
void allocator_free(Allocator *a, void *p);
void allocator_free_th(Allocator *a, void *p);
size_t allocator_get_size(Allocator *a, void *p);
