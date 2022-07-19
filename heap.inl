
#ifndef heap_h
#define heap_h
#include "section.inl"
#include <stdio.h>
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
#define HEAP_BASE_SIZE_EXPONENT 22

/*
    block. Memory
    slab. collection of blocks.
    
 
    top - [a, s, links][a, s, links][a, s, zero] [filter1, filter2]
    mid - [a, s, links][a, s, zero][p, n, scale]
    bot - [a, s][ p, n, scale]
 
    find a slab.
        - if request size is the same as previously.
            - if we have a cached slab. Allocate from slab if we can.
        - how big memory are you asking for.
            - at what level. l1, l2, l3.
            - the range count.
            - cache previous size.
    
    if size > 64th of heap.
        - allocate from the top.
    else
        - find mid slab with free space.
            - allocate from the top. 64k.
 
 
    find a free slab [ ]
    get a block from slab [ ]
        - if fail. goto find.
 
    if slab is full, update parent slab.
        heap
            - slab
                - parent
    current_slab
        -next free idx
        -
 */

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
 //
 
 // L2 -
 // reserve L2 (range)
 // tag filter of L1 at L2 root ( range )
 // reserve from L3 at L3 root. ( 1 bit )
 // if mask becomes full. -> reserve at root. l1 l2.
 
 // L1
 // reserve L1 ( range )
 // reserve L2 at L2 root ( 1 bit )
 // reserve L3 at L3 root ( 1 bit )
 // if masks becomes full -> reserve at l2 root.
 // if l2 root becomes full. -> reserve at l3 root.

 // free -----
 // L3
 // free L3 (range)
 // zero_init_mark L3 (range) (L1 and L2 allocations will need to zero initialize)
 // tag filter of L2 and L1 at L3 root. ( range )
 //

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
typedef enum HeapLevels_t
{
    LEVEL_0 = 0,    //LEVEL_1/64
    LEVEL_1,        // LEVEL_2/64
    LEVEL_2,        // LEVEL_3/64
    LEVEL_3         // size of heap
} HeapLevels;

const uint32_t new_heap_container_overhead = HEAP_PARTS + HEAP_PARTS*HEAP_PARTS + HEAP_PARTS*HEAP_PARTS*HEAP_PARTS;
const uint32_t new_heap_level_size[] = {0, HEAP_PARTS, HEAP_PARTS*HEAP_PARTS};
const uint32_t new_heap_level_offset[] = { 0, HEAP_PARTS, HEAP_PARTS+HEAP_PARTS*HEAP_PARTS};
const uint32_t heap_level_offset = HEAP_BASE_SIZE_EXPONENT;
const uint64_t heap_empty_mask = 1UL << 63;
const uint64_t heap_empty_mask_z = 15UL << 60;
const uint64_t heap_mask_size = sizeof(uint64_t)*3;
const uint64_t heap_filter_size = sizeof(uint64_t)*2;

// 4, 8, 16, 32, 64, 128, 256
// 22, 23, 24, 25, 26, 27, 28
// heap_idx exponent - 22
typedef struct heap_size_table_t
{
    uint64_t exponents[4];
    uint64_t sizes[4];
} heap_size_table;

const heap_size_table heap_tables[7] = {
    {{4, 10, 16, 22},{1 << 4, 1 << 10, 1 << 16, 1 << 22}},
    {{5, 11, 17, 23},{1 << 5, 1 << 11, 1 << 17, 1 << 23}},
    {{6, 12, 18, 24},{1 << 6, 1 << 12, 1 << 18, 1 << 24}},
    {{7, 13, 19, 25},{1 << 7, 1 << 13, 1 << 19, 1 << 25}},
    {{8, 14, 20, 26},{1 << 8, 1 << 14, 1 << 20, 1 << 26}},
    {{9, 15, 21, 27},{1 << 9, 1 << 15, 1 << 21, 1 << 27}},
    {{10, 16, 22, 28},{1 << 10, 1 << 16, 1 << 22, 1 << 28}}};

