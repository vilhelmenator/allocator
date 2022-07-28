

#include "callocator.inl"

#define WSIZE 4
#define DSIZE 8
#define HEADER_OVERHEAD 4
#define HEADER_FOOTER_OVERHEAD 8

static inline uint8_t size_to_heap(const size_t as)
{
    if (as <= SMALL_OBJECT_SIZE) {
        return 0;
    } else if (as <= MEDIUM_OBJECT_SIZE) {
        return 1; // 128Mb pages
    } else if (as <= LARGE_OBJECT_SIZE) {
        return 2; // 128Mb pages
    } else if (as <= HUGE_OBJECT_SIZE) {
        return 3; // 128Mb pages
    } else {
        return 4; // 256Mb pages
    }
}
bool heap_is_connected(const Heap *h);

bool heap_has_room(const Heap *h, const size_t s);

void heap_place(Heap *h, void *bp, const uint32_t asize, const int32_t header, const int32_t csize);

void *heap_find_fit(Heap *h, const uint32_t asize);
static inline uint32_t heap_block_get_header(HeapBlock *hb) { return *(uint32_t *)((uint8_t *)&hb->data - WSIZE); }

static inline size_t heap_get_block_size(Heap *h, void *bp)
{
    HeapBlock *hb = (HeapBlock *)bp;
    int header = heap_block_get_header(hb);
    return header & ~0x7;
}

static inline int32_t heap_get_good_size(uint32_t s)
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

void *heap_get_block(Heap *h, uint32_t s);

bool resize_block(Heap *h, void *bp, int32_t size);

static inline void heap_update_max(Heap *h, int32_t size)
{
    if (size > h->max_block) {
        h->max_block = size;
    }
}

void *heap_coalesce(Heap *h, void *bp);

void heap_reset(Heap *h);

void heap_free(Heap *h, void *bp, bool should_coalesce);

void heap_extend(Heap *h);

void heap_init(Heap *h, int8_t pidx, const size_t psize);
