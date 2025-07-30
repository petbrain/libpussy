#include <assert.h>
#include <sys/mman.h>
#include <unistd.h>

#include "arena.h"
#include "allocator.h"

[[ gnu::constructor ]]
static void init_page_size()
{
    if (sys_page_size == 0) {
        sys_page_size = sysconf(_SC_PAGE_SIZE);
    }
}

/**********************************************************
 * General purpose helper functions and macros
 */

#define is_power_of_two(v) (v && !((v & (v - 1))))
// http://www.graphics.stanford.edu/~seander/bithacks.html#DetermineIfPowerOf2

#define max(a, b) (((a) > (b))? (a) : (b))

static void* alloc_mem(unsigned size)
{
    void* result = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (result == MAP_FAILED) {
        perror("Arena allocation");
        return nullptr;
    }
    return result;
}

static inline void free_mem(void* ptr, unsigned size)
{
    munmap(ptr, size);
}

/**********************************************************
 * Arena and Region structures
 */

typedef struct _Region Region;

struct _Region {
    Region* next;
    unsigned tail;
    unsigned capacity;
    char data[1];
};

#define REGION_HEADER_SIZE offsetof(Region, data)

struct _Arena {
    Region* last;
    unsigned new_region_capacity;
    Region  first;  // arena embeds the first region
};

#define ARENA_HEADER_SIZE (offsetof(Arena, first) + REGION_HEADER_SIZE)


/**********************************************************
 * Internal functions
 */

static inline void free_arena(Arena* arena)
{
    free_mem(arena, arena->first.capacity + ARENA_HEADER_SIZE);
}

static inline void free_region(Region* region)
{
    free_mem(region, region->capacity + REGION_HEADER_SIZE);
}

static Region* create_region(unsigned capacity)
{
    unsigned mem_size = align_unsigned_to_page(capacity + REGION_HEADER_SIZE);
    Region* region = alloc_mem(mem_size);
    if (region) {
        region->next = nullptr;
        region->tail = 0;
        region->capacity = mem_size - REGION_HEADER_SIZE;
    }
    return region;
}

static void* region_alloc(Region* region, unsigned size, unsigned alignment)
/*
 * Allocate aligned `size` bytes from `region`.
 * Return nullptr if `region` has no space available.
 */
{
    assert(size > 0);
    assert(alignment <= alignof(max_align_t) && is_power_of_two(alignment));

    unsigned start = align_unsigned(region->tail + offsetof(Region, data), alignment) - offsetof(Region, data);
    if (start >= region->capacity) {
        return nullptr;
    }

    unsigned bytes_available = region->capacity - start;
    if (size > bytes_available) {
        return nullptr;
    }

    void* result = &region->data[start];
    region->tail = start + size;
    return result;
}

static void* new_region_alloc(Arena* arena, unsigned size, unsigned alignment)
/*
 * Create new region and allocate aligned `size` bytes from it.
 */
{
    Region* new_region = create_region(max(size, arena->new_region_capacity));
    if (!new_region) {
        return nullptr;
    }
    arena->last->next = new_region;
    arena->last = new_region;
    return region_alloc(new_region, size, alignment);
}

/**********************************************************
 * Public API
 */

Arena* create_arena(unsigned capacity)
{
    unsigned mem_size = align_unsigned_to_page(capacity + ARENA_HEADER_SIZE);
    Arena* arena = (Arena*) alloc_mem(mem_size);
    if (arena) {
        arena->last = &arena->first;

        arena->first.next = nullptr;
        arena->first.tail = 0;
        arena->first.capacity = mem_size - ARENA_HEADER_SIZE;
        arena->new_region_capacity = capacity;
    }
    return arena;
}

void delete_arena(Arena* arena)
{
    for (Region* region = arena->first.next; region != nullptr;) {

        assert(region->tail <= region->capacity);

        Region* next_region = region->next;
        free_region(region);
        region = next_region;
    }

    assert(arena->first.tail <= arena->first.capacity);

    free_arena(arena);
}

void set_region_capacity(Arena* arena, unsigned capacity)
{
    arena->new_region_capacity = capacity;
}

void* _arena_alloc(Arena* arena, unsigned size, unsigned alignment)
{
    void* result = region_alloc(arena->last, size, alignment);
    if (result) {
        return result;
    } else {
        return new_region_alloc(arena, size, alignment);
    }
}

void* _arena_fit(Arena* arena, unsigned size, unsigned alignment)
{
    for (Region* region = &arena->first; region != nullptr; region = region->next) {
        void* result = region_alloc(region, size, alignment);
        if (result) {
            return result;
        }
    }
    return new_region_alloc(arena, size, alignment);
}

void arena_print(FILE* fp, Arena* arena)
{
    fprintf(fp, "Arena at %p\n", (void*) arena);
    fprintf(fp, "last region: %p\n", (void*) arena->last);
    fprintf(fp, "new_region_capacity: %u\n", arena->new_region_capacity);
    fprintf(fp, "first region -> next: %p\n", (void*) arena->first.next);
    fprintf(fp, "first region -> tail: %u\n", arena->first.tail);
    fprintf(fp, "first region -> capacity: %u\n", arena->first.capacity);
    for (Region* region = arena->first.next; region != nullptr; region = region->next) {
        fprintf(fp, "\nRegion %p\n", (void*) region);
        fprintf(fp, "next region: %p\n", (void*) region->next);
        fprintf(fp, "tail: %u\n", region->tail);
        fprintf(fp, "capacity: %u\n", region->capacity);
    }
}