static inline const heap_size_table* get_size_table(uint32_t exponent)
{
    return &heap_tables[exponent - heap_level_offset];
}

void printBits(size_t const size, void const * const ptr)
{
    unsigned char *b = (unsigned char*) ptr;
    unsigned char byte;
    ssize_t i, j;
    
    for (i = size-1; i >= 0; i--) {
        for (j = 7; j >= 0; j--) {
            byte = (b[i] >> j) & 1;
            printf("%u", byte);
        }
    }
    printf("]:[%16llX]\n",*(uint64_t*)ptr);
}

static inline void print_header(Heap*h, uintptr_t ptr)
{
    uintptr_t root_masks = ((uintptr_t)h + sizeof(Heap) + (CACHE_LINE - 1)) & ~(CACHE_LINE - 1);
    uintptr_t base = ((uintptr_t)h & ~(os_page_size - 1));
    ptrdiff_t rdiff = (uint8_t *)ptr - (uint8_t *)base;
    printf("\nL3!\n");
    printf("L1 Allocated:[0b");printBits(sizeof(uint64_t), (uint64_t*)root_masks);
    printf("L1 Available:[0b");printBits(sizeof(uint64_t), (uint64_t*)(root_masks + sizeof(uint64_t)));
    printf("L1 Ranges   :[0b");printBits(sizeof(uint64_t), (uint64_t*)(root_masks + sizeof(uint64_t)*2));
    printf("L2 Allocated:[0b");printBits(sizeof(uint64_t), (uint64_t*)(root_masks + sizeof(uint64_t)*3));
    printf("L2 Available:[0b");printBits(sizeof(uint64_t), (uint64_t*)(root_masks + sizeof(uint64_t)*4));
    printf("L2 Ranges   :[0b");printBits(sizeof(uint64_t), (uint64_t*)(root_masks + sizeof(uint64_t)*5));
    printf("L3 Allocated:[0b");printBits(sizeof(uint64_t), (uint64_t*)(root_masks + sizeof(uint64_t)*6));
    printf("L3 ZERO     :[0b");printBits(sizeof(uint64_t), (uint64_t*)(root_masks + sizeof(uint64_t)*7));
    printf("L1 Filter   :[0b");printBits(sizeof(uint64_t), (uint64_t*)(root_masks + sizeof(uint64_t)*8));
    printf("L2 Filter   :[0b");printBits(sizeof(uint64_t), (uint64_t*)(root_masks + sizeof(uint64_t)*9));
    
    uintptr_t mask = ((uintptr_t)ptr & ~((1 << (h->container_exponent - 6)) - 1));
    const ptrdiff_t diff = (uint8_t *)ptr - (uint8_t *)mask;
    const uint32_t idx = (uint32_t)((size_t)diff >> (h->container_exponent - 12));
    if(idx != 0)
    {
        mask = base + idx*(1UL << (h->container_exponent - 12));
        printf("L2!\n");
        printf("L1 Allocated:[0b");printBits(sizeof(uint64_t), (uint64_t*)mask);
        printf("L1 Available:[0b");printBits(sizeof(uint64_t), (uint64_t*)(mask + sizeof(uint64_t)));
        printf("L1 Range    :[0b");printBits(sizeof(uint64_t), (uint64_t*)(mask + sizeof(uint64_t)*2));
        printf("L2 Allocated:[0b");printBits(sizeof(uint64_t), (uint64_t*)(mask + sizeof(uint64_t)*3));
        printf("L2 ZERO:     [0b");printBits(sizeof(uint64_t), (uint64_t*)(mask + sizeof(uint64_t)*4));
        mask = ((uintptr_t)ptr & ~((1 << (h->container_exponent - 12)) - 1));
        const ptrdiff_t diff = (uint8_t *)ptr - (uint8_t *)mask;
        const uint32_t idx = (uint32_t)((size_t)diff >> (h->container_exponent - 18));
        if(idx != 0)
        {
            printf("L1!\n");
            printf("L1 Allocated:[0b");printBits(sizeof(uint64_t), (uint64_t*)mask);
            printf("L1 Ranges   :[0b");printBits(sizeof(uint64_t), (uint64_t*)mask + sizeof(uint64_t));
        }
    }
}
uint64_t apply_range(uint32_t range, uint32_t at)
{
    // range == 1 -> nop
    // set bit at (at)
    // set bit at at + (range - 1).
    //
    // 111110011
    return (1UL << at ) | (1UL << (at - (range - 1)));
}

