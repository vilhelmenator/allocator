

#ifndef ALLOCATOR_H
#define ALLOCATOR_H 
#include "callocator.inl"
typedef void* (*internal_alloc) (Allocator *a, const size_t as);
Allocator *allocator_aquire(uintptr_t thread_id, uintptr_t thr_mem);
void *allocator_malloc(Allocator_param *prm);
bool allocator_release_local_areas(Allocator *a);
void allocator_free(Allocator *a, void *p);
void allocator_free_th(Allocator *a, void *p);
size_t allocator_get_size(void *p);
int allocator_try_resize(void*p, const size_t s, size_t *os);
bool allocator_try_release_local_area(Allocator* alloc);

#endif /* ALLOCATOR_H */
