#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct _FsbaPageHeader;
typedef struct _FsbaPageHeader FsbaPageHeader;

typedef struct {
    unsigned block_size;         // block alignment equals to block_size
    unsigned blocks_per_page;    // ??? (sys_page_size - align(sizeof(FsbArena) + bitmap_size * sizeof(WORD), block_size)) / block_size
    unsigned bitmap_size;        // in words
    FsbaPageHeader* avail_pages; // list of pages with free blocks, one page is always allocated
    FsbaPageHeader* full_pages;  // list of full pages
} FsbArena;


bool _init_fsb_arena(FsbArena* arena, unsigned block_size, unsigned block_alignment);

#define init_fsb_arena(arena, data_type)  _init_fsb_arena((arena), sizeof(data_type), alignof(data_type))

void destroy_fsb_arena(FsbArena* arena);

void* fsb_arena_allocate(FsbArena* arena);
void fsb_arena_release(void** block_ptr);

void dump_fsb_arena(FsbArena* arena);


#ifdef __cplusplus
}
#endif