uint32_t get_range(uint32_t at, uint64_t mask)
{
    //  at == 7
    //  00001000111
    //  00000111111 &
    //  00000000111 tz8 - at == 3
    //  lz8 - at == 3
    //  3 + 2 == 5
    // zero all bits from at to the highest bit.
    uint64_t top_mask = ((1UL << at) - 1);
    return at - __builtin_ctzll(mask & top_mask) + 1;
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
    return ((1UL << range) - 1UL) << (idx - (range - 1));
}


void heap_init_zero(uintptr_t baseptr)
{
    *(uint64_t*)baseptr  = 1UL << 63;
    *(uint64_t*)(baseptr + sizeof(uint64_t)) = 0;
    *(uint64_t*)(baseptr + sizeof(uint64_t)*2) = 0;
}

int32_t heap_init_head_range(Heap* h, uintptr_t mask_offset, size_t size)
{
    const heap_size_table* stable = get_size_table(h->container_exponent);
    // setup initial masks.
    uintptr_t header_size = size;
    uintptr_t range = header_size >> stable->exponents[0];
    uintptr_t rem = ( header_size & ((stable->sizes[0]) - 1));
    range += (rem) ? 1 : 0;
    int32_t idx = (int32_t)(64 - range);
    *(uint64_t*)mask_offset = reserve_range_idx(range, 63);
    *(uint64_t*)(mask_offset + sizeof(uint64_t)) = 0;
    *(uint64_t*)(mask_offset + sizeof(uint64_t)*2) = 0;
    return idx - 1;
}

Heap* heap_init(uintptr_t base_addr, int32_t idx, size_t heap_size_exponent)
{
    
    Heap* h = (Heap*)base_addr;
    h->idx = idx;
    h->container_exponent = (uint32_t)heap_size_exponent;
    uintptr_t mask_offset = (base_addr + sizeof(Heap) + (CACHE_LINE - 1)) & ~(CACHE_LINE - 1);
    uintptr_t base = ((uintptr_t)h & ~(os_page_size - 1));
    //
    h->num_allocations = 0;
    h->previous_l1_offset = 0;
    // high allocations
    heap_init_head_range(h, mask_offset, (mask_offset + sizeof(uint64_t)*10) - base);
    *(uint64_t*)(mask_offset + sizeof(uint64_t)) = 0;
    *(uint64_t*)(mask_offset + sizeof(uint64_t)*2) = 0;
    mask_offset += sizeof(uint64_t)*3;
    
    *(uint64_t*)mask_offset = 1UL << 63;
    *(uint64_t*)(mask_offset + sizeof(uint64_t)) = 0;
    *(uint64_t*)(mask_offset + sizeof(uint64_t)*2) = 0;
    mask_offset += sizeof(uint64_t)*3;
    
    *(uint64_t*)mask_offset = 1UL << 63;
    *(uint64_t*)(mask_offset + sizeof(uint64_t)) = 1UL << 63;
    // filters
    *(uint64_t*)(mask_offset + sizeof(uint64_t)*2) = 0;
    *(uint64_t*)(mask_offset + sizeof(uint64_t)*3) = 0;

    
    if (h->container_exponent == 22) {
        Section *section = (Section *)((uintptr_t)h & ~(SECTION_SIZE - 1));
        section_reserve_all(section);
    } else {
        size_t area_size = area_size_from_addr((uintptr_t)h);
        Area *area = (Area *)((uintptr_t)h & ~(area_size - 1));
        area_reserve_all(area);
    }
    print_header(h, (uintptr_t)h);
    return h;
}


