#ifndef _GNU_SOURCE
#   define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>

#include "mmarray.h"

typedef struct {
    unsigned capacity;
    unsigned length;
    unsigned item_size;
    unsigned padding;
} ArrayHeader;

static size_t page_size = 0;

static inline void get_page_size()
{
    if (!page_size) {
        page_size = sysconf(_SC_PAGE_SIZE);
    }
}

static size_t calc_memsize(unsigned capacity, unsigned item_size)
{
    get_page_size();
    return (sizeof(ArrayHeader) + ((size_t) capacity) * item_size + page_size - 1) & ~(page_size - 1);
}

static inline ArrayHeader* get_array_header(void* array)
{
    get_page_size();
    return (ArrayHeader*) (
        ((ptrdiff_t) array) & ~(page_size - 1)  // header starts on page boundary
    );
}

void* mmarray_allocate(unsigned length, unsigned item_size)
{
    size_t memsize = calc_memsize(length, item_size);

    ArrayHeader* a = mmap(NULL, memsize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (a == MAP_FAILED) {
        perror("mmap");
        abort();
    }
    a->capacity = (memsize - sizeof(ArrayHeader)) / item_size;
    a->length = length;
    a->item_size = item_size;

    // return pointer to the array data
    return a + 1;
}

void* mmarray_grow(void* array, unsigned increment)
{
    ArrayHeader* a = get_array_header(array);

    if (a->length + increment > a->capacity) {
        size_t old_memsize = calc_memsize(a->capacity, a->item_size);
        size_t new_memsize = calc_memsize(a->length + increment, a->item_size);

        ArrayHeader* new_array = mremap(a, old_memsize, new_memsize, MREMAP_MAYMOVE);
        if (new_array == MAP_FAILED) {
            perror("mremap");
            abort();
        }
        a = new_array;
        a->capacity = (new_memsize - sizeof(ArrayHeader)) / a->item_size;
    }
    a->length += increment;

    // return pointer to the array data
    return a + 1;
}

void* mmarray_append_item(void* array, void* item)
{
    unsigned index = mmarray_length(array);

    array = mmarray_grow(array, 1);
    ArrayHeader* a = get_array_header(array);

    memcpy(((char*) array) + (index * a->item_size), item, a->item_size);

    // return pointer to the array data
    return a + 1;
}

unsigned mmarray_length(void* array)
{
    return get_array_header(array)->length;
}

unsigned mmarray_capacity(void* array)
{
    return get_array_header(array)->capacity;
}
