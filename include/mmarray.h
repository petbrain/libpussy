#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void* mmarray_allocate(unsigned length, unsigned item_size);
/*
 * Allocate array of desired `length` and `item_size` using mmap.
 * Return pointer to the array.
 * Abort the program if mmap fails.
 */

void* mmarray_grow(void* array, unsigned increment);
/*
 * Reallocate array if necessary using mremap.
 * Return new pointer.
 * Abort the program if mremap fails.
 */

void* mmarray_append_item(void* array, void* item);
/*
 * Append new item to the array.
 * Return new pointer.
 * Abort the program if array_grow fails.
 */

unsigned mmarray_length(void* array);
unsigned mmarray_capacity(void* array);

#ifdef __cplusplus
}
#endif