bool heap_has_room(Heap*h, size_t size)
{
    const heap_size_table* stable = get_size_table(h->container_exponent);
    uintptr_t level_size = stable->sizes[2];
    uintptr_t mask_offset = ((uintptr_t)h + sizeof(Heap) + (CACHE_LINE - 1)) & ~(CACHE_LINE - 1);
    uintptr_t data_offset = mask_offset + sizeof(uint64_t)*10;
    uintptr_t end_offset = (data_offset + (level_size - 1)) & ~(level_size - 1);
    uint64_t* masks[] = {
        (uint64_t*)mask_offset,
        (uint64_t*)(mask_offset + sizeof(uint64_t)*3),
        (uint64_t*)(mask_offset + sizeof(uint64_t)*6)};
        
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

void* heap_get_block_L3(Heap*h, uintptr_t base, size_t range, uint64_t** masks)
{
    const heap_size_table* stable = get_size_table(h->container_exponent);
    // test to see if the structs have been initilized.
    // just look at the base masks..
    uint64_t *base_mask = masks[2];
    // special case for zero... because our header data is there
    int32_t idx = find_first_nzeros(*base_mask, range);
    uint64_t *filters = (uint64_t*)((uint8_t*)masks[0] + CACHE_LINE);
    if(idx != -1)
    {
        // found memory
        uintptr_t mask = reserve_range_idx(range, idx);
        *base_mask = *base_mask | mask;
        filters[0] = filters[0] | mask;
        filters[1] = filters[1] | mask;
        idx = 63 - idx;
        if(range > 1)
        {
            *(base_mask+1) |= apply_range((uint32_t)range, idx);
        }
        h->num_allocations++;
        return (void*)(base + (idx*stable->sizes[2]));
    }
    return NULL;
}

void* heap_get_block_L2(Heap*h, uintptr_t base, size_t range, uint64_t** masks)
{
    uint64_t *filters = (uint64_t*)((uint8_t*)masks[0] + CACHE_LINE);
    uint64_t invmask = ~filters[1];
    uint32_t midx = get_next_mask_idx(invmask, 0);
    const heap_size_table* stable = get_size_table(h->container_exponent);
    // test to see if the structs have been initilized.
    // just look at the base masks..
    if(midx == 0)
    {
        // zero pos is always zero initialized
        uint64_t *base_mask = masks[1];
        // special case for zero... because our header data is there
        int32_t idx = find_first_nzeros(*base_mask, range);
        if(idx != -1)
        {
            // found memory
            
            uintptr_t mask = reserve_range_idx(range, idx);
            *base_mask = *base_mask | mask;
            idx = 63 - idx;
            if(range > 1)
            {
                *(base_mask+1) |= apply_range((uint32_t)range, idx);
            }
            h->num_allocations++;
            return (void*)(base + (idx*stable->sizes[1]));
        }
    }
    
    uint64_t *zmask = (uint64_t*)((uint8_t*)masks[2] + sizeof(uint64_t));
    uint64_t *l3_reserve = masks[2];
    while((midx = get_next_mask_idx(invmask, midx+1)) != -1)
    {
        uintptr_t base_addr = (base + (midx*stable->sizes[2]));
        int32_t idx = 62;
        if(((1UL << (63 - midx)) & *zmask) == 0)
        {
            // if it is empty... we can safely just get the first available.
            heap_init_zero(base_addr);
            heap_init_zero(base_addr+sizeof(int64_t)*3);
            *zmask = *zmask | (1UL << (63 - midx));
            base_addr += sizeof(int64_t)*3;
        }
        else
        {
            base_addr += sizeof(int64_t)*3;
            idx = find_first_nzeros(*(uint64_t*)base_addr, range);
        }
        
        if(idx != -1)
        {
            uintptr_t mask = reserve_range_idx(range, idx);
            *(uint64_t*)base_addr |= mask;
            idx = 63 - idx;
            if(range > 1)
            {
                *(uint64_t*)(base_addr+sizeof(uint64_t)) |= apply_range((uint32_t)range, idx);
            }
            h->num_allocations++;
            *l3_reserve = *l3_reserve | (1UL << (63 -  midx));
            if(*(uint64_t*)base_addr == UINT64_MAX)
            {
                uint64_t* l1_filter = (uint64_t*)((uint8_t*)masks[0] + sizeof(uint64_t));
                *l1_filter = *l1_filter | (1UL << (63 -  midx));
            }
            return (void*)(base_addr + (idx*stable->sizes[1]));
        }
    }

    return NULL;
}

uint64_t* heap_get_mask(Heap*h, HeapOffset* hoffset, HeapLevels level)
{
    uintptr_t offset = 0;
    switch(level)
    {
        case LEVEL_0:
        {
            if((hoffset->l3 == 0) && (hoffset->l2 == 0))
            {
                offset = ((uintptr_t)h + sizeof(Heap) + (CACHE_LINE - 1)) & ~(CACHE_LINE - 1);
            }
            else
            {
                const heap_size_table* stable = get_size_table(h->container_exponent);
                uintptr_t base = ((uintptr_t)h & ~(os_page_size - 1));
                offset = base + hoffset->l3*stable->sizes[2] + hoffset->l2*stable->sizes[1];
            }
            break;
        }

        case LEVEL_1:
        {
            if(hoffset->l3 == 0)
            {
                offset = ((uintptr_t)h + sizeof(Heap) + (CACHE_LINE - 1)) & ~(CACHE_LINE - 1);
                offset =  offset + sizeof(uint64_t)*3;
            }
            else
            {
                const heap_size_table* stable = get_size_table(h->container_exponent);
                uintptr_t base = ((uintptr_t)h & ~(os_page_size - 1));
                offset = base + hoffset->l3*stable->sizes[2];
            }
            break;
        }
        case LEVEL_2:
        {
            offset = ((uintptr_t)h + sizeof(Heap) + (CACHE_LINE - 1)) & ~(CACHE_LINE - 1);
            offset =  offset + sizeof(uint64_t)*6;
            break;
        }
        default:
            return NULL;
    }
    return (uint64_t*)offset;
    
}

void* heap_get_block_L1(Heap*h, uintptr_t base, size_t range, uint64_t** masks)
{
    uint64_t *filters = (uint64_t*)((uint8_t*)masks[0] + CACHE_LINE);
    const heap_size_table* stable = get_size_table(h->container_exponent);
    
    if(h->previous_l1_offset != -1)
    {
        uint64_t* offset = (uint64_t*)((uintptr_t)h + (uintptr_t)h->previous_l1_offset);
        int32_t idx = find_first_nzeros(*offset, range);
        if(idx != -1)
        {
            uintptr_t mask = reserve_range_idx(range, idx);
            *offset |= mask;
            idx = 63 - idx;
            if(range > 1)
            {
                *(uint64_t*)(offset+sizeof(uint64_t)) |= apply_range((uint32_t)range, idx);
            }
            h->num_allocations++;
            if(*offset == UINT64_MAX)
            {
                uintptr_t mask = ((uintptr_t)offset & ~(stable->sizes[2] - 1));
                if(mask < (uintptr_t)h)
                {
                    mask = (uintptr_t)masks[0];
                }
                const ptrdiff_t diff = (uint8_t *)offset - (uint8_t *)mask;
                const uint32_t pidx = (uint32_t)((size_t)diff >> stable->exponents[1]);
                uint64_t* cmask = (uint64_t*)(mask + sizeof(uint64_t));
                
                *cmask = *cmask | (1UL << (63 -  pidx));
                if(*(uint64_t*)cmask == UINT64_MAX)
                {
                    cmask = filters + 1;
                    *cmask = *cmask | (1UL << (63 -  pidx));
                }
            }
            if(offset == masks[0])
            {
                return (void*)(base + (idx*stable->sizes[0]));
            }
            else
            {
                return (void*)(offset + (idx*stable->sizes[0]));
            }
        }
        h->previous_l1_offset = -1;
    }
    
    uint64_t invmask = ~filters[0];
    uint32_t midx = -1;
    
    
    uint64_t *zmask = (uint64_t*)((uint8_t*)masks[2] + sizeof(uint64_t));
    uint64_t *l3_reserve = masks[2];
    while((midx = get_next_mask_idx(invmask, midx+1)) != -1)
    {
        
        uintptr_t base_addr = (base + (midx*stable->sizes[2]));
        if(midx == 0)
        {
            base_addr = (uintptr_t)masks[0];
        }
        if(((1UL << (63 - midx)) & *zmask) == 0)
        {
            // if it is empty... we can safely just get the first available.
            heap_init_zero(base_addr);
            heap_init_zero(base_addr +sizeof(int64_t)*3);
            *zmask = *zmask | (1UL << (63 - midx));
        }
        
        uintptr_t binvmask = ~*(uint64_t*)(base_addr + sizeof(uint64_t));
        uint32_t bidx = -1;
        while((bidx = get_next_mask_idx(binvmask, bidx+1)) != -1)
        {
            uintptr_t sub_base_addr = (base_addr + (bidx*stable->sizes[1]));
            int32_t idx = 0;
            if(midx != 0)
            {
                uint64_t *bzmask = (uint64_t*)((uint8_t*)base_addr +sizeof(int64_t)*4);
                if(((1UL << (63 - bidx)) & *bzmask) == 0)
                {
                    // if it is empty... we can safely just get the first available.
                    idx = heap_init_head_range(h, sub_base_addr, sizeof(uint64_t)*6);
                    *bzmask = *bzmask | (1UL << (63 - bidx));
                }
                else
                {
                    idx = find_first_nzeros(*(uint64_t*)sub_base_addr, range);
                }
            }
            else
            {
                idx = find_first_nzeros(*(uint64_t*)sub_base_addr, range);
            }
            
            
            if(idx != -1)
            {
                uintptr_t mask = reserve_range_idx(range, idx);
                *(uint64_t*)sub_base_addr |= mask;
                idx = 63 - idx;
                if(range > 1)
                {
                    *(uint64_t*)(sub_base_addr+sizeof(uint64_t)) |= apply_range((uint32_t)range, idx);
                }
                h->num_allocations++;
                *l3_reserve = *l3_reserve | (1UL << (63 -  midx));
                if(*(uint64_t*)sub_base_addr == UINT64_MAX)
                {
                    uint64_t* scmask = (uint64_t*)(base_addr + sizeof(uint64_t));
                    *scmask = *scmask | (1UL << (63 -  bidx));
                    if(*(uint64_t*)scmask == UINT64_MAX)
                    {
                        filters[0] = filters[0] | (1UL << (63 -  midx));
                    }
                }
                h->previous_l1_offset = sub_base_addr;
                if(h->previous_l1_offset == (uintptr_t)masks[0])
                {
                    return (void*)(base + (idx*stable->sizes[0]));
                }
                else
                {
                    return (void*)(h->previous_l1_offset + (idx*stable->sizes[0]));
                }
            }
        }
    }
    h->previous_l1_offset  = 0;
    return NULL;
}

void* heap_get_block_at(Heap*h, size_t l3idx, size_t l2idx, size_t l1idx)
{
    uintptr_t base = ((uintptr_t)h & ~(os_page_size - 1));
    const heap_size_table* stable = get_size_table(h->container_exponent);
    uintptr_t offset = base + stable->sizes[2] * l3idx + stable->sizes[1] * l2idx + stable->sizes[0] * l1idx;
    return (void*)offset;
}

void* heap_get_block(Heap*h, size_t size)
{
    print_header(h, (uintptr_t)h);
    const heap_size_table* stable = get_size_table(h->container_exponent);
    
    uintptr_t base = ((uintptr_t)h & ~(os_page_size - 1));
    uintptr_t mask_offset = ((uintptr_t)h + sizeof(Heap) + (CACHE_LINE - 1)) & ~(CACHE_LINE - 1);
    uintptr_t data_offset = mask_offset + sizeof(uint64_t)*10;
    uintptr_t end_offset = (data_offset + (stable->sizes[3] - 1)) & ~(stable->sizes[3] - 1);
    uint64_t* masks[] = {
        (uint64_t*)mask_offset,
        (uint64_t*)(mask_offset + sizeof(uint64_t)*3),
        (uint64_t*)(mask_offset + sizeof(uint64_t)*6)};
        
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
            limit = stable->sizes[0];
        }
        else if(*masks[2] == UINT64_MAX)
        {
            limit = stable->sizes[1];
        }
        else
        {
            limit = stable->sizes[2];
        }
    }
    
    if(size > limit)
    {
        return NULL;
    }
    int32_t level_idx = 2;
    if(size < stable->sizes[1])
    {
        level_idx = 0;
    }
    else if(size < stable->sizes[2])
    {
        level_idx = 1;
    }
    
    size_t range = size >> stable->exponents[level_idx];
    range += (size & (stable->sizes[level_idx] - 1)) ? 1 : 0;
    void* res = NULL;
    switch(level_idx)
    {
        case 0:
        {
            res = heap_get_block_L1(h, base, range, masks);
            break;
        }
        case 1:
        {
            res = heap_get_block_L2(h, base, range, masks);
            break;
        }
        default:
        {
            res = heap_get_block_L3(h, base, range, masks);
            break;
        }
    }
    print_header(h, (uintptr_t)res);
    return res;
}

