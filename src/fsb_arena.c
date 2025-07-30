#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include "allocator.h"  // for align_unsigned
#include "dump.h"
#include "fsb_arena.h"
#include "src/word.h"

struct _FsbaPageHeader {
    FsbArena* arena;
    struct _FsbaPageHeader* next;
    struct _FsbaPageHeader* prev;
    unsigned num_free;
    Word bitmap[ /* bitmap_size */ ];
};

static void add_to_list(FsbaPageHeader** list, FsbaPageHeader* page)
/*
 * Add page to circular doubly-linked list.
 */
{
    FsbaPageHeader* first = *list;
    if (first == nullptr) {
        // init list
        *list = page->next = page->prev = page;
    } else {
        page->prev = first->prev;
        page->next = first;
        first->prev->next = page;
        first->prev = page;
        *list = page;
    }
}

static void delete_from_list(FsbaPageHeader** list, FsbaPageHeader* page)
/*
 * Delete page from circular doubly-linked list.
 */
{
    if (page->next == page->prev) {
        // last page, make list empty
        *list = nullptr;
    } else {
        if (*list == page) {
            *list = page->next;
        }
        page->next->prev = page->prev;
        page->prev->next = page->next;
    }
}

static inline void free_page(FsbaPageHeader* page)
{
    munmap(page, sys_page_size);
}

bool _init_fsb_arena(FsbArena* arena, unsigned block_size, unsigned block_alignment)
{
    if (block_size < block_alignment) {
        arena->block_size = block_alignment;
    } else {
        arena->block_size = block_size;
    }
    unsigned header_size = align_unsigned(sizeof(FsbaPageHeader) + sizeof(Word), arena->block_size);
    unsigned max_block_size = sys_page_size - header_size;

    if (arena->block_size > max_block_size) {
        return false;
    }

    // calculate bitmap size and blocks per page
    arena->bitmap_size = 0;
    do {
        arena->bitmap_size++;
        header_size = align_unsigned(sizeof(FsbaPageHeader) + sizeof(Word) * arena->bitmap_size, arena->block_size);
        arena->blocks_per_page = (sys_page_size - header_size) / arena->block_size;
    } while (arena->blocks_per_page > (arena->bitmap_size * WORD_WIDTH));

    // initialize lists
    arena->avail_pages = nullptr;
    arena->full_pages = nullptr;
    return true;
}

void destroy_fsb_arena(FsbArena* arena)
{
    FsbaPageHeader* page = arena->avail_pages;
    FsbaPageHeader* next;
    if (page) {
        do {
            next = page->next;
            free_page(page);
        } while (page != next);
    }
    page = arena->full_pages;
    if (page) {
        do {
            next = page->next;
            free_page(page);
        } while (page != next);
    }
    arena->avail_pages = nullptr;
    arena->full_pages = nullptr;
}

void* fsb_arena_allocate(FsbArena* arena)
{
    FsbaPageHeader* page = arena->avail_pages;
    if (!page) {
        // allocate new page
        page = mmap(nullptr, sys_page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED) {
            return nullptr;
        }
        page->arena = arena;
        page->num_free = arena->blocks_per_page;
        Word* bitmap = page->bitmap;
        for (unsigned i = 0, n = arena->bitmap_size; i < n; i++) {
            *bitmap++ = 0;
        }
        add_to_list(&arena->avail_pages, page);
    }
    // find available position in the bitmap
    unsigned bitmap_size = arena->bitmap_size;
    Word* bitmap = page->bitmap;
    for (unsigned i = 0; i < bitmap_size; i++, bitmap++) {
        Word w = ~*bitmap;
        if (w) {
            // found, do allocate
            unsigned block_size = arena->block_size;
            unsigned header_size = align_unsigned(sizeof(FsbaPageHeader) + sizeof(Word) * bitmap_size, block_size);
            unsigned bit_index = count_trailing_zeros(w);
            *bitmap |= ((Word) 1) << bit_index;

            // decrement free blocks counter
            page->num_free--;
            if (page->num_free == 0) {
                // the page is full, move it from avail_pages to full_pages
                delete_from_list(&arena->avail_pages, page);
                add_to_list(&arena->full_pages, page);
            }
            return ((uint8_t*) page) + header_size + (i * WORD_WIDTH + bit_index) * block_size;
        }
    }
    fputs("FSB arena: bad bitmap\n", stderr);
    abort();
}

void fsb_arena_release(void** block_ptr)
{
    void* block = *block_ptr;
    if (!block) {
        return;
    }
    *block_ptr = nullptr;

    // get page from block address
    FsbaPageHeader* page = (FsbaPageHeader*)( ((ptrdiff_t) block) & ~(ptrdiff_t) (sys_page_size - 1) );

    // calculate bit index in the bitmap
    FsbArena* arena = page->arena;
    unsigned offset = ((uint8_t*) block) - ((uint8_t*) page);
    unsigned block_size = arena->block_size;
    unsigned header_size = align_unsigned(sizeof(FsbaPageHeader) + sizeof(Word) * arena->bitmap_size, block_size);
    unsigned index = (offset - header_size) / block_size;

    // clear bit
    page->bitmap[index / WORD_WIDTH] &= ~(((Word) 1) << (index & (WORD_WIDTH - 1)));

    // increment free blocks counter
    page->num_free++;
    if (page->num_free == arena->blocks_per_page) {
        // entire page is free now
        FsbaPageHeader* list = arena->avail_pages;
        if (list->next != list->prev) {
            // not the last page, reclaim it back to the operating system
            delete_from_list(&arena->avail_pages, page);
            free_page(page);
        }
    } else if (page->num_free == 1) {
        // the page was full, move it from full_pages to avail_pages
        delete_from_list(&arena->full_pages, page);
        add_to_list(&arena->avail_pages, page);
    }
}

static void dump_page(FsbaPageHeader* page)
{
    fprintf(stderr, "Page %p: %u free blocks\n", (void*) page, page->num_free);
    dump_bitmap(stderr, (uint8_t*)(page->bitmap), page->arena->bitmap_size * sizeof(Word));
}

void dump_fsb_arena(FsbArena* arena)
{
    fprintf(stderr, "\nFSB arena: block_size=%u, blocks_per_page=%u, bitmap_size=%u, word_size=%zu\nAvailable pages:",
            arena->block_size, arena->blocks_per_page, arena->bitmap_size, sizeof(Word));
    if (arena->avail_pages == nullptr) {
        fputs(" none\n", stderr);
    } else {
        fputc('\n', stderr);
        FsbaPageHeader* first_page = arena->avail_pages;
        FsbaPageHeader* page = first_page;
        do {
            dump_page(page);
            page = page->next;
        } while (page != first_page);
    }
    fputs("Full pages:", stderr);
    if (arena->full_pages == nullptr) {
        fputs(" none\n", stderr);
    } else {
        fputc('\n', stderr);
        FsbaPageHeader* first_page = arena->full_pages;
        FsbaPageHeader* page = first_page;
        do {
            dump_page(page);
            page = page->next;
        } while (page != first_page);
    }
    fputc('\n', stderr);
}
