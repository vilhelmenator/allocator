/*
 * Implicit List Memory Allocator
    * A boundary tag allocator that uses an implicit list to manage free blocks.
    * It supports deferred freeing of blocks and can handle alignment requirements.
    * The allocator is designed to be efficient for small to medium-sized allocations.
    * It uses a queue to manage free nodes and atomic operations for thread safety.
 */
#ifndef IMPLICIT_LIST_H
#define IMPLICIT_LIST_H 
#include "callocator.inl"

#define HEADER_OVERHEAD 4
#define HEADER_FOOTER_OVERHEAD 8


bool implicitList_is_connected(const ImplicitList *h);
bool implicitList_has_room(const ImplicitList *h, const size_t s);
void implicitList_place(ImplicitList *h, void *bp, const uint32_t asize, const int32_t header, const int32_t csize);
void *implicitList_find_fit(ImplicitList *h, const uint32_t asize, const uint32_t align);
static inline uint32_t implicitList_block_get_header(HeapBlock *hb) { return *(uint32_t *)((uint8_t *)&hb->data - WSIZE); }

static inline size_t implicitList_get_block_size(void *bp)
{
    HeapBlock *hb = (HeapBlock *)bp;
    int header = implicitList_block_get_header(hb);
    return header & ~0x7;
}

static inline int32_t implicitList_get_good_size(uint32_t s)
{
    if (s <= DSIZE * 2) {
        return DSIZE * 2 + HEADER_FOOTER_OVERHEAD;
    } else {
        //
        // lets be nice to odd sizes. since for allocated items, it only needs the header. 4 bytes.
        // if it is a multiple of 8. We will need to bump up to the next 8 multiple
        // but, for multiples of 4. We can just bump up by 4 bytes.
        if ((s & 0x7) == 0) {
            return DSIZE * ((s + HEADER_FOOTER_OVERHEAD + DSIZE - 1) >> 3);
        } else {
            return WSIZE * ((s + HEADER_OVERHEAD + WSIZE - 1) >> 2);
        }
    }
}

void *implicitList_get_block(ImplicitList *h, uint32_t s, uint32_t align);
bool implicitList_resize_block(ImplicitList *h, void *bp, int32_t size);
static inline void implicitList_update_max(ImplicitList *h, uint32_t size)
{
    if (size > h->max_block) {
        h->max_block = size;
    }
}

void *implicitList_coalesce(ImplicitList *h, void *bp);
void implicitList_reset(ImplicitList *h);
void implicitList_freeAll(ImplicitList *h);
void implicitList_reserve(ImplicitList *h);
void implicitList_free(ImplicitList *h, void *bp, bool should_coalesce);
void implicitList_extend(ImplicitList *h);
void implicitList_init(ImplicitList *h, int8_t pidx, const size_t psize);
void implicitList_move_deferred(ImplicitList *h);
void implicit_list_thread_free(ImplicitList* list, Block* block);
void implicit_list_thread_free_batch(ImplicitList* list, Block* head, Block* tail, uint32_t num);

#endif // IMPLICIT_LIST_H