void heap_reset_L3(Heap*h, uintptr_t sub_mask, bool needs_zero)
{
    uintptr_t maskL3 = ((uintptr_t)h + sizeof(Heap) + (CACHE_LINE - 1)) & ~(CACHE_LINE - 1);
    uintptr_t filterL1 = maskL3 + sizeof(uint64_t);
    uintptr_t filterL2 = maskL3 + CACHE_LINE + sizeof(uint64_t);
    uintptr_t filterL3 = maskL3 + 2*CACHE_LINE;
    *(uint64_t*)filterL1 = *(uint64_t*)filterL1 & sub_mask;
    *(uint64_t*)filterL2 = *(uint64_t*)filterL2 & sub_mask;
    *(uint64_t*)filterL3 = *(uint64_t*)filterL3 & sub_mask;
    filterL1 = maskL3;
    filterL2 = maskL3 + CACHE_LINE;
    *(uint64_t*)filterL1 = *(uint64_t*)filterL1 & sub_mask;
    *(uint64_t*)filterL2 = *(uint64_t*)filterL2 & sub_mask;
    if(needs_zero)
    {
        filterL3 += sizeof(uint64_t);
        *(uint64_t*)filterL3 = *(uint64_t*)filterL3 & sub_mask;
    }
}

void heap_free_L3(Heap*h, void*p, uintptr_t mask, uintptr_t sub_mask)
{
    heap_reset_L3(h, sub_mask, true);
}


