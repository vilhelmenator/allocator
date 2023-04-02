
#include "area.h"
#include "../cthread/cthread.h"
#include "os.h"



// os_alloc_blocks
// os_free_block
// partition allocator
// 
void *alloc_memory_aligned_blocks(uint8_t partition, size_t count)
{
    //os_allocator  : partition_queue[partition_max];
    //              : init_counter[partition_max];
    //uint8_t partition_set =
    // each partition has 64 areas
    // we will attempt to allocate each of those areas and pluck them into a free list.
    // we will only reserve the memory for those blocks and not commit them.
    // we reserve the blocks, but only commit the first page of each served area so we can write into it and attach
    // it to a list.
    return NULL;
}

