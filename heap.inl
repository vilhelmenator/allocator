
#ifndef heap_h
#define heap_h
#include "section.inl"

#define WSIZE 4
#define DSIZE 8
#define HEADER_OVERHEAD 4
#define HEADER_FOOTER_OVERHEAD 8
#define HEAP_PARTS 64
#define HEAP_AVAIL_1 3
#define HEAP_AVAIL_2 4
#define HEAP_RANGE_OFFSET 16
#define HEAP_RANGE_FIELD 48
#define HEAP_BITS_PER_RANGE 6
#define HEAP_BITS_QUAD (HEAP_BITS_PER_RANGE * 4)
#define HEAP_BITS_GROUP (HEAP_BITS_QUAD/HEAP_BITS_PER_RANGE)
/*
 
 heap allocations.
 // Layout
 [L3     ] 1     [(headers 128b)(masks 192b)| data   ]
 [L2     ] 64    [(masks 128b)   | data              ]
 [L1     ] 4096  [(masks 64b)    | data              ]
 // init

 [ L1 reserved | L1 next reserved ] [ L2 reserved | L2 next reserved ] [ L3 reserved | need_zero_init ]
 [ L1 reserved | L1 next reserved ] [ L2 reserved | need_zero_init ]
 [ L1 reserved | need_zero_init ]

 //
 // allocate
 // When there is no room for a L3 allocation.
 // might need to search 64 masks, if remaning memory allows.
 // When there is no room for a L2 allocation
 // anything more than L1 will only succeed if the cached L1 mask has room.
 //
 // L3
 // reserve L3 (range)
 // tag filter of L2 and L1 at L3 root. ( range )

 // L2 -
 // reserve L2 (range)
 // tag filter of L1 at L2 root ( range )
 // reserve from L3 at L3 root. ( 1 bit )
 // if mask full: reserve from L2 at L3 root. ( 1 bit )
 // if mask full: reserve from L1 at L3 root. ( 1 bit )

 // L1
 // reserve L1 ( range )
 // reserve L2 at L2 root ( 1 bit )
 // reserve L3 at L3 root ( 1 bit )


 // free -----
 // L3
 // free L3 (range)
 // zero_init_mark L3 (range) (L1 and L2 allocations will need to zero initialize)
 // tag filter of L2 and L1 at L3 root. ( range )

 // L2
 // free L2 (range)
 // zero_init_mark L2 (range) (L1 allocations will need to zero initialize)
 // tag filter of L1 at L2 root ( range )
 // if mask empty: free from L3 at L3 root. ( 1 bit )

 // L1
 // free L1 ( range )
 // if mask empty : free L2 at L2 root ( 1 bit )
 // if L2 mask empty from previous step: free L3 at L3 root ( 1 bit )
 */
const uint32_t new_heap_container_overhead = HEAP_PARTS + HEAP_PARTS*HEAP_PARTS + HEAP_PARTS*HEAP_PARTS*HEAP_PARTS;
const uint32_t new_heap_level_size[] = {0, HEAP_PARTS, HEAP_PARTS*HEAP_PARTS};
const uint32_t new_heap_level_offset[] = { 0, HEAP_PARTS, HEAP_PARTS+HEAP_PARTS*HEAP_PARTS};

static inline uint32_t new_heap_reset_range_for_index(uintptr_t mask_addr, size_t idx)
{
    // 7.5 = 15 / 2
    // rem = 15 - 7.5*2
    // c = a / b
    // rem = a - c*b
    const uint32_t omasks [] = {0x00003F00, 0x000F6000, 0x03F00000, 0xF6000000};
    const uint32_t imasks [] = {0xFFFF60FF, 0xFFF03FFF, 0xF60FFFFF, 0x03FFFFFF};
    const uint32_t bytegroup = (( (uint32_t)idx * HEAP_BITS_PER_RANGE )*0xAAAAAAAB) >> 3;
    const uint32_t rem = ( (uint32_t)idx * HEAP_BITS_PER_RANGE ) - bytegroup*HEAP_BITS_QUAD;           // 0, 6, 12, 18
    const uint32_t subgroup = (rem * 0xAAAAAAAB) >> 1;; // 0 - 3
    uintptr_t start_addr = mask_addr + HEAP_RANGE_OFFSET;
    uintptr_t targt_addr = start_addr + bytegroup * HEAP_BITS_GROUP;
    uint32_t range = ((omasks[subgroup] & *(uint32_t*)targt_addr)) >> (rem + 8);
    *(uint32_t*)targt_addr = ((imasks[subgroup] & *(uint32_t*)targt_addr));
    return range;
}

