#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "allocator.h"
#include "dump.h"

static AllocatorStats stats = {};

#define BUBBLEWRAP  32  // the number of bytes around allocated block

typedef struct {
    void* addr;
    union {
        unsigned nbytes;
        void* _padding;
    };
} MemBlockInfo;

static unsigned calc_memsize(unsigned nbytes)
{
    return sizeof(MemBlockInfo) + nbytes + BUBBLEWRAP * 2;
}

static uint8_t* region_from_block(void* block)
{
    return (uint8_t*) (((ptrdiff_t) block) - sizeof(MemBlockInfo) - BUBBLEWRAP);
}

static uint8_t* block_from_region(uint8_t* region_start)
{
    return region_start + sizeof(MemBlockInfo) + BUBBLEWRAP;
}

static void check_region(const char* caller_name, void* block, unsigned nbytes)
{
    unsigned memsize = calc_memsize(nbytes);
    uint8_t* region_start = region_from_block(block);
    uint8_t* region_end  = region_start + memsize;
    uint8_t* block_start = block_from_region(region_start);
    uint8_t* block_end   = block_start + nbytes;

    unsigned num_damaged_lower = 0;
    for (unsigned i = sizeof(MemBlockInfo) + BUBBLEWRAP - 1; i >= sizeof(MemBlockInfo); i--) {
        if (region_start[i] != 0xFF) {
            num_damaged_lower++;
        }
    }

    unsigned num_damaged_upper = 0;
    for (uint8_t* p = block_end; p < region_end; p++) {
        if (*p != 0xFF) {
            num_damaged_upper++;
        }
    }

    if (num_damaged_upper || num_damaged_lower) {
        if (num_damaged_upper && num_damaged_lower) {
            fprintf(stderr, "%s: damaged %u bytes below %p and %u bytes above %u\n",
                    caller_name, num_damaged_lower, block, num_damaged_upper, nbytes);
            dump_hex_simple(stderr, region_start, BUBBLEWRAP);
            dump_hex_simple(stderr, block_end, BUBBLEWRAP);
        } else if (num_damaged_upper) {
            fprintf(stderr, "%s: damaged %u bytes above %p + %u\n",
                    caller_name, num_damaged_upper, block, nbytes);
            dump_hex_simple(stderr, block_end, BUBBLEWRAP);
        } else {
            fprintf(stderr, "%s: damaged %u bytes below %p\n",
                    caller_name, num_damaged_lower, block);
            dump_hex_simple(stderr, region_start, BUBBLEWRAP);
        }
        exit(1);
    }
}

static void* _allocate(unsigned nbytes, bool clean)
{
    unsigned memsize = calc_memsize(nbytes);

    uint8_t* region_start;
    if (clean) {
        region_start = calloc(1, memsize);
    } else {
        region_start = malloc(memsize);
    }
    if (!region_start) {
        return nullptr;
    }
    uint8_t* region_end  = region_start + memsize;
    uint8_t* block_start = region_start + sizeof(MemBlockInfo) + BUBBLEWRAP;
    uint8_t* block_end   = block_start + nbytes;

    if (region_end <= block_end) {
        fprintf(stderr, "%s: region_end %p must be greater than block_end %p\n",
                __func__, region_end, block_end);
        exit(1);
    }

    memset(region_start, 0xFF, block_start - region_start);
    memset(block_end, 0xFF, region_end - block_end);

    MemBlockInfo* info = (MemBlockInfo*) region_start;
    info->addr = block_start;
    info->nbytes = nbytes;

    atomic_fetch_add(&stats.blocks_allocated, 1);

    if (debug_allocator.verbose) {
        printf("%s: %u bytes -> %p\n", __func__, nbytes, block_start);
    }
    return block_start;
}

static void _release(void** addr_ptr, unsigned nbytes)
{
    void* addr = *addr_ptr;
    if (!addr) {
        return;
    }

    check_region(__func__, addr, nbytes);

    free(region_from_block(addr));

    if (debug_allocator.verbose) {
        fprintf(stderr, "%s: %p %u bytes\n", __func__, addr, nbytes);
    }
    atomic_fetch_sub(&stats.blocks_allocated, 1);

    *addr_ptr = nullptr;
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

    void* new_addr = _allocate(new_nbytes, false);
    if (new_addr == nullptr) {
        goto error;
    }

    memcpy(new_addr, addr, old_nbytes);
    _release(&addr, old_nbytes);

    if (clean) {
        memset(((uint8_t*) new_addr) + old_nbytes, 0, new_nbytes - old_nbytes);
    }
    *addr_ptr = new_addr;

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
    fprintf(stderr, "Debug allocator: dump is not implemented\n");
}

Allocator debug_allocator = {
    .init       = nullptr,
    .allocate   = _allocate,
    .reallocate = _reallocate,
    .release    = _release,
    .dump       = _dump,
    .trace      = false,
    .verbose    = false,
    .stats      = &stats
};
