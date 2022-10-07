
#include "callocator.inl"
#include "../cthread/cthread.h"

void deferred_move_thread_free(DeferredFree* d)
{
    AtomicMessage *back = (AtomicMessage *)atomic_load_explicit(&d->thread_free.tail, memory_order_relaxed);
    AtomicMessage *curr = (AtomicMessage *)(uintptr_t)d->thread_free.head;
    // loop between start and end addres
    while (curr != back) {
        AtomicMessage *next = (AtomicMessage *)atomic_load_explicit(&curr->next, memory_order_acquire);
        if (next == NULL)
            break;
        ((Block*)curr)->next = d->free;
        d->free = (Block*)curr;
        curr = next;
    }
    d->thread_free.head = (uintptr_t)curr;
}

void deferred_thread_enqueue(AtomicQueue *queue, AtomicMessage *first, AtomicMessage *last)
{
    atomic_store_explicit(&last->next, (uintptr_t)NULL, memory_order_release); // last.next = null
    AtomicMessage *prev = (AtomicMessage *)atomic_exchange_explicit(&queue->tail, (uintptr_t)last,
                                                         memory_order_release); // swap back and last
    atomic_store_explicit(&prev->next, (uintptr_t)first, memory_order_release); // prev.next = first
}



// compute bounds and initialize
void deferred_cache_init(deferred_cache*c, void*p)
{
    /*
    id = owner_id(p);
    owning_thread = this_owns(id);
    size = get_arena_size(p);
    start = p >> size;
    end = start + size;

    p->next = NULL;
    deferredList->head = p;
    deferredList->tail = p;
    */
}

// release to owning structures.
void deferred_cache_release(deferred_cache*c, void* p)
{
    /*
        // get the area/arena from the start address of cache.
     */
    if(p)
    {
        deferred_cache_init(c, p);
    }
    else
    {
        c->start = UINT64_MAX;
        c->end = 0;
    }
}
