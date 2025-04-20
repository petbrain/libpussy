#include <stdio.h>
#include <stdlib.h>

#include "allocator.h"
#include "ringbuffer.h"

bool srb_init(SyncRingBuffer* srb, unsigned size)
{
    if (init_ringbuffer(&srb->ringbuf, size)) {
        if (mtx_init(&srb->lock, mtx_plain) != thrd_success) {
            fprintf(stderr, "%s cannot init mutex\n", __func__);
        } else {
            srb->more = create_event();
            srb->less = create_event();
            if (srb->more && srb->less) {
                set_event(srb->less);  // the buffer is empty
                return true;
            }
            delete_event(&srb->more);
            delete_event(&srb->less);
        }
        fini_ringbuffer(&srb->ringbuf);
    }
    return false;
}

void srb_fini(SyncRingBuffer* srb)
{
    if (srb->ringbuf.data) {
        mtx_destroy(&srb->lock);
        delete_event(&srb->more);
        delete_event(&srb->less);
        fini_ringbuffer(&srb->ringbuf);
    }
}

SyncRingBuffer* srb_create(unsigned size)
{
    if (sys_page_size == 0) {
        fprintf(stderr, "%s: allocator is not initialized\n", __func__);
        abort();
    }
    SyncRingBuffer* srb = allocate(sizeof(SyncRingBuffer), true);
    if (srb) {
        if (srb_init(srb, size)) {
            return srb;
        }
        release((void**) &srb, sizeof(SyncRingBuffer));
    }
    return nullptr;
}

void srb_delete(SyncRingBuffer** srb_ptr)
{
    SyncRingBuffer* srb = *srb_ptr;
    if (srb) {
        srb_fini(srb);
        release((void**) &srb, sizeof(SyncRingBuffer));
    }
    *srb_ptr = nullptr;
}

bool srb_grow(SyncRingBuffer* srb, unsigned new_size)
{
    mtx_lock(&srb->lock);
    bool result = grow_ringbuffer(&srb->ringbuf, new_size);
    set_event(srb->less);
    mtx_unlock(&srb->lock);
    return result;
}

void srb_shrink(SyncRingBuffer* srb, unsigned new_size)
{
    mtx_lock(&srb->lock);
    shrink_ringbuffer(&srb->ringbuf, new_size);
    mtx_unlock(&srb->lock);
}

unsigned srb_read(SyncRingBuffer* srb, uint8_t* buffer, unsigned size)
{
    mtx_lock(&srb->lock);
    unsigned result = read_ringbuffer(&srb->ringbuf, buffer, size);
    set_event(srb->less);
    mtx_unlock(&srb->lock);
    return result;
}

bool srb_write(SyncRingBuffer* srb, uint8_t* data, unsigned size)
{
    mtx_lock(&srb->lock);
    bool result = write_ringbuffer(&srb->ringbuf, data, size);
    set_event(srb->more);
    mtx_unlock(&srb->lock);
    return result;
}
