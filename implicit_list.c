

#include "implicit_list.h"
#include <stdatomic.h>

#define MIN_BLOCK_SIZE (DSIZE + HEADER_FOOTER_OVERHEAD)


static inline void implicitList_block_set_header(HeapBlock *hb, const uint32_t s, const uint32_t v, const uint32_t pa)
{
    // Set the header with size, allocated bit, and previous allocated bit
    *(uint32_t *)((uint8_t *)&hb->data - WSIZE) = (s | v | pa << 1);
}

static inline void implicitList_block_set_footer(HeapBlock *hb, const uint32_t s, const uint32_t v)
{
    const uint32_t size = (*(uint32_t *)((uint8_t *)&hb->data - WSIZE)) & ~0x7;
    *(uint32_t *)((uint8_t *)(&hb->data) + (size)-DSIZE) = (s | v);
}

static inline HeapBlock *implicitList_block_next(HeapBlock *hb)
{
    const uint32_t size = *(uint32_t *)((uint8_t *)&hb->data - WSIZE) & ~0x7;
    return (HeapBlock *)((uint8_t *)&hb->data + (size));
}

static inline HeapBlock *implicitList_block_prev(HeapBlock *hb)
{
    const uint32_t size = *(uint32_t *)((uint8_t *)&hb->data - DSIZE) & ~0x7;
    return (HeapBlock *)((uint8_t *)&hb->data - (size));
}

bool implicitList_is_connected(const ImplicitList *h) { return h->prev != NULL || h->next != NULL; }

bool implicitList_has_room(const ImplicitList *h, const size_t s)
{
    if ((h->used_memory + s + HEADER_FOOTER_OVERHEAD) > h->total_memory) {
        return false;
    }
    if (s <= h->max_block && s >= h->min_block) {
        return true;
    }
    return false;
}

void implicitList_place(ImplicitList *h, void *bp, const uint32_t asize, const int32_t header, const int32_t csize)
{
    HeapBlock *hb = (HeapBlock *)bp;
    const uint32_t prev_alloc = (header & 0x3) >> 1;
    if ((csize - asize) >= (MIN_BLOCK_SIZE)) {
        implicitList_block_set_header(hb, asize, 1, prev_alloc);
        hb = implicitList_block_next(hb);
        implicitList_block_set_header(hb, csize - asize, 0, 1);
        implicitList_block_set_footer(hb, csize - asize, 0);
        list_enqueue(&h->free_nodes, (QNode *)hb);
    } else {
        implicitList_block_set_header(hb, csize, 1, prev_alloc);
    }
}

void* implicitList_place_aligned(ImplicitList *h, void *bp,  uint32_t asize, const int32_t header, const int32_t bsize, const int32_t alignment)
{
    uintptr_t raw_addr = (uintptr_t)bp;
    uintptr_t user_addr = (raw_addr + sizeof(void*) + alignment - 1) & ~(alignment - 1);
    uintptr_t prefix_size = user_addr - raw_addr;
    const uint32_t prev_alloc = (header & 0x3) >> 1;

    // Split prefix if needed
    if (prefix_size > 0 && prefix_size < MIN_BLOCK_SIZE) {
        user_addr = raw_addr + ((MIN_BLOCK_SIZE + alignment - 1) & ~(alignment - 1));
        prefix_size = user_addr - raw_addr;
        if ((prefix_size + asize + HEADER_FOOTER_OVERHEAD) >= bsize) {
            // Not enough space for aligned block, fail allocation
            return NULL;
        }
        else {
            // the prefix size should have enough space for a valid block
            implicitList_block_set_header((HeapBlock *)raw_addr, prefix_size, 0, prev_alloc);
            implicitList_block_set_footer((HeapBlock *)raw_addr, prefix_size, 0);
            list_enqueue(&h->free_nodes, (QNode *)raw_addr);
        }
    }

    list_remove(&h->free_nodes, (QNode*)bp);
    
    // Set up the aligned block
    HeapBlock *aligned_hb = (HeapBlock *)user_addr;
    implicitList_block_set_header(aligned_hb, asize, 1, prefix_size > 0 ? 0 : prev_alloc); 
    implicitList_block_set_footer(aligned_hb, asize, 1);

    uintptr_t suffix_size = bsize - prefix_size - asize;
    // Ensure suffix is at least MIN_BLOCK_SIZE
    if (suffix_size > 0 && suffix_size < MIN_BLOCK_SIZE) {
        // Add the small suffix to the aligned block
        asize += suffix_size;
        suffix_size = 0;
    }
    else 
    {
        HeapBlock *suffix_hb = (HeapBlock *)(user_addr + asize);
        implicitList_block_set_header(suffix_hb, suffix_size, 0, 1);
        implicitList_block_set_footer(suffix_hb, suffix_size, 0);
        list_enqueue(&h->free_nodes, (QNode *)suffix_hb);
    }

    return (void *)user_addr;
}
// Moves all thread_free blocks to deferred_free (call from owning thread)
void implicit_list_claim_thread_frees(ImplicitList* list) {
    // Atomically extract the entire thread_free list
    Block* head = atomic_exchange_explicit(
        &list->thread_free,
        NULL,
        memory_order_acquire  // Ensures we see all prior releases
    );
    
    // Prepend to deferred_free (no atomic needed - owner thread only)
    if (head) {
        Block* tail = head;
        while (tail->next) {
            tail = tail->next;
        }
        tail->next = list->deferred_free;
        list->deferred_free = head;
    }
}

