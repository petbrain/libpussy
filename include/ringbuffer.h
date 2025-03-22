#pragma once

#include <threads.h>
#include <libpussy/sync.h>

#ifdef __cplusplus
extern "C" {
#endif

/****************************************************************
 * Basic ring buffer
 */

typedef struct {
    uint8_t *data;
    unsigned size;
    unsigned head;
    unsigned tail;
} RingBuffer;

bool init_ringbuffer(RingBuffer* ringbuf, unsigned size);
/*
 * Initialize ring buffer.
 *
 * `size` should be multiple of page size, although it is not
 * strictly required. It will be rounded as necessary.
 */

void fini_ringbuffer(RingBuffer* ringbuf);
/*
 * Finalize ring buffer.
 */

RingBuffer* create_ringbuffer(unsigned size);
/*
 * Allocate RingBuffer structure and call init_ringbuffer.
 */

void delete_ringbuffer(RingBuffer** ringbuf_ptr);
/*
 * Destructor.
 * The type of argument is natural for autocleaned variables.
 */

bool grow_ringbuffer(RingBuffer* ringbuf, unsigned new_size);
/*
 * Reallocate buffer to new size.
 */

void shrink_ringbuffer(RingBuffer* ringbuf, unsigned new_size);
/*
 * Release unused pages back to the system,
 * making the buffer as small as possible but no smaller than `new_size`.
 */

unsigned read_ringbuffer(RingBuffer* ringbuf, uint8_t* buffer, unsigned size);
/*
 * Read the data from ring buffer.
 * Return the number of bytes read.
 */

bool write_ringbuffer(RingBuffer* ringbuf, uint8_t* data, unsigned size);
/*
 * Write the data into ring buffer.
 *
 * Return false if no memory available in the buffer.
 */

/****************************************************************
 * Ring buffer with synchronization
 */

typedef struct {
    RingBuffer  ringbuf;
    mtx_t       lock;  // mutex to serialize ring buffer operations
    Event*      more;  // set when data is written to buffer
    Event*      less;  // set when data is read from buffer or the buffer is extended
} SyncRingBuffer;

bool srb_init(SyncRingBuffer* ringbuf, unsigned size);
void srb_fini(SyncRingBuffer* ringbuf);
SyncRingBuffer* srb_create(unsigned size);
void srb_delete(SyncRingBuffer** srb_ptr);
bool srb_grow(SyncRingBuffer* srb, unsigned new_size);
void srb_shrink(SyncRingBuffer* srb, unsigned new_size);
unsigned srb_read(SyncRingBuffer* srb, uint8_t* buffer, unsigned size);
bool srb_write(SyncRingBuffer* srb, uint8_t* data, unsigned size);

#ifdef __cplusplus
}
#endif
