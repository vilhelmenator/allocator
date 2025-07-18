
#ifndef _callocator_h_
#define _callocator_h_


#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
// DONE
void *cmalloc(size_t s);
void cfree(void *p);
// IN PROGRESS
void *crealloc(void *p, size_t s);
void *ccalloc(size_t num, size_t size);

void *caligned_alloc(size_t alignment, size_t size);
void *zalloc( size_t num, size_t size ); // initilized to zero

bool callocator_release(void);
void *cmalloc_at(size_t s, uintptr_t vm_addr);
void *cmalloc_os(size_t s);

// NOT DONE

void callocator_purge(void);
void *cmalloc_from_pool(size_t s);
//
// basic API
//
// cmalloc(  )
// cfree(  )
// crealloc(  )
// ccmalloc(  )
// cmalloc_aligned(  )
//

//
// extended API.
//
// cmalloc_pool(  )
// cmalloc_heap(  )
// cmalloc_area(  )
// cmalloc_at(  )       -- allocate memory at a particular address within a certain range.
// cmalloc_bound(  )    -- bounds the memory allocated by read_only pages that will cause exceptions if touched.
// cpurge(  )   -- remove all memory reserved by thread.        Will release all allocations it got from the OS.
// crelease(  ) -- release all memory that is unused by thread. Will release all the allocations that are not in use.
// creset(  )  -- reset all memory used by current thread.     Will reset OS pages that it has touched. Without
// releasing allocation structures. creport(  )  -- log amount of used memory for particular thread.

#endif /* _callocator_h_ */