static inline uint32_t new_heap_get_range_for_index(uintptr_t mask_addr, size_t idx)
{
    const uint32_t omasks [] = {0x00003F00, 0x000F6000, 0x03F00000, 0xF6000000};
    const uint32_t bytegroup = (( (uint32_t)idx * HEAP_BITS_PER_RANGE )*0xAAAAAAAB) >> 3;
    const uint32_t rem = ( (uint32_t)idx * HEAP_BITS_PER_RANGE ) - bytegroup*HEAP_BITS_QUAD;           // 0, 6, 12, 18
    const uint32_t subgroup = (rem * 0xAAAAAAAB) >> 1;; // 0 - 3
    uintptr_t start_addr = mask_addr + HEAP_RANGE_OFFSET;
    uintptr_t targt_addr = start_addr + bytegroup * HEAP_BITS_GROUP;
    return ((omasks[subgroup] & *(uint32_t*)targt_addr)) >> (rem + 8);
}
static inline void new_heap_set_range_for_index(uintptr_t mask_addr, size_t range, size_t idx)
{
    const uint32_t masks [] = {0xFFFF60FF, 0xFFF03FFF, 0xF60FFFFF, 0x03FFFFFF};
    uintptr_t start_addr = mask_addr + HEAP_RANGE_OFFSET;
    const uint32_t bytegroup = (( (uint32_t)idx * HEAP_BITS_PER_RANGE )*0xAAAAAAAB) >> 3;
    const uint32_t rem = ( (uint32_t)idx * HEAP_BITS_PER_RANGE ) - bytegroup*HEAP_BITS_QUAD;           // 0, 6, 12, 18
    const uint32_t subgroup = (rem * 0xAAAAAAAB) >> 1;; // 0 - 3
    uintptr_t targt_addr = start_addr + bytegroup * HEAP_BITS_GROUP;
    uint32_t val = ((masks[subgroup] & *(uint32_t*)targt_addr)) | ((uint32_t)range << (rem + 8));
    *(uint32_t*)targt_addr = val;
}

static inline uintptr_t new_heap_get_mask_addr(Heap* h, size_t i, size_t j)
{
    uintptr_t base = ((uintptr_t)h & ~(os_page_size - 1));
    return base + new_heap_level_offset[i] + new_heap_level_size[i] * j;
}

static inline uintptr_t new_heap_get_data_addr(Heap* h, size_t i, size_t j, size_t k)
{
    uintptr_t base = ((uintptr_t)h & ~(os_page_size - 1));
    return base + (1 << (h->container_exponent - 6))*i + (1 << ((h->container_exponent - 12)))*j + (1 << (h->container_exponent - 18))*k;
}

static inline int32_t get_next_mask_idx(uint64_t mask, uint32_t cidx)
{
    uint64_t msk_cpy = mask << cidx;
    if (msk_cpy == 0 || (cidx > 63)) {
        return -1;
    }
    return __builtin_clzll(msk_cpy) + cidx;
}

static inline uintptr_t reserve_range_idx(size_t range, size_t idx){
    return ((1UL << range) - 1UL) << idx;
}

void new_heap_init_base_range(uintptr_t baseptr)
{
    *(uint64_t*)baseptr = 1UL << 63;
    for(int i = 1; i < 64; i++)
    {
        *(uint64_t*)(baseptr + sizeof(uint64_t)*i) = 0;
    }
}

