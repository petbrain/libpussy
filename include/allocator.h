#pragma once

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Pussy allocator functions reallocate() and release()
 * require the number of bytes argument for allocated block.
 *
 * This is different from traditional allocators that store
 * the size of allocated block.
 *
 * The approach is more error-prone.
 * However, it is more efficient for small blocks.
 *
 *
 * Caveat: unlike other types, the compiler sees no difference
 * between void* and void**.
 * This makes use of release() extremely error prone.
 */

typedef void  (*FnInitAllocator)();
typedef void* (*FnAllocate)  (unsigned nbytes, bool clean);
typedef bool  (*FnReallocate)(void** addr_ptr, unsigned old_nbytes, unsigned new_nbytes, bool clean, bool* addr_changed);
typedef void  (*FnRelease)   (void** addr_ptr, unsigned nbytes);
typedef void  (*FnDump)();

typedef struct {
    atomic_size_t blocks_allocated;
} AllocatorStats;

typedef struct {
    FnInitAllocator init;  // optional, can be nullptr
    FnAllocate   allocate;
    FnReallocate reallocate;
    FnRelease    release;
    FnDump       dump;

    AllocatorStats* stats;

    // optionally supported:
    bool verbose;
    bool trace;

} Allocator;

/****************************************************************
 * Allocators.
 */

extern Allocator pet_allocator;
extern Allocator stdlib_allocator;
extern Allocator debug_allocator;  // checks if memory was damaged around the block

/****************************************************************
 * Alignment helpers.
 */

extern unsigned sys_page_size;

/*
 * Align to `alignment` boundary which must be a power of two or zero.
 */

static inline unsigned align_unsigned(unsigned n, unsigned alignment)
{
    if (alignment > 1) {
        alignment--;
        return (n + alignment) & ~alignment;
    } else {
        return n;
    }
}

static inline void* align_pointer(void* ptr, unsigned alignment)
{
    if (alignment > 1) {
        ptrdiff_t n = (ptrdiff_t) ptr;
        alignment--;
        return (void*) ((n + alignment) & ~(ptrdiff_t) alignment);
    } else {
        return ptr;
    }
}

/*
 * Align to page boundary.
 */

static inline unsigned align_unsigned_to_page(unsigned n)
{
    return align_unsigned(n, sys_page_size);
}

static inline void* align_pointer_to_page(void* ptr)
{
    return align_pointer(ptr, sys_page_size);
}

/****************************************************************
 * Default allocator and shorthand wrappers.
 */

extern Allocator default_allocator;  // uninitialized by default, use init_allocator at startup

static inline void init_allocator(Allocator* allocator)
{
    if (allocator->init) {
        allocator->init();
    }
    default_allocator = *allocator;
}

static inline void* allocate(unsigned nbytes, bool clean)
{
    return default_allocator.allocate(nbytes, clean);
}

static bool inline reallocate(void** addr_ptr, unsigned old_nbytes, unsigned new_nbytes, bool clean, bool* addr_changed)
{
    return default_allocator.reallocate(addr_ptr, old_nbytes, new_nbytes, clean, addr_changed);
}

static inline void release(void** addr_ptr, unsigned nbytes)
{
    default_allocator.release(addr_ptr, nbytes);
}

#ifdef __cplusplus
}
#endif