void heap_free_L2(Heap*h, void*p, uintptr_t mask, uintptr_t sub_mask, uintptr_t root_mask, uint32_t ridx, bool needs_zero)
{
    uintptr_t rfilter = root_mask + CACHE_LINE + sizeof(uint64_t);
    *(uint64_t*)rfilter = *(uint64_t*)rfilter & ~(1UL << (63 - ridx));
    rfilter = root_mask + sizeof(uint64_t);
    *(uint64_t*)rfilter = *(uint64_t*)rfilter & ~(1UL << (63 - ridx));
    if(ridx != 0)
    {
        if(*(uint64_t*)mask == heap_empty_mask)
        {
            heap_reset_L3(h, ~(1UL << (63 - ridx)), needs_zero);
        }
        if(needs_zero)
        {
            uintptr_t rfilter = root_mask + CACHE_LINE*2 + sizeof(uint64_t);
            *(uint64_t*)rfilter = *(uint64_t*)rfilter & ~(1UL << (63 - ridx));
        }
    }
}

void heap_free_L1(Heap*h, void*p, uintptr_t mask, uintptr_t sub_mask, uintptr_t root_mask, uint32_t ridx)
{
    
    uintptr_t rfilter = root_mask + sizeof(uint64_t);
    *(uint64_t*)rfilter = *(uint64_t*)rfilter & ~(1UL << (63 - ridx));
    if(ridx != 0)
    {
        if(*(uint64_t*)mask == heap_empty_mask_z)
        {
            const heap_size_table* stable = get_size_table(h->container_exponent);
            mask = ((uintptr_t)p & ~(stable->sizes[2] - 1));
            ptrdiff_t diff = (uint8_t *)p - (uint8_t *)mask;
            uint32_t idx = 63 - (uint32_t)((size_t)diff >> stable->exponents[1]);
            uintptr_t rfilter = mask + sizeof(uint64_t);
            *(uint64_t*)rfilter = *(uint64_t*)rfilter & ~(1UL << (63 - idx));
            heap_free_L2(h, p, mask + CACHE_LINE, ~(1UL << idx), root_mask, ridx, false );
        }
    }
}



