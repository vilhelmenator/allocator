
#ifndef _callocator_h_
#define _callocator_h_


#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
// DONE
void *cmalloc(size_t s);
void cfree(void *p);
void *cmalloc_os(size_t s);
void cfree_os(void *p);
void *cmalloc_at(size_t s, uintptr_t vm_addr);

// IN PROGRESS
void *crealloc(void *p, size_t s);
void *ccalloc(size_t num, size_t size);

void *caligned_alloc(size_t alignment, size_t size);
void *zalloc( size_t num, size_t size ); // initilized to zero

bool callocator_release(void);



#endif /* _callocator_h_ */
