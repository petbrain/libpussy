#ifndef _GNU_SOURCE
#   define _GNU_SOURCE
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include "allocator.h"
#include "ringbuffer.h"

bool init_ringbuffer(RingBuffer* ringbuf, unsigned size)
{
    ringbuf->size = align_unsigned(size, sys_page_size);
    if (ringbuf->size == 0) {
        ringbuf->size = sys_page_size;
    }
    ringbuf->data = mmap(nullptr, ringbuf->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ringbuf->data == MAP_FAILED) {
        fprintf(stderr, "%s mmap: %s\n", __func__, strerror(errno));
        return false;
    }
    ringbuf->head = 0;
    ringbuf->tail = 0;
    return true;
}

void fini_ringbuffer(RingBuffer* ringbuf)
{
    if (ringbuf->data) {
        munmap(ringbuf->data, ringbuf->size);
        ringbuf->data = nullptr;
    }
}

RingBuffer* create_ringbuffer(unsigned size)
{
    if (sys_page_size == 0) {
        fprintf(stderr, "%s: allocator is not initialized\n", __func__);
        abort();
    }
    RingBuffer* ringbuf = allocate(sizeof(RingBuffer), false);
    if (ringbuf) {
        if (init_ringbuffer(ringbuf, size)) {
            return ringbuf;
        }
        release((void**) &ringbuf, sizeof(RingBuffer));
    }
    return nullptr;
}

void delete_ringbuffer(RingBuffer** ringbuf_ptr)
{
    RingBuffer* ringbuf = *ringbuf_ptr;
    if (ringbuf) {
        fini_ringbuffer(ringbuf);
        release((void**) &ringbuf, sizeof(RingBuffer));
    }
    *ringbuf_ptr = nullptr;
}

bool grow_ringbuffer(RingBuffer* ringbuf, unsigned new_size)
{
    new_size = align_unsigned(new_size, sys_page_size);
    if (new_size == 0) {
        new_size = sys_page_size;
    }
    if (new_size <= ringbuf->size) {
        return true;
    }
    uint8_t* new_addr = mremap(ringbuf->data, ringbuf->size, new_size, MREMAP_MAYMOVE);
    if (new_addr == MAP_FAILED) {
        return false;
    }
    ringbuf->data = new_addr;

    if (ringbuf->head > ringbuf->tail) {
        // shift upper data up to the end of buffer
        uint8_t* upper_data = ringbuf->data + ringbuf->head;
        unsigned upper_data_size = ringbuf->size - ringbuf->head;
        unsigned offset = new_size - ringbuf->size;
        memmove(upper_data + offset, upper_data, upper_data_size);
        ringbuf->head += offset;
    }
    ringbuf->size = new_size;
    return true;
}

void shrink_ringbuffer(RingBuffer* ringbuf, unsigned new_size)
{
    new_size = align_unsigned(new_size, sys_page_size);
    if (new_size == 0) {
        new_size = sys_page_size;
    }
    if (new_size >= ringbuf->size) {
        return;
    }
    unsigned shrinkable_bytes;
    if (ringbuf->head == ringbuf->tail) {
        // buffer is empty
        ringbuf->head = ringbuf->tail = 0;
        shrinkable_bytes = ringbuf->size - sys_page_size;
        if (shrinkable_bytes == 0) {
            return;
        }
    } else {
        // buffer is not empty, check if we can shrink
        if (ringbuf->head > ringbuf->tail) {
            shrinkable_bytes = (ringbuf->head - ringbuf->tail - 1) & ~(sys_page_size - 1);
            if (shrinkable_bytes == 0) {
                return;
            }
            // shift upper data down
            uint8_t* upper_data = ringbuf->data + ringbuf->head;
            unsigned upper_data_size = ringbuf->size - ringbuf->head;
            memmove(upper_data - shrinkable_bytes, upper_data, upper_data_size);
            ringbuf->head -= shrinkable_bytes;
        } else {
            shrinkable_bytes = (ringbuf->head + ringbuf->size - ringbuf->tail - 1) & ~(sys_page_size - 1);
            if (shrinkable_bytes == 0) {
                return;
            }
            // shift data down
            if (ringbuf->head) {
                memmove(ringbuf->data, ringbuf->data + ringbuf->head, ringbuf->tail - ringbuf->head);
                ringbuf->tail -= ringbuf->head;
                ringbuf->head = 0;
            }
        }
    }
    if (new_size < ringbuf->size - shrinkable_bytes) {
        new_size = ringbuf->size - shrinkable_bytes;
    }
    uint8_t* new_addr = mremap(ringbuf->data, ringbuf->size, new_size, MREMAP_MAYMOVE);
    if (new_addr == MAP_FAILED) {
        fprintf(stderr, "%s mremap(%p, %u, %u): %s\n", __func__, ringbuf->data, ringbuf->size, new_size, strerror(errno));
        abort();
    }
    ringbuf->data = new_addr;
    ringbuf->size = new_size;
    return;
}

bool write_ringbuffer(RingBuffer* ringbuf, uint8_t* data, unsigned size)
{
    unsigned bytes_avail;
    unsigned tail_len;
    if (ringbuf->head > ringbuf->tail) {
        tail_len = ringbuf->head - ringbuf->tail;
        bytes_avail = tail_len;
    } else {
        tail_len = ringbuf->size - ringbuf->tail;
        bytes_avail = ringbuf->head + tail_len;
    }

    if (size >= bytes_avail) {
        return false;
    }

    if (size < tail_len) {
        // tail does not wrap
        memcpy(&ringbuf->data[ringbuf->tail], data, size);
        ringbuf->tail += size;
    } else {
        // tail wraps around
        memcpy(&ringbuf->data[ringbuf->tail], data, tail_len);
        unsigned head_len = size - tail_len;
        if (head_len) {
            memcpy(ringbuf->data, data + tail_len, head_len);
        }
        ringbuf->tail = head_len;
    }
    return true;
}

unsigned read_ringbuffer(RingBuffer* ringbuf, uint8_t* buffer, unsigned buffer_size)
{
    if (ringbuf->head == ringbuf->tail) {
        return 0;
    }
    if (ringbuf->head < ringbuf->tail) {
        unsigned data_size = ringbuf->tail - ringbuf->head;
        unsigned len = (data_size <= buffer_size)? data_size : buffer_size;
        memcpy(buffer, &ringbuf->data[ringbuf->head], len);
        ringbuf->head += len;
        return len;
    }
    unsigned upper_len = ringbuf->size - ringbuf->tail;
    if (buffer_size <= upper_len) {
        memcpy(buffer, &ringbuf->data[ringbuf->head], buffer_size);
        if (buffer_size == upper_len) {
            ringbuf->head = 0;
        } else {
            ringbuf->head += upper_len;
        }
        return buffer_size;
    }
    memcpy(buffer, &ringbuf->data[ringbuf->head], upper_len);
    buffer_size -= upper_len;
    unsigned lower_len = (ringbuf->tail < buffer_size)? ringbuf->tail : buffer_size;
    memcpy(buffer + upper_len, ringbuf->data, lower_len);
    ringbuf->head = lower_len;
    return upper_len + lower_len;
}