uint64_t new_heap_init_head_range(Heap* nheap, uintptr_t mask_offset)
{
    uintptr_t head = ((uintptr_t)nheap & ~(os_page_size - 1));
    const uintptr_t data_start = mask_offset + CACHE_LINE * 3;
    // setup initial masks.
    uintptr_t header_size = data_start - head;
    uintptr_t range = header_size >> (nheap->container_exponent - 18);
    uintptr_t rem = ( header_size & ((1<< (nheap->container_exponent - 18)) - 1));
    range += (rem) ? 1 : 0;
    return reserve_range_idx(range, 64 - range);
}

Heap* heap_init(uintptr_t base_addr, int32_t idx, size_t heap_size_exponent)
{
    
    Heap* nheap = (Heap*)base_addr;
    nheap->idx = idx;
    nheap->container_exponent = (uint32_t)heap_size_exponent;
    uintptr_t mask_offset = (base_addr + sizeof(Heap) + (CACHE_LINE - 1)) & ~(CACHE_LINE - 1);

    nheap->num_allocations = 0;
    if(true)
    {
        // high allocations
        *(uint64_t*)mask_offset = new_heap_init_head_range(nheap, mask_offset);
        *(uint64_t*)(mask_offset + sizeof(uint64_t)) = 0;
        mask_offset += CACHE_LINE;
        // lowest allocations
        *(uint64_t*)mask_offset = 1UL << 63;
        *(uint64_t*)(mask_offset + sizeof(uint64_t)) = 0;
        mask_offset += CACHE_LINE;
        // mid allocations
        *(uint64_t*)mask_offset = 1UL << 63;
    }
    else
    {
        *(uint64_t*)mask_offset = 0;
        *(uint64_t*)(mask_offset + sizeof(uint64_t)) = 0;
        mask_offset += CACHE_LINE;
        *(uint64_t*)mask_offset = 0;
        *(uint64_t*)(mask_offset + sizeof(uint64_t)) = 0;
        mask_offset += CACHE_LINE;
        *(uint64_t*)mask_offset = 0;
        *(uint64_t*)(mask_offset + sizeof(uint64_t)) = 0;
    }
    
    return nheap;
}


bool heap_has_room(Heap*h, size_t size)
{
    uintptr_t level_size = 1 << (h->container_exponent - 6);
    uintptr_t mask_offset = ((uintptr_t)h + sizeof(Heap) + (CACHE_LINE - 1)) & ~(CACHE_LINE - 1);
    uintptr_t data_offset = mask_offset + CACHE_LINE*3;
    uintptr_t end_offset = (data_offset + (level_size - 1)) & ~(level_size - 1);
    uint64_t* masks[] = {
        (uint64_t*)mask_offset,
        (uint64_t*)(mask_offset + CACHE_LINE),
        (uint64_t*)(mask_offset + CACHE_LINE*2)};
        
    if((*masks[0] == UINT64_MAX) && (*masks[1] == UINT64_MAX) && (*masks[2] == UINT64_MAX))
    {
        return false;
    }
    size_t limit = end_offset - data_offset;
    if((*masks[1] == UINT64_MAX) || (*masks[2] == UINT64_MAX))
    {
        // there is no room for anything more than a small multiple of the smallest
        // size. new requests will be rejected. Only resize requests will be permitted.
        if((*masks[1] == UINT64_MAX) && (*masks[2] == UINT64_MAX))
        {
            limit = 1 << (h->container_exponent - 18);
        }
        else if(*masks[2] == UINT64_MAX)
        {
            limit = 1 << (h->container_exponent - 12);
        }
        else
        {
            limit = 1 << (h->container_exponent - 6);
        }
    }
    
    if(size > limit)
    {
        return false;
    }
    return true;
}


