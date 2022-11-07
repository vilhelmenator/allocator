

#include "heap.h"
#include "section.h"

static inline void implicitList_block_set_header(HeapBlock *hb, const uint32_t s, const uint32_t v, const uint32_t pa)
{
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
    if ((csize - asize) >= (DSIZE + HEADER_FOOTER_OVERHEAD)) {
        implicitList_block_set_header(hb, asize, 1, prev_alloc);
        hb = implicitList_block_next(hb);
        implicitList_block_set_header(hb, csize - asize, 0, 1);
        implicitList_block_set_footer(hb, csize - asize, 0);
        list_enqueue(&h->free_nodes, (QNode *)hb);
    } else {
        implicitList_block_set_header(hb, csize, 1, prev_alloc);
    }
}

void *implicitList_find_fit(ImplicitList *h, const uint32_t asize)
{
    // find the first fit.
    QNode *current = (QNode *)h->free_nodes.head;
    while (current != NULL) {
        HeapBlock *hb = (HeapBlock *)current;
        int header = implicitList_block_get_header(hb);
        uint32_t bsize = header & ~0x7;
        if (asize <= bsize) {
            list_remove(&h->free_nodes, current);
            implicitList_place(h, current, asize, header, bsize);
            return current;
        }
        current = (QNode *)current->next;
    }
    return NULL;
}

void *implicitList_get_block(ImplicitList *h, uint32_t s)
{
    if (h->num_allocations++ == 0) {
        // on first allocation we write our footer at the end.
        // we delay this just so that we do not touch the pages till needed
        uint8_t *blocks = (uint8_t *)h + sizeof(ImplicitList);
        HeapBlock *hb = (HeapBlock *)(blocks + DSIZE * 2);
        implicitList_block_set_footer(hb, h->total_memory, 0);
        implicitList_block_set_header(implicitList_block_next(hb), 0, 1, 0);
        //
        if (h->total_memory < SECTION_SIZE) {
            Section *section = (Section *)((uintptr_t)h & ~(SECTION_SIZE - 1));
            section_reserve_all(section);
        } else {
            size_t area_size = area_size_from_addr((uintptr_t)h);
            Area *area = (Area *)((uintptr_t)h & ~(area_size - 1));
            area_reserve_all(area);
        }
    }
    s = implicitList_get_good_size(s);
    void *ptr = implicitList_find_fit(h, s);
    h->used_memory += s;
    h->max_block -= s;
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
        if (h->total_memory < SECTION_SIZE) {
            // if we have been placed inside of a section.
            Section *section = (Section *)((uintptr_t)h & ~(SECTION_SIZE - 1));
            section_free_all(section);
        } else {
            size_t area_size = area_size_from_addr((uintptr_t)h);
            Area *area = (Area *)((uintptr_t)h & ~(area_size - 1));
            area_free_all(area);
        }
        implicitList_reset(h);
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
    const uintptr_t section_end = ((uintptr_t)h + (psize - 1)) & ~(psize - 1);
    const size_t remaining_size = section_end - (uintptr_t)blocks;

    const size_t block_memory = psize - sizeof(ImplicitList) - sizeof(Section);
    const size_t header_footer_offset = sizeof(uintptr_t) * 2;
    h->idx = pidx;
    h->used_memory = 0;
    h->total_memory = (uint32_t)((MIN(remaining_size, block_memory)) - header_footer_offset - HEADER_FOOTER_OVERHEAD);
    h->max_block = h->total_memory;
    h->min_block = sizeof(uint32_t);
    h->num_allocations = 0;
    h->next = NULL;
    h->prev = NULL;
    implicitList_extend(h);
}
