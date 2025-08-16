
#ifndef _callocator_h_
#define _callocator_h_
/*
 * Custom memory allocator header file
 * This file defines the interface for a custom memory allocator.
 * It includes functions for allocation, deallocation, and memory management.
 * The allocator is designed to be efficient and thread-safe.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>


void *cmalloc(size_t s);
void cfree(void *p);

void *crealloc(void *p, size_t s);
void *caligned_alloc(size_t alignment, size_t size);
void *caligned_realloc(size_t alignment, size_t size);
bool callocator_release(void);

void *cmalloc_os(size_t s);
void cfree_os(void *p);
void *cmalloc_at(size_t s, uintptr_t vm_addr);

void *zalloc( size_t num, size_t size ); // initilized to zero
void *zaligned_alloc( size_t num, size_t size ); // initilized to zero




#endif /* _callocator_h_ */