void* heap_get_block(Heap*h, size_t size)
{
    const uint64_t level_exponents[] = {
        (h->container_exponent - 18),
        (h->container_exponent - 12),
        (h->container_exponent - 6),
        (h->container_exponent)};
    
    const uint64_t level_sizes[] = {
        (1 << level_exponents[0]),
        (1 << level_exponents[1]),
        (1 << level_exponents[2]),
        (1 << level_exponents[3])
    };
    
    uintptr_t base = ((uintptr_t)h & ~(os_page_size - 1));
    uintptr_t mask_offset = ((uintptr_t)h + sizeof(Heap) + (CACHE_LINE - 1)) & ~(CACHE_LINE - 1);
    uintptr_t data_offset = mask_offset + CACHE_LINE*3;
    uintptr_t end_offset = (data_offset + (level_sizes[3] - 1)) & ~(level_sizes[3] - 1);
    uint64_t* masks[] = {
        (uint64_t*)mask_offset,
        (uint64_t*)(mask_offset + CACHE_LINE),
        (uint64_t*)(mask_offset + CACHE_LINE*2)};
        
    if((*masks[0] == UINT64_MAX) && (*masks[1] == UINT64_MAX) && (*masks[2] == UINT64_MAX))
    {
        return NULL;
    }
    
    size_t limit = end_offset - data_offset;
    if((*masks[1] == UINT64_MAX) || (*masks[2] == UINT64_MAX))
    {
        // there is no room for anything more than a small multiple of the smallest
        // size. new requests will be rejected. Only resize requests will be permitted.
        if((*masks[1] == UINT64_MAX) && (*masks[2] == UINT64_MAX))
        {
            limit = level_sizes[0];
        }
        else if(*masks[2] == UINT64_MAX)
        {
            limit = level_sizes[1];
        }
        else
        {
            limit = level_sizes[2];
        }
    }
    
    if(size > limit)
    {
        return NULL;
    }
    
    int32_t level_idx = 2;
    int32_t midx = 0;
    uint64_t invmask = 0;
    if(size < level_sizes[1])
    {
        level_idx = 0;
        invmask = ~*(masks[level_idx] + sizeof(uint64_t));
        midx = get_next_mask_idx(invmask, 0);
    }
    else if(size < level_sizes[2])
    {
        level_idx = 1;
        invmask = ~*(masks[level_idx] + sizeof(uint64_t));
        midx = get_next_mask_idx(invmask, 0);
    }
    
    
    size_t range = size >> level_exponents[level_idx];
    range += (size & (level_sizes[level_idx] - 1)) ? 1 : 0;
    uint32_t pidx = midx;
    if(midx == 0)
    {
        // test to see if the structs have been initilized.
        // just look at the base masks..
        uint64_t *base_mask = masks[level_idx] + CACHE_LINE*level_idx;
        // special case for zero... because our header data is there
        int32_t idx = find_first_nzeros(*base_mask, range);
        if(idx != -1)
        {
            // found memory
            uintptr_t mask = reserve_range_idx(range, idx);
            *base_mask = *base_mask | mask;
            idx = 63 - idx;
            if(*base_mask == UINT64_MAX)
            {

                
                for(int32_t i = 0; i < level_idx - 1; i++)
                {
                    
                }
                h->filter_l[0] |= (1UL << (63 - pidx));
                h->filter_l[1] |= (1UL << (63 - pidx));
                invmask = ~h->filter_l[level_idx];
            }
            if(range > 1)
            {
                new_heap_set_range_for_index((uintptr_t)base_mask, range, idx);
            }
            h->num_allocations++;
            return (void*)(base + (idx*level_sizes[level_idx]));
        }
        if(level_idx == 2)
        {
            return NULL;
        }
        midx = get_next_mask_idx(invmask, midx+1);
    }
    if(midx == -1)
    {
        return NULL;
    }
    //
    // if there is a limit.
    // 64k limit. that measn that all 64k contiguous parts are full.
    //  we don't know if we have
    if(level_idx == 1)
    {
        // find free section.
        do
        {
            uintptr_t base_addr = (base + (midx*level_sizes[level_idx])) + CACHE_LINE*level_idx;
            int32_t idx = find_first_nzeros(*(uint64_t*)base_addr, range);
            if(idx != -1)
            {
                // found memory
                uintptr_t mask = reserve_range_idx(range, idx);
                *(uint64_t*)base_addr |= mask;
                idx = 63 - idx;
                for(int32_t i = 0; i < level_idx - 1; i++)
                {
                    h->filter_l[i] |= mask;
                }
                if(range > 1)
                {
                    new_heap_set_range_for_index(base_addr, range, idx);
                }
                h->num_allocations++;
                return (void*)(base_addr + (idx*level_sizes[level_idx]));
            }
        
            midx = get_next_mask_idx(invmask, midx+1);
        }while(midx != -1);

        return NULL;
    }
    else
    {
        //
        // we only know that there is 16 bytes to be found in the 64k area.
        // if there is 1k to be found in the 64k area.
        //
        // lets look at that mask first.
        // find free section.
        do
        {
            uintptr_t base_addr = (base + (midx*level_sizes[level_idx+1])) + CACHE_LINE*level_idx;
            int32_t idx = find_first_nzeros(*(uint64_t*)base_addr, range);
            if(idx != -1)
            {
                // found memory
                uintptr_t mask = reserve_range_idx(range, idx);
                *(uint64_t*)base_addr |= mask;
                idx = 63 - idx;
                for(int32_t i = 0; i < level_idx - 1; i++)
                {
                    h->filter_l[i] |= mask;
                }
                if(*(uint64_t*)base_addr == UINT64_MAX)
                {
                    h->filter_l[0] |= (1UL << (63 - pidx));
                    h->filter_l[1] |= (1UL << (63 - pidx));
                    invmask = ~h->filter_l[level_idx];
                }
                if(range > 1)
                {
                    new_heap_set_range_for_index(base_addr, range, idx);
                }
                h->num_allocations++;
                return (void*)(base_addr + (idx*level_sizes[level_idx]));
            }
        
            midx = get_next_mask_idx(invmask, midx+1);
        }while(midx != -1);

        return NULL;
    }
}