void implicitList_move_deferred(ImplicitList *h)
{
    // for every item in the deferred list.
    Block *current = h->free_nodes.head;
    // move thread free items to the deferred list.
    implicit_list_claim_thread_frees(h);
    // move deferred to our free list.
    h->free_nodes.head = h->deferred_free;
    Block* c = h->deferred_free;
    Block* tail = NULL;
    while(c != NULL)
    {
        tail = c;
        //
        HeapBlock *hb = (HeapBlock *)c;
        int header = implicitList_block_get_header(hb);
        h->used_memory -= header & ~0x7;
        c = c->next;
        h->num_allocations--;
    }
    tail->next = current;
    h->free_nodes.tail = tail;
    h->deferred_free = NULL;
    if (h->num_allocations == 0) {
        implicitList_freeAll(h);
    }
}

void *implicitList_find_fit(ImplicitList *h, const uint32_t asize, const uint32_t align)
{
    // find the first fit.
    QNode *current = (QNode *)h->free_nodes.head;
    if(current == NULL)
    {
        implicitList_move_deferred(h);
        current = (QNode *)h->free_nodes.head;
    }
    while (current != NULL) {
        HeapBlock *hb = (HeapBlock *)current;
        int header = implicitList_block_get_header(hb);
        uint32_t bsize = header & ~0x7;
        if (asize <= bsize) {
            
            if(align != sizeof(void*))
            {
                // Align the size to the next multiple of align
                void* res = implicitList_place_aligned(h, current, asize, header, bsize, align);
                if(res != NULL)
                {
                    return res;
                }
            }   
            else
            {
                list_remove(&h->free_nodes, current);
                implicitList_place(h, current, asize, header, bsize);
                return current;
            }
        }
        current = (QNode *)current->next;
    }
    return NULL;
}

void implicitList_freeAll(ImplicitList *h)
{
    implicitList_reset(h);
}


void *implicitList_get_block(ImplicitList *h, uint32_t s, uint32_t align)
{
    if(align != sizeof(void*))
    {
        // Align the size to the next multiple of align
        s = s + align - 1 + sizeof(void*);
    }
    if (!implicitList_has_room(h, s)) {
        return NULL;
    }
    if (h->num_allocations++ == 0) {
        // on first allocation we write our footer at the end.
        // we delay this just so that we do not touch the pages till needed
        uint8_t *blocks = (uint8_t *)h + sizeof(ImplicitList);
        HeapBlock *hb = (HeapBlock *)(blocks + DSIZE * 2);
        implicitList_block_set_footer(hb, h->total_memory, 0);
        implicitList_block_set_header(implicitList_block_next(hb), 0, 1, 0);

    }
    s = implicitList_get_good_size(s);
    void *ptr = implicitList_find_fit(h, s, align);
    if(ptr != NULL)
    {
        h->used_memory += s;
        h->max_block -= s;
    }
    else
    {
        h->num_allocations--;
    }
    return ptr;
}

bool resize_block(ImplicitList *h, void *bp, int32_t size)
{
    //
    HeapBlock *hb = (HeapBlock *)bp;
    int header = implicitList_block_get_header(hb);
    int32_t bsize = header & ~0x7;

    //
    HeapBlock *next_block = implicitList_block_next(hb);
    int next_header = implicitList_block_get_header(next_block);
    const size_t next_alloc = next_header & 0x1;
    if (next_alloc) {
        // next block is not free so we can't merge with it.
        return false;
    }
    //
    const size_t next_size = next_header & ~0x7;
    if ((bsize + next_size) >= size) {
        // merge the two blocks
        const uint32_t prev_alloc = (header & 0x3) >> 1;
        size += next_size;
        implicitList_block_set_header(hb, size, 0, prev_alloc);
        implicitList_block_set_footer(hb, size, 0);
        list_remove(&h->free_nodes, (QNode *)next_block);
        return true;
    }
    //
    return false;
}

