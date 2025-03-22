#pragma once

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _Arena Arena;
/*
 * Arena is a singly linked list of regions allocated with mmap.
 */

Arena* create_arena(unsigned capacity);
/*
 * Create new arena with one region of desired capacity.
 *
 * The capacity of subsequent regions will have
 * the same capacity unless adjusted with `set_region_capacity`.
 */

void delete_arena(Arena* arena);
/*
 * Free arena and all its regions.
 */

void set_region_capacity(Arena* arena, unsigned capacity);
/*
 * Set desired capacity for newly created regions.
 */

void* _arena_alloc(Arena* arena, unsigned size, unsigned alignment);
/*
 * Allocate `size` bytes from the last region aligned at `alignment` boundary.
 * If the last region has no space available, allocate new region.
 *
 * This is a low-level function because its arguments
 * tell how to allocate a block.
 *
 * The following macro is a wrapper to allocate things of specific type:
 */

#define arena_alloc(arena, num_elements, element_type) \
    _arena_alloc((arena), (num_elements) * sizeof(element_type), alignof(element_type))

void* _arena_fit(Arena* arena, unsigned size, unsigned element_size);
/*
 * Try to find a region with sufficient free space
 * and allocate from it.
 * If no region has space available, allocate new region.
 *
 * This is a low-level function because its arguments
 * tell how to allocate a block.
 *
 * The following macro gets what to allocate:
 */

#define arena_fit(arena, num_elements, element_type) \
    _arena_fit((arena), (num_elements) * sizeof(element_type), alignof(element_type))

void arena_print(FILE* fp, Arena* arena);

#ifdef __cplusplus
}
#endif