void heap_free(Heap*h, void*p, bool dummy)
{
    const uint64_t level_exponents[] = {
        (h->container_exponent - 18),
        (h->container_exponent - 12),
        (h->container_exponent - 6),
        (h->container_exponent)};
    
    const uint64_t level_sizes[] = {
        (1 << level_exponents[0]),
        (1 << level_exponents[1]),
        (1 << level_exponents[2]),
        (1 << level_exponents[3])};
    
    uintptr_t pmask = ((uintptr_t)p & ~(level_sizes[2] - 1));
    ptrdiff_t pdiff = (uint8_t *)p - (uint8_t *)pmask;
    uint32_t pidx = (uint32_t)((size_t)pdiff >> level_exponents[2]);
    
    for(int i = 0; i < 3; i++)
    {
        if(((uintptr_t)p & (level_sizes[i] - 1)) == 0)
        {
            // aligned to base level
            uintptr_t mask = ((uintptr_t)p & ~(level_sizes[i + 1] - 1));
            const ptrdiff_t diff = (uint8_t *)p - (uint8_t *)mask;
            const uint32_t idx = 63 - (uint32_t)((size_t)diff >> level_exponents[i]);
            if(mask < (uintptr_t)h)
            {
                // we at the start
                uintptr_t mask_offset = ((uintptr_t)h + sizeof(Heap) + (CACHE_LINE - 1)) & ~(CACHE_LINE - 1);
                mask = mask_offset + i*CACHE_LINE;
            }
            else
            {
                // if size is not the lowest size...
                if(i != 0)
                {
                    //octosq
                    uintptr_t base = ((uintptr_t)h & ~(os_page_size - 1));
                    const ptrdiff_t basediff = (uint8_t *)p - (uint8_t *)base;
                    const uint32_t bidx = (uint32_t)((size_t)basediff >> level_exponents[2]);
                    h->init_filter ^= (1UL << bidx);
                }
            }
            uint32_t range = new_heap_reset_range_for_index(mask, idx);
            uintptr_t sub_mask = ~reserve_range_idx(range||1, idx);
            *(uint64_t*)mask = *(uint64_t*)mask & sub_mask;
            if(i < 2)
            {
                h->filter_l[i] &= ~(1UL << (63 - pidx));
            }
            h->num_allocations--;
            break;
        }
    }
}
static size_t heap_get_block_size(Heap *h, void *p)
{
    const uint64_t level_exponents[] = {
        (h->container_exponent),
        (h->container_exponent - 6),
        (h->container_exponent - 12),
        (h->container_exponent - 18)};
    
    const uint64_t level_sizes[] = {
        (1 << level_exponents[0]),
        (1 << level_exponents[1]),
        (1 << level_exponents[2]),
        (1 << level_exponents[3])};
    
    for(int i = 1; i < 4; i++)
    {
        if(((uintptr_t)p & (level_sizes[i] - 1)) == 0)
        {
            // aligned to base level
            uintptr_t mask = ((uintptr_t)p & ~(level_sizes[i - 1] - 1));
            const ptrdiff_t diff = (uint8_t *)p - (uint8_t *)mask;
            const uint32_t idx = (uint32_t)((size_t)diff >> level_exponents[i]);
            if(mask < (uintptr_t)h)
            {
                // we at the start
                uintptr_t mask_offset = ((uintptr_t)h + sizeof(Heap) + (CACHE_LINE - 1)) & ~(CACHE_LINE - 1);
                mask = mask_offset + (i - 1)*CACHE_LINE;
            }
            
            uint32_t range = new_heap_get_range_for_index(mask, idx);
            return level_sizes[i]*range;
        }
    }
    return 0;
}
/*
    - sizelimits (4m / 64 == 64k)
                base_size = area_size/64
                mid_size = base_size/64
                low_size = mid_size/64
 [64| 4k | 256k]
    find free (size )
        - >= 64k gets 64k piece
          <  64k get 1k pieces.
 -
 
 [Area|Section 48 bytes ] 4M
 64 bytes
 64 * 64 bytes 4k.
 4k * 64 = 256k
 1/16th is wasted on structures. 6.25%

 8 byte mask. per level.
 56 byte range fild.
 // 64 bytes per range.

 4M heaps.
    4m =    64 * 64k
    64k =   64 * 1k
    1k =    64 * 16bytes
 32M heaps.
    32m =   64 * 512k
    512k =  64 * 8k
    8k =    64 * 128bytes
 64M heaps.
    64m =   64 * 1m
    1m =    64 * 16k
    16k =   64 * 256bytes
 128M heaps.
    128m =  64 * 2m
    2m =    64 * 32k
    32k =   64 * 512bytes
 256M heaps.
    256m =  64 * 4m
    4m =    64 * 64k
    64k =   64 * 1k

 find free.
    - look at top level mask.
    - will be aligned to size value. >= 64 k, aligned to 64 k.
    - look at address alignment. if aligned to 64k, 1k, 16 bytes... which level to look at.
    -
 */
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
bool heap_is_connected(const Heap *h) { return h->prev != NULL || h->next != NULL; }
/*
static inline uint32_t heap_block_get_header(HeapBlock *hb) { return *(uint32_t *)((uint8_t *)&hb->data - WSIZE); }

static inline void heap_block_set_header(HeapBlock *hb, const uint32_t s, const uint32_t v, const uint32_t pa)
{
    *(uint32_t *)((uint8_t *)&hb->data - WSIZE) = (s | v | pa << 1);
}

static inline void heap_block_set_footer(HeapBlock *hb, const uint32_t s, const uint32_t v)
{
    const uint32_t size = (*(uint32_t *)((uint8_t *)&hb->data - WSIZE)) & ~0x7;
    *(uint32_t *)((uint8_t *)(&hb->data) + (size)-DSIZE) = (s | v);
}

static inline HeapBlock *heap_block_next(HeapBlock *hb)
{
    const uint32_t size = *(uint32_t *)((uint8_t *)&hb->data - WSIZE) & ~0x7;
    return (HeapBlock *)((uint8_t *)&hb->data + (size));
}

static inline HeapBlock *heap_block_prev(HeapBlock *hb)
{
    const uint32_t size = *(uint32_t *)((uint8_t *)&hb->data - DSIZE) & ~0x7;
    return (HeapBlock *)((uint8_t *)&hb->data - (size));
}

bool heap_is_connected(const Heap *h) { return h->prev != NULL || h->next != NULL; }
bool heap_has_room(const Heap *h, const size_t s)
{
    if ((h->used_memory + s + HEADER_FOOTER_OVERHEAD) > h->total_memory) {
        return false;
    }
    if (s <= h->max_block && s >= h->min_block) {
        return true;
    }
    return false;
}

static inline void heap_place(Heap *h, void *bp, const uint32_t asize, const int32_t header, const int32_t csize)
{
    HeapBlock *hb = (HeapBlock *)bp;
    const uint32_t prev_alloc = (header & 0x3) >> 1;
    if ((csize - asize) >= (DSIZE + HEADER_FOOTER_OVERHEAD)) {
        heap_block_set_header(hb, asize, 1, prev_alloc);
        hb = heap_block_next(hb);
        heap_block_set_header(hb, csize - asize, 0, 1);
        heap_block_set_footer(hb, csize - asize, 0);
        list_enqueue(&h->free_nodes, (QNode *)hb);
    } else {
        heap_block_set_header(hb, csize, 1, prev_alloc);
    }
}

void *heap_find_fit(Heap *h, const uint32_t asize)
{
    // find the first fit.
    QNode *current = (QNode *)h->free_nodes.head;
    while (current != NULL) {
        HeapBlock *hb = (HeapBlock *)current;
        int header = heap_block_get_header(hb);
        uint32_t bsize = header & ~0x7;
        if (asize <= bsize) {
            list_remove(&h->free_nodes, current);
            heap_place(h, current, asize, header, bsize);
            return current;
        }
        current = (QNode *)current->next;
    }
    return NULL;
}

static size_t heap_get_block_size(Heap *h, void *bp)
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

void *heap_get_block(Heap *h, uint32_t s)
{
    if (h->num_allocations++ == 0) {
        // on first allocation we write our footer at the end.
        // we delay this just so that we do not touch the pages till needed
        uint8_t *blocks = (uint8_t *)h + sizeof(Heap);
        HeapBlock *hb = (HeapBlock *)(blocks + DSIZE * 2);
        heap_block_set_footer(hb, h->total_memory, 0);
        heap_block_set_header(heap_block_next(hb), 0, 1, 0);
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
    s = heap_get_good_size(s);
    void *ptr = heap_find_fit(h, s);
    h->used_memory += s;
    h->max_block -= s;
    return ptr;
}

static inline bool resize_block(Heap *h, void *bp, int32_t size)
{
    //
    HeapBlock *hb = (HeapBlock *)bp;
    int header = heap_block_get_header(hb);
    int32_t bsize = header & ~0x7;

    //
    HeapBlock *next_block = heap_block_next(hb);
    int next_header = heap_block_get_header(next_block);
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
        heap_block_set_header(hb, size, 0, prev_alloc);
        heap_block_set_footer(hb, size, 0);
        list_remove(&h->free_nodes, (QNode *)next_block);
        return true;
    }
    //
    return false;
}

static inline void heap_update_max(Heap *h, int32_t size)
{
    if (size > h->max_block) {
        h->max_block = size;
    }
}

void *heap_coalesce(Heap *h, void *bp)
{
    HeapBlock *hb = (HeapBlock *)bp;
    int header = heap_block_get_header(hb);
    int32_t size = header & ~0x7;
    const uint32_t prev_alloc = (header & 0x3) >> 1;
    HeapBlock *next_block = heap_block_next(hb);
    int next_header = heap_block_get_header(next_block);
    const size_t next_alloc = next_header & 0x1;

    QNode *hn = (QNode *)bp;
    if (!(prev_alloc && next_alloc)) {

        const size_t next_size = next_header & ~0x7;

        // next is free
        if (prev_alloc && !next_alloc) {
            size += next_size;
            QNode *h_next = (QNode *)next_block;
            list_remove(&h->free_nodes, h_next);
            heap_block_set_header(hb, size, 0, 1);
            heap_block_set_footer(hb, size, 0);
            list_enqueue(&h->free_nodes, hn);
        } // prev is fre
        else {
            HeapBlock *prev_block = heap_block_prev(hb);
            int prev_header = heap_block_get_header(prev_block);
            const size_t prev_size = prev_header & ~0x7;
            const uint32_t pprev_alloc = (prev_header & 0x3) >> 1;
            if (!prev_alloc && next_alloc) {
                size += prev_size;
                heap_block_set_footer(hb, size, 0);
                heap_block_set_header(prev_block, size, 0, pprev_alloc);
                bp = (void *)heap_block_prev(hb);
            } else { // both next and prev are free
                size += prev_size + next_size;
                QNode *h_next = (QNode *)next_block;
                list_remove(&h->free_nodes, h_next);
                heap_block_set_header(prev_block, size, 0, pprev_alloc);
                heap_block_set_footer(next_block, size, 0);
                bp = (void *)heap_block_prev(hb);
            }
        }
    } else {
        list_enqueue(&h->free_nodes, hn);
    }
    heap_update_max(h, size);

    return bp;
}

void heap_reset(Heap *h)
{
    uint8_t *blocks = (uint8_t *)h + sizeof(Heap);
    h->free_nodes.head = NULL;
    h->free_nodes.tail = NULL;
    HeapBlock *hb = (HeapBlock *)(blocks + DSIZE * 2);
    list_enqueue(&h->free_nodes, (QNode *)hb);
    heap_block_set_header(hb, h->total_memory, 0, 1);

    h->max_block = h->total_memory;
}

static void heap_free(Heap *h, void *bp, bool should_coalesce)
{
    if (bp == 0)
        return;

    HeapBlock *hb = (HeapBlock *)bp;
    int header = heap_block_get_header(hb);
    const uint32_t size = header & ~0x7;
    const uint32_t prev_alloc = (header & 0x3) >> 1;
    heap_block_set_header(hb, size, 0, prev_alloc);
    heap_block_set_footer(hb, size, 0);

    if (should_coalesce) {
        heap_coalesce(h, bp);
    } else {
        list_enqueue(&h->free_nodes, (QNode *)bp);
        heap_update_max(h, size);
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
        heap_reset(h);
    }
}

void heap_extend(Heap *h)
{
    uint32_t *blocks = (uint32_t *)((uint8_t *)h + sizeof(Heap));
    blocks[0] = 0;
    blocks[1] = DSIZE | 1;
    blocks[2] = DSIZE | 1;
    blocks[3] = 1;

    heap_reset(h);
}

static void heap_init(Heap *h, int8_t pidx, const size_t psize)
{
    void *blocks = (uint8_t *)h + sizeof(Heap);
    const uintptr_t section_end = ((uintptr_t)h + (psize - 1)) & ~(psize - 1);
    const size_t remaining_size = section_end - (uintptr_t)blocks;

    const size_t block_memory = psize - sizeof(Heap) - sizeof(Section);
    const size_t header_footer_offset = sizeof(uintptr_t) * 2;
    h->idx = pidx;
    h->used_memory = 0;
    h->total_memory = (uint32_t)((MIN(remaining_size, block_memory)) - header_footer_offset - HEADER_FOOTER_OVERHEAD);
    h->max_block = h->total_memory;
    h->min_block = sizeof(uint32_t);
    h->num_allocations = 0;
    h->next = NULL;
    h->prev = NULL;
    heap_extend(h);
}
*/
#endif /* heap_h */