void *implicitList_coalesce(ImplicitList *h, void *bp)
{
    HeapBlock *hb = (HeapBlock *)bp;
    int header = implicitList_block_get_header(hb);
    int32_t size = header & ~0x7;
    const uint32_t prev_alloc = (header & 0x3) >> 1;
    HeapBlock *next_block = implicitList_block_next(hb);
    int next_header = implicitList_block_get_header(next_block);
    const size_t next_alloc = next_header & 0x1;

    QNode *hn = (QNode *)bp;
    if (!(prev_alloc && next_alloc)) {

        const size_t next_size = next_header & ~0x7;

        // next is free
        if (prev_alloc && !next_alloc) {
            size += next_size;
            QNode *h_next = (QNode *)next_block;
            list_remove(&h->free_nodes, h_next);
            implicitList_block_set_header(hb, size, 0, 1);
            implicitList_block_set_footer(hb, size, 0);
            list_enqueue(&h->free_nodes, hn);
        } // prev is fre
        else {
            HeapBlock *prev_block = implicitList_block_prev(hb);
            int prev_header = implicitList_block_get_header(prev_block);
            const size_t prev_size = prev_header & ~0x7;
            const uint32_t pprev_alloc = (prev_header & 0x3) >> 1;
            if (!prev_alloc && next_alloc) {
                size += prev_size;
                implicitList_block_set_footer(hb, size, 0);
                implicitList_block_set_header(prev_block, size, 0, pprev_alloc);
                bp = (void *)implicitList_block_prev(hb);
            } else { // both next and prev are free
                size += prev_size + next_size;
                QNode *h_next = (QNode *)next_block;
                list_remove(&h->free_nodes, h_next);
                implicitList_block_set_header(prev_block, size, 0, pprev_alloc);
                implicitList_block_set_footer(next_block, size, 0);
                bp = (void *)implicitList_block_prev(hb);
            }
        }
    } else {
        list_enqueue(&h->free_nodes, hn);
    }
    implicitList_update_max(h, size);

    return bp;
}

void implicitList_reset(ImplicitList *h)
{
    uint8_t *blocks = (uint8_t *)h + sizeof(ImplicitList);
    h->free_nodes.head = NULL;
    h->free_nodes.tail = NULL;
    HeapBlock *hb = (HeapBlock *)(blocks + DSIZE * 2);
    list_enqueue(&h->free_nodes, (QNode *)hb);
    implicitList_block_set_header(hb, h->total_memory, 0, 1);

    h->max_block = h->total_memory;
    h->used_memory = 0;
    h->deferred_free = NULL;
}

void implicitList_free(ImplicitList *h, void *bp, bool should_coalesce)
{
    if (bp == 0)
        return;

    HeapBlock *hb = (HeapBlock *)bp;
    int header = implicitList_block_get_header(hb);
    const uint32_t size = header & ~0x7;
    const uint32_t prev_alloc = (header & 0x3) >> 1;
    implicitList_block_set_header(hb, size, 0, prev_alloc);
    implicitList_block_set_footer(hb, size, 0);

    if (should_coalesce) {
        implicitList_coalesce(h, bp);
    } else {
        list_enqueue(&h->free_nodes, (QNode *)bp);
        implicitList_update_max(h, size);
    }
    h->used_memory -= size;

    if (--h->num_allocations == 0) {
        implicitList_freeAll(h);
    }
}

void implicitList_extend(ImplicitList *h)
{
    uint32_t *blocks = (uint32_t *)((uint8_t *)h + sizeof(ImplicitList));
    blocks[0] = 0;
    blocks[1] = DSIZE | 1;
    blocks[2] = DSIZE | 1;
    blocks[3] = 1;

    implicitList_reset(h);
}

void implicitList_init(ImplicitList *h, int8_t pidx, const size_t psize)
{
    void *blocks = (uint8_t *)h + sizeof(ImplicitList);
    const uintptr_t section_end = ((uintptr_t)blocks + (psize - 1)) & ~(psize - 1);
    const size_t remaining_size = section_end - (uintptr_t)blocks;

    const size_t block_memory = psize - sizeof(ImplicitList);
    const size_t header_footer_offset = sizeof(uintptr_t) * 2;
    h->idx = pidx;
    h->used_memory = 0;
    h->total_memory = (uint32_t)((MIN(remaining_size, block_memory)) - header_footer_offset - HEADER_FOOTER_OVERHEAD);
    h->max_block = h->total_memory;
    h->min_block = sizeof(uint32_t);
    h->num_allocations = 0;
    h->next = NULL;
    h->prev = NULL;
    h->deferred_free = NULL;
    implicitList_extend(h);
}

// Adds a block to the thread_free list from another thread
void implicit_list_thread_free(ImplicitList* list, Block* block) {
    // Set block's next pointer
    block->next = atomic_load_explicit(&list->thread_free, memory_order_relaxed);
    
    // loop to atomically prepend the block
    while (!atomic_compare_exchange_weak_explicit(
        &list->thread_free,
        &block->next,  // Expected current head
        block,         // New head
        memory_order_release,  // Success ordering
        memory_order_relaxed   // Failure ordering
    )) {
        //update block->next and retry
        block->next = atomic_load_explicit(&list->thread_free, memory_order_relaxed);
    }
}

void implicit_list_thread_free_batch(ImplicitList* list, Block* head, Block* tail) {
    // Link the batch to current head
    tail->next = atomic_load_explicit(&list->thread_free, memory_order_relaxed);
    
    // CAS loop to atomically prepend the batch
    while (!atomic_compare_exchange_weak_explicit(
        &list->thread_free,
        &tail->next,  // Expected current head
        head,         // New head (start of batch)
        memory_order_release,
        memory_order_relaxed
    )) {
        // Update tail->next if CAS fails
        tail->next = atomic_load_explicit(&list->thread_free, memory_order_relaxed);
    }
}