void heap_free(Heap*h, void*p, bool dummy)
{
    const heap_size_table* stable = get_size_table(h->container_exponent);
    
    uintptr_t root_mask = ((uintptr_t)h + sizeof(Heap) + (CACHE_LINE - 1)) & ~(CACHE_LINE - 1);
    ptrdiff_t rdiff = (uint8_t *)p - (uint8_t *)root_mask;
    uint32_t ridx = (uint32_t)((size_t)rdiff >> stable->exponents[2]);
    for(int i = 0; i < 3; i++)
    {
        if(((uintptr_t)p & (stable->sizes[i] - 1)) == 0)
        {
            // aligned to base level
            uintptr_t mask = ((uintptr_t)p & ~(stable->sizes[i + 1] - 1));
            const ptrdiff_t diff = (uint8_t *)p - (uint8_t *)mask;
            const uint32_t idx = (uint32_t)((size_t)diff >> stable->exponents[i]);
            if(mask < (uintptr_t)h)
            {
                // we at the start
                mask = root_mask + i*CACHE_LINE;
            }

            uint32_t range = get_range(idx, mask);
            uintptr_t sub_mask = ~reserve_range_idx(range+1, 63 - idx);
            *(uint64_t*)mask = *(uint64_t*)mask & sub_mask;
            *(uint64_t*)(mask + sizeof(uint64_t)) = *(uint64_t*)(mask + sizeof(uint64_t)) & sub_mask;
            switch(i)
            {
                case 0:
                    heap_free_L1(h, p, mask, sub_mask, root_mask, ridx);
                    break;
                case 1:
                    heap_free_L2(h, p, mask, sub_mask, root_mask, ridx, true);
                    break;
                default:
                    heap_free_L3(h, p, mask, sub_mask);
                    break;
            };
            h->num_allocations--;
            if(h->num_allocations == 0)
            {
                if (h->container_exponent == 22) {
                    Section *section = (Section *)((uintptr_t)h & ~(SECTION_SIZE - 1));
                    section_free_all(section);
                } else {
                    size_t area_size = area_size_from_addr((uintptr_t)h);
                    Area *area = (Area *)((uintptr_t)h & ~(area_size - 1));
                    area_free_all(area);
                }
            }
            
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
            
            uint32_t range = get_range(idx, mask);
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
