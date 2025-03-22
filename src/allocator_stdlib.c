#include "stdio.h"
#include "stdlib.h"
#include "string.h"

#include "allocator.h"

static AllocatorStats stats = {};

static void* _allocate(unsigned nbytes, bool clean)
{
    void* result;
    if (clean) {
        result = calloc(1, nbytes);
    } else {
        result = malloc(nbytes);
    }
    if (result) {
        atomic_fetch_add(&stats.blocks_allocated, 1);
    }
    return result;
}

static void _release(void** addr_ptr, unsigned nbytes)
{
    void* addr = *addr_ptr;
    if (addr) {
        free(addr);
        *addr_ptr = nullptr;
        atomic_fetch_sub(&stats.blocks_allocated, 1);
    }
}

static bool _reallocate(void** addr_ptr, unsigned old_nbytes, unsigned new_nbytes, bool clean, bool* addr_changed)
{
    if (old_nbytes == new_nbytes) {
        goto success_same_addr;
    }

    void* addr = *addr_ptr;

    // shall we allocate new addr?
    if (addr == nullptr) {
        if (old_nbytes != 0) {
            goto error;
        }
        addr = _allocate(new_nbytes, clean);
        if (!addr) {
            goto error;
        }
        *addr_ptr = addr;
        goto success_changed_addr;
    }

    void* new_block = realloc(addr, new_nbytes);
    if (!new_block) {
        goto error;
    }
    *addr_ptr = new_block;
    if (addr_changed) { *addr_changed = new_block != addr; }
    if (clean && old_nbytes < new_nbytes) {
        memset(((uint8_t*) new_block) + old_nbytes, 0, new_nbytes - old_nbytes);
    }
    return true;

success_changed_addr:
    if (addr_changed) { *addr_changed = true; }
    return true;

success_same_addr:
    if (addr_changed) { *addr_changed = false; }
    return true;

error:
    if (addr_changed) { *addr_changed = false; }
    return false;
}

static void _dump()
{
    fprintf(stderr, "Stdlib allocator: dump is not implemented\n");
}

Allocator stdlib_allocator = {
    .init       = nullptr,
    .allocate   = _allocate,
    .reallocate = _reallocate,
    .release    = _release,
    .dump       = _dump,
    .trace      = false,
    .verbose    = false,
    .stats      = &stats
};
