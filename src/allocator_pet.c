#ifndef _GNU_SOURCE
#   define _GNU_SOURCE
#endif

#include <errno.h>
#include <stdarg.h>
//#include <stdbit.h> not in libc yet, using __builtin_* functions
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <sys/mman.h>

#include "allocator.h"
#include "dump.h"
#include "src/word.h"

// unit size should not be less than size of pointer
#define UNIT_SIZE  16

// threads serialization
static mtx_t lock;

/****************************************************************
 * Trace/debug output
 */

static void print_msg(const char* func_name, char* fmt, ...)
{
    fprintf(stderr, "Bitmap allocator -- %s: ", func_name);
    va_list ap;
    va_start(ap);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

#define ERR(...)  print_msg(__func__, __VA_ARGS__)

#define SAY(...)  do { if (pet_allocator.verbose) { print_msg(__func__, __VA_ARGS__); } } while(false)

#ifdef DEBUG
#   define TRACE(...)  do { if (pet_allocator.trace) { print_msg(__func__, __VA_ARGS__); } } while(false)
#else
#   define TRACE(...)
#endif


/****************************************************************
 * Stats
 */

static AllocatorStats stats = {};

static atomic_size_t num_bm_pages = 0;

/****************************************************************
 * memory cleaning
 */

static void cleanse(void* addr, unsigned start, unsigned end)
{
    TRACE("addr=%p, start=%u, end=%u\n", addr, start, end);

    unsigned length = end - start;

    // clean bytes till start of word
    uint8_t* byteptr = addr;
    byteptr += start;
    unsigned nbytes = start & (sizeof(Word) - 1);
    if (nbytes) {
        nbytes = sizeof(Word) - nbytes;
        if (nbytes > length) {
            nbytes = length;
        }
        TRACE("leading %u bytes\n", nbytes);
        for (unsigned i = 0; i < nbytes; i++) {
            *byteptr++ = 0;
            length--;
        }
    }

    // clean words
    TRACE("%u words\n", length / sizeof(Word));
    Word* wordptr = (Word*) byteptr;
    while (length >= sizeof(Word)) {
        *wordptr++ = 0;
        length -= sizeof(Word);
    }

    if (length) {
        // clean remaining bytes
        TRACE("remaining %u bytes\n", length);
        byteptr = (uint8_t*) wordptr;
        while (length--) {
            *byteptr++ = 0;
        }
    }
}

/****************************************************************
 * mmap/mremap/munmap wrappers
 */

static void* call_mmap(unsigned size, bool clean)
/*
 * call mmap to allocate pages
 *
 * size must be multiple of sys_page_size
 *
 * bear in mind mmap called immediately after munmap may return dirty page,
 * so explicit cleaning is a must
 */
{
    void* result = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (result == MAP_FAILED) {
        ERR("mmap: %s\n", strerror(errno));
        return nullptr;
    }
    if (clean) {
        cleanse(result, 0, size);
    }
    return result;
}

static inline void call_munmap(void* addr, unsigned size)
{
    if (munmap(addr, size) == -1) {
        ERR("munmap(%p, %u): %s\n", addr, size, strerror(errno));
    }
}

static void* call_mremap(void* addr, unsigned old_nbytes, unsigned new_nbytes, bool clean)
/*
 * old/new_nbytes are unaligned
 */
{
    unsigned old_size = align_unsigned_to_page(old_nbytes);
    unsigned new_size = align_unsigned_to_page(new_nbytes);
    if (new_size == old_size) {
        if (clean && new_nbytes > old_nbytes) {
            cleanse(addr, old_nbytes, new_nbytes);
        }
        return addr;
    }
    int flags;
    if (new_size > old_size) {
        flags = MREMAP_MAYMOVE;
    } else {
        flags = 0;
        clean = false;  // don't clean when shrinking
    }
    void* new_addr = mremap(addr, old_size, new_size, flags);
    if (new_addr == MAP_FAILED) {
        ERR("mremap(%p, %u, %u): %s\n", addr, old_size, new_size, strerror(errno));
        if (new_size > old_size) {
            // grow failed
            return nullptr;
        } else {
            // shrink failed, return same address
            return addr;
        }
    }
    if (clean) {
        cleanse(new_addr, old_nbytes, new_nbytes);
    }
    return new_addr;
}

/****************************************************************
 * bitmap allocator parameters
 */

static unsigned units_per_page;  // sys_page_size / UNIT_SIZE

static unsigned bm_page_header_size_in_units; // num_reserved_units ???
/*
 * Calculated as: (offsetof(BmPageHeader, bitmap)
 *                 + units_per_page / 8  // size of bitmap in bytes
 *                 + UNIT_SIZE - 1       // rounding
 *                ) / UNIT_SIZE
 */

static unsigned max_data_units;  // sys_page_size - bm_page_header_size_in_units


static inline unsigned bytes_to_units(unsigned nbytes)
{
    return align_unsigned(nbytes, UNIT_SIZE) / UNIT_SIZE;
}

/****************************************************************
 * Bitmap allocator data page and superblock
 */

typedef struct _BmPageHeader {
    /*
     * On 4K page the header takes four 16-byte units, leaving 4032 bytes for data.
     */
    struct _BmPageHeader** volatile list;
    struct _BmPageHeader* next;
    struct _BmPageHeader* prev;

    // variable part

    // the size of bitmap depends on page size, for 4K it takes 32 bytes
    Word bitmap[ /* sys_page_size / UNIT_SIZE / WORD_WIDTH */ ];

} BmPageHeader;


static BmPageHeader* volatile lru_page = nullptr;
/*
 * last recently used page.
 *
 * Technically, this is a list, and functions that grab a page
 * do not make a difference between this variable and superblock entry.
 * However, LRU may contain single item only.
 */

static BmPageHeader** volatile superblock;
/*
 * Straightforward definition would be:
 *
 * BmPageHeader* superblock[ units_per_page ];
 *
 * The array contains pointers to bm_page lists grouped by their
 * longest free block.
 *
 * Superblock cannot take more than one page.
 * Usually it takes a half, if UNIT_SIZE is twice longer than size of pointer.
 */

static void dump_bm_page(BmPageHeader* bm_page)
{
    fprintf(stderr, "Page %p: list=%p, next=%p, prev=%p\n",
            (void*) bm_page, (void*) bm_page->list, (void*) bm_page->next, (void*) bm_page->prev);
    dump_bitmap(stderr, (uint8_t*)(bm_page->bitmap), units_per_page / 8);
}

static void dump()
{
    BmPageHeader** list = superblock;
    fprintf(stderr, "\nAllocator bm pages: %zu, blocks allocated %zu\n",
            num_bm_pages, stats.blocks_allocated);
    if (lru_page) {
        fprintf(stderr, "LRU page: %p\n", (void*) lru_page);
        dump_bm_page(lru_page);
    }
    for (unsigned i = 0; i < units_per_page; i++, list++) {
        BmPageHeader* first_page = *list;
        if (first_page) {
            fprintf(stderr, "Superblock entry %u: %p -> %p\n", i, (void*) list, (void*) first_page);
            BmPageHeader* bm_page = first_page;
            do {
                dump_bm_page(bm_page);
                bm_page = bm_page->next;
            } while (bm_page != first_page);
        }
    }
    fputc('\n', stderr);
}

/****************************************************************
 * Basic bitmap functions
 */

static unsigned count_zero_bits(BmPageHeader* bm_page, unsigned offset, unsigned limit)
/*
 * Count consecutive zero bits in the bitmap starting from `offset` bit
 * up to `limit`. The limit is treated as a hint when to stop, returned count can be greater.
 */
{
    unsigned count = 0;
    Word* ptr = &bm_page->bitmap[offset / WORD_WIDTH];

    // count starting bits up to the the next word boundary
    unsigned bit_index = offset & (WORD_WIDTH - 1);
    if (bit_index) {
        Word w = *ptr++;
        w >>= bit_index;
        if (w) {
            // we have only ending bits
            return count_trailing_zeros(w);
        }
        count = WORD_WIDTH - bit_index;
        offset += count;
    }

    // count zero words
    while (offset < units_per_page && count < limit) {
        Word w = *ptr++;
        if (w) {
            // count ending bits
            count += count_trailing_zeros(w);
            break;
        }
        count += WORD_WIDTH;
        offset += WORD_WIDTH;
    }
    return count;
}

static unsigned count_nonzero_bits(BmPageHeader* bm_page, unsigned offset, unsigned limit)
/*
 * Count consecutive nonzero bits in the bitmap starting from `offset` bit
 * up to `limit`. The limit is treated as a hint when to stop, returned count can be greater.
 *
 * The code is exactly the same as in count_zero_bits, the only difference is inversion.
 */
{
    unsigned count = 0;
    Word* ptr = &bm_page->bitmap[offset / WORD_WIDTH];

    // count starting bits up to the the next word boundary
    unsigned bit_index = offset & (WORD_WIDTH - 1);
    if (bit_index) {
        Word w = ~*ptr++;
        w >>= bit_index;
        if (w) {
            // we have only ending bits
            return count_trailing_zeros(w);
        }
        count = WORD_WIDTH - bit_index;
        offset += count;
    }

    // count all-one words
    while (offset < units_per_page && count < limit) {
        Word w = ~*ptr++;
        if (w) {
            // count ending bits
            count += count_trailing_zeros(w);
            break;
        }
        count += WORD_WIDTH;
        offset += WORD_WIDTH;
    }
    return count;
}

static void set_bits(BmPageHeader* bm_page, unsigned offset, unsigned length)
/*
 * Set bits in the bitmap starting from offset.
 */
{
    TRACE("bm_page=%p offset=%u length=%u\n", (void*) bm_page, offset, length);
    Word* ptr = &bm_page->bitmap[offset / WORD_WIDTH];

    // set starting bits up to the the next word boundary
    unsigned bit_index = offset & (WORD_WIDTH - 1);
    if (bit_index) {
        Word bitmask = WORD_MAX;
        unsigned num_bits = WORD_WIDTH - bit_index;
        if (length <= num_bits) {
            bitmask &= (((Word) 1) << length) - 1;
            num_bits = length;
        }
        bitmask <<= bit_index;
        *ptr++ |= bitmask;
        offset += num_bits;
        length -= num_bits;
    }

    // set remaining words
    while (length >= WORD_WIDTH) {
        *ptr++ = WORD_MAX;
        length -= WORD_WIDTH;
    }

    // set ending bits
    if (length) {
        *ptr++ |= (((Word) 1) << length) - 1;
    }
}

static void clear_bits(BmPageHeader* bm_page, unsigned offset, unsigned length)
/*
 * Clear bits in the bitmap starting from offset.
 *
 * The logic is the same as in set_bits.
 */
{
    TRACE("bm_page=%p offset=%u length=%u\n", (void*) bm_page, offset, length);
    Word* ptr = &bm_page->bitmap[offset / WORD_WIDTH];

    // clear starting bits up to the the next word boundary
    unsigned bit_index = offset & (WORD_WIDTH - 1);
    if (bit_index) {
        Word bitmask = WORD_MAX;
        unsigned num_bits = WORD_WIDTH - bit_index;
        if (length <= num_bits) {
            bitmask &= (((Word) 1) << length) - 1;
            num_bits = length;
        }
        bitmask <<= bit_index;
        *ptr++ &= ~bitmask;
        offset += num_bits;
        length -= num_bits;
    }

    // clear remaining words
    while (length >= WORD_WIDTH) {
        *ptr++ = 0;
        length -= WORD_WIDTH;
    }

    // clear ending bits
    if (length) {
        *ptr++ &= ~((((Word) 1) << length) - 1);
    }
}

/****************************************************************
 * Bitmap allocator functions
 */

static unsigned find_free_block(BmPageHeader* bm_page, unsigned block_size)
/*
 * Search for free block.
 * Return offset of the first available block or 0 if no block is found.
 * Given that first units of bm_page are always in use,
 * offset can never be zero on success.
 */
{
    unsigned offset = bm_page_header_size_in_units;
    while (offset < units_per_page) {
        unsigned length = count_zero_bits(bm_page, offset, block_size);
        if (length >= block_size) {
            TRACE("bm_page=%p block_size=%u -> offset=%u\n", (void*) bm_page, block_size, offset);
            return offset;
        }
        offset += length;
        offset += count_nonzero_bits(bm_page, offset, UINT_MAX);
    }
    TRACE("bm_page=%p block_size=%u -> 0\n", (void*) bm_page, block_size);
    return 0;
}

static unsigned find_longest_free_block(BmPageHeader* bm_page)
/*
 * Search for the longest sequence of zero bits and return its length.
 */
{
    unsigned offset = bm_page_header_size_in_units;
    unsigned n = max_data_units;
    unsigned lfb = 0;
    while (n) {
        unsigned length = count_zero_bits(bm_page, offset, n);
        if (length > lfb) {
            lfb = length;
        }
        offset += length;
        n -= length;

        length = count_nonzero_bits(bm_page, offset, n);
        offset += length;
        n -= length;
    }
    TRACE("bm_page=%p -> lfb=%u\n", (void*) bm_page, lfb);
    return lfb;
}

static void add_to_list(BmPageHeader** list, BmPageHeader* bm_page)
/*
 * Add bm_page to circular doubly-linked list.
 */
{
    BmPageHeader* first = *list;
    if (first) {
        // add to the end of list
        bm_page->prev = first->prev;
        bm_page->next = first;
        first->prev->next = bm_page;
        first->prev = bm_page;
    } else {
        // init list
        *list = bm_page->next = bm_page->prev = bm_page;
    }
    bm_page->list = list;
}

static void delete_from_list(BmPageHeader* bm_page)
/*
 * Delete bm_page from circular doubly-linked list.
 */
{
    BmPageHeader** list = bm_page->list;

#   ifdef DEBUG
        if (!list) {
            ERR("double call delete_from_list(%p)\n", (void*) bm_page);
            abort();
        }
        if (list == &lru_page) {
            TRACE("deleting page %p from LRU\n", (void*) bm_page);
        } else {
            TRACE("deleting page %p from superblock[%tu]\n", (void*) bm_page, list - superblock);
        }
#   endif

    if (bm_page->next == bm_page->prev) {
        // last page, make list empty
        *list = nullptr;
    } else {
        if (*list == bm_page) {
            *list = bm_page->next;
        }
        bm_page->next->prev = bm_page->prev;
        bm_page->prev->next = bm_page->next;
    }
    bm_page->list = nullptr;
}

static void add_to_superblock_entry(BmPageHeader* bm_page, unsigned lfb)
/*
 * Add page to superblock entry by `lfb`.
 */
{
    mtx_lock(&lock);
    TRACE("adding page %p to superblock[%tu]\n", (void*) bm_page, lfb);
    add_to_list(&superblock[lfb], bm_page);
    mtx_unlock(&lock);
}

static inline void unhand_page(BmPageHeader* bm_page)
/*
 * If `lru_page` is set, move it to superblock.
 * Assign `bm_page` to `lru_page`.
 */
{
    BmPageHeader* page_to_reclaim = nullptr;

    mtx_lock(&lock);
    if (lru_page) {
        // have to scan while lock is held
        unsigned lfb = find_longest_free_block(lru_page);

        if (lfb < max_data_units) {
            TRACE("adding lru_page %p to superblock[%tu]\n", (void*) lru_page, lfb);
            add_to_list(&superblock[lfb], lru_page);
        } else {
            // okay to reclaim this page
            page_to_reclaim = lru_page;
        }
    }
    // now set lru_page
    TRACE("adding bm_page %p to LRU\n", (void*) bm_page);
    bm_page->list = (BmPageHeader** /* make compiller happy with volatile */) &lru_page;
    lru_page = bm_page->next = bm_page->prev = bm_page;
    mtx_unlock(&lock);

    if (page_to_reclaim) {
        TRACE("releasing page %p\n", (void*) page_to_reclaim);
        call_munmap(page_to_reclaim, sys_page_size);
        atomic_fetch_sub(&num_bm_pages, 1);
    }
}

static inline BmPageHeader* bm_page_by_addr(void* addr)
/*
 * Get address of the bm_page from `addr`
 */
{
    return (BmPageHeader*) (
        ((ptrdiff_t) addr) & ~((ptrdiff_t) sys_page_size - 1)
    );
}

static void grab_page(BmPageHeader* bm_page)
/*
 * Prepare page for reallocating units
 * in the context of current thread.
 * The page is obtained by address using `bm_page_by_addr`.
 *
 * Take the page out of whatever list it belongs,
 * either superblock or LRU.
 */
{
#   if DEBUG
        unsigned msg_interval = 0;
#   endif

    for (;;) {
        mtx_lock(&lock);
        if (bm_page->list) {
            // the page belongs to some list
            break;
        }
        // page is probably in use by other thread
#       if DEBUG
            if (msg_interval == 0) {
                TRACE("waiting for page %p to be released\n", (void*) bm_page);
                msg_interval = 1000;
            } else {
                msg_interval--;
            }
#       endif
        mtx_unlock(&lock);
        thrd_yield();
    }
#   if DEBUG
        if (bm_page->list == &lru_page) {
            TRACE("taking page %p out of LRU\n", (void*) bm_page);
        } else {
            TRACE("taking page %p out of superblock[%tu]\n", (void*) bm_page, bm_page->list - superblock);
        }
#   endif
    delete_from_list(bm_page);
    mtx_unlock(&lock);
}

static inline unsigned ptrdiff_to_units(void* addr, BmPageHeader* bm_page)
// helper function for bm_shrink and bm_release invocation
{
    return (
        ((uint8_t*) addr) - ((uint8_t*) bm_page)
    ) / UNIT_SIZE;
}

#ifdef DEBUG
    static void check_units_allocated(const char* func, BmPageHeader* bm_page,
                                      unsigned offset, unsigned num_units)
    {
        unsigned n = count_nonzero_bits(bm_page, offset, num_units);
        if (n < num_units) {
            print_msg(func, "already released some units on bm_page %p starting from %u: in use %u of %u\n",
                      (void*) bm_page, offset, n, num_units);
        }
    }
#endif

static BmPageHeader* find_available_page(unsigned num_units, unsigned* offset)
/*
 * Find available page for new allocation.
 *
 * First, get LRU page. If missing, search superblock lists.
 *
 * When found, the page is removed either form LRU or from the superblock,
 * so only current thread continue working with it while other threads
 * can work with their own pages in parallel.
 */
{
    BmPageHeader* bm_page = nullptr;

    mtx_lock(&lock);

    if (lru_page) {
        bm_page = lru_page;
        TRACE("taking page %p out of LRU\n", (void*) bm_page);
        delete_from_list(bm_page);
        mtx_unlock(&lock);

        // find free block on the LRU page
        *offset = find_free_block(bm_page, num_units);
        if (*offset) {
            return bm_page;
        }
        // LRU page has no space available, move it to superblock
        add_to_superblock_entry(bm_page, find_longest_free_block(bm_page));

        // continue with superblock
        mtx_lock(&lock);
    }
    // XXX optimize for speed with bitmap?
    // start searching from num_units position
    BmPageHeader** list = &superblock[num_units];
    unsigned lfb = num_units;
    for (; lfb <= max_data_units; lfb++) {
        bm_page = *list++;
        if (bm_page) {
            TRACE("taking page %p out of superblock[%tu]\n", (void*) bm_page, bm_page->list - superblock);
            delete_from_list(bm_page);
            goto found;
        }
    }
    mtx_unlock(&lock);
    return nullptr;

found:
    mtx_unlock(&lock);
    *offset = find_free_block(bm_page, num_units);
    if (*offset == 0) {
        ERR("bm_page %p with LFB=%u must contain enough free space for %u units\n",
            (void*) bm_page, bm_page->list - superblock, num_units);
        abort();
    }
    return bm_page;
}

static void* bm_allocate(unsigned num_units, bool clean)
/*
 * Bitmap sub-allocator, should be called with num_units < max_data_units
 */
{
    TRACE("num_units %u\n", num_units);

    void* result = nullptr;
    unsigned offset;
    BmPageHeader* bm_page = find_available_page(num_units, &offset);
    if (bm_page) {
        // allocate
        set_bits(bm_page, offset, num_units);
        unhand_page(bm_page);
        result = ((uint8_t*) bm_page) + offset * UNIT_SIZE;
        goto out;
    }

    TRACE("allocating new page\n");

    bm_page = call_mmap(sys_page_size, false);
    if (!bm_page) {
        goto out;
    }
    // clean bitmap
    Word* ptr = bm_page->bitmap;
    for (unsigned i = 0, n = units_per_page / WORD_WIDTH; i < n; i++) {
        *ptr++ = 0;
    }
    // mark reserved units and allocate units
    set_bits(bm_page, 0, bm_page_header_size_in_units + num_units);

    // give page away to LRU or superblock
    unhand_page(bm_page);

    atomic_fetch_add(&num_bm_pages, 1);
    result = ((uint8_t*) bm_page) + bm_page_header_size_in_units * UNIT_SIZE;

out:
    if (result) {
        atomic_fetch_add(&stats.blocks_allocated, 1);
    }

    if (result && clean) {
        cleanse(result, 0, num_units * UNIT_SIZE);
    }
    TRACE("result=%p\n", result);
    return result;
}

static void bm_shrink(BmPageHeader* bm_page, unsigned offset, unsigned old_num_units, unsigned new_num_units)
{
    TRACE("bm_page=%p, offset=%u, old_num_units=%u, new_num_units=%u\n",
          (void*) bm_page, offset, old_num_units, new_num_units);

    grab_page(bm_page);

    unsigned tail_units = old_num_units - new_num_units;

#   ifdef DEBUG
        check_units_allocated(__func__, bm_page, offset + new_num_units, tail_units);
#   endif
    clear_bits(bm_page, offset + new_num_units, tail_units);

    unhand_page(bm_page);
}

static bool bm_grow(BmPageHeader* bm_page, unsigned offset, unsigned old_num_units, unsigned new_num_units)
{
    TRACE("bm_page=%p, offset=%u, old_num_units=%u, new_num_units=%u\n",
          (void*) bm_page, offset, old_num_units, new_num_units);

    grab_page(bm_page);

    unsigned increment = new_num_units - old_num_units;
    unsigned length = count_zero_bits(bm_page, offset + old_num_units, increment);
    if (length < increment) {
        TRACE("available length %u is less than increment %u; need to move\n", length, increment);
        unhand_page(bm_page);
        return false;
    }
    set_bits(bm_page, offset + old_num_units, increment);

    unhand_page(bm_page);
    return true;
}

static void bm_release(BmPageHeader* bm_page, unsigned offset, unsigned num_units)
{
    TRACE("bm_page=%p, offset=%u, num_units=%u\n", (void*) bm_page, offset, num_units);

    grab_page(bm_page);

#   ifdef DEBUG
        check_units_allocated(__func__, bm_page, offset, num_units);
#   endif
    clear_bits(bm_page, offset, num_units);

    unhand_page(bm_page);
    atomic_fetch_sub(&stats.blocks_allocated, 1);
}

/****************************************************************
 * Allocator interface functions
 */

static void _init()
{
    // init page parameters

    units_per_page = sys_page_size / UNIT_SIZE;

    bm_page_header_size_in_units = (
        offsetof(BmPageHeader, bitmap)
        + units_per_page / 8  // size of bitmap in bytes
        + UNIT_SIZE - 1       // rounding
    ) / UNIT_SIZE;

    max_data_units = units_per_page - bm_page_header_size_in_units;

    // allocate superblock

    superblock = call_mmap(sys_page_size, true);
    if (!superblock) {
        abort();
    }

    // init mutex
    if (mtx_init(&lock, mtx_plain) != thrd_success) {
        ERR("cannot init mutex\n");
    }

    SAY("page size %u; units per page: %u; header: %u units; data units: %u (%u bytes)\n",
        sys_page_size, units_per_page, bm_page_header_size_in_units, max_data_units, max_data_units * UNIT_SIZE);
}


static void* _allocate(unsigned nbytes, bool clean)
{
    TRACE("nbytes=%u\n", nbytes);

    if (nbytes == 0) {
        return nullptr;
    }
    unsigned num_units = bytes_to_units(nbytes);
    if (num_units < max_data_units) {
        // use bitmap sub-allocator for smaller blocks
        return bm_allocate(num_units, clean);
    } else {
        // allocate pages directly
        void* result = call_mmap(align_unsigned_to_page(nbytes), clean);
        if (result) {
            atomic_fetch_add(&stats.blocks_allocated, 1);
        }
        return result;
    }
}

static void _release(void** addr_ptr, unsigned nbytes)
{
    void* addr = *addr_ptr;
    if (!addr) {
        return;
    }

    TRACE("addr=%p nbytes=%u\n", addr, nbytes);

    if (nbytes == 0) {
        ERR("called for %p with zero nbytes\n", addr);
        abort();
    }

    BmPageHeader* bm_page = bm_page_by_addr(addr);
    if (addr == (void*) bm_page) {
        /*
         * addr is aligned on page boundary, this means
         * the block was allocated directly with mmap
         */
        call_munmap(addr, align_unsigned_to_page(nbytes));
        atomic_fetch_sub(&stats.blocks_allocated, 1);

    } else {
        // use bitmap sub-allocator for smaller blocks
        bm_release(bm_page, ptrdiff_to_units(addr, bm_page), bytes_to_units(nbytes));
    }
    *addr_ptr = nullptr;
}

static bool _reallocate(void** addr_ptr, unsigned old_nbytes, unsigned new_nbytes, bool clean, bool* addr_changed)
{
    if (old_nbytes == new_nbytes) {
        goto success_same_addr;
    }

    void* addr = *addr_ptr;

    TRACE("addr=%p old_nbytes=%u new_nbytes=%u\n", addr, old_nbytes, new_nbytes);

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

    if (!(old_nbytes | new_nbytes)) {
        // XXX might be a serious error, but it's a caller's problem
        if (!old_nbytes) { ERR("called for %p with zero old_nbytes\n", addr); }
        if (!new_nbytes) { ERR("called for %p with zero new_nbytes\n", addr); }
        goto error;
    }

    unsigned new_num_units = bytes_to_units(new_nbytes);
    unsigned old_num_units = bytes_to_units(old_nbytes);

    if (new_num_units == old_num_units) {
        if (clean && new_nbytes > old_nbytes) {
            cleanse(addr, old_nbytes, new_nbytes);
        }
        goto success_same_addr;
    }

    BmPageHeader* bm_page = bm_page_by_addr(addr);

    // shall we shrink?
    if (new_num_units < old_num_units) {

        if (new_num_units < max_data_units) {

            // new block will use bitmap sub-allocator

            if (old_num_units < max_data_units) {
                // shrink using bitmap sub-allocator
                if (addr == (void*) bm_page) {
                    ERR("address %p is not within data area\n", addr);
                    abort();
                }
                bm_shrink(bm_page, ptrdiff_to_units(addr, bm_page), old_num_units, new_num_units);
                goto success_same_addr;
            }

            // shrinking block from page allocator to bitmap sub-allocator

            if (addr != (void*) bm_page) {
                ERR("address %p is not aligned on page boundary\n", addr);
                abort();
            }
            void* new_block = bm_allocate(new_num_units, false);
            if (!new_block) {
                TRACE("falling back to remap\n");
                goto remap;
            }
            memcpy(new_block, addr, new_nbytes);
            bm_release(bm_page, ptrdiff_to_units(addr, bm_page), new_num_units);
            *addr_ptr = new_block;
            goto success_changed_addr;

        } else {
            // shrink using mremap
            if (addr != (void*) bm_page) {
                ERR("address %p is not aligned on page boundary\n", addr);
                abort();
            }
    remap:
            call_mremap(addr, old_nbytes, new_nbytes, false);
            goto success_same_addr;
        }
    }

    // grow

    if (old_num_units < max_data_units) {

        if (new_num_units < max_data_units) {

            // grow using bitmap sub-allocator

            if (addr == (void*) bm_page) {
                ERR("address %p is not within data area\n", addr);
                abort();
            }
            // try to grow within the same page
            if(bm_grow(bm_page, ptrdiff_to_units(addr, bm_page), old_num_units, new_num_units)) {
                if (clean) {
                    cleanse(addr, old_nbytes, new_nbytes);
                }
                goto success_same_addr;
            }
        }

        // reallocate block

        void* new_block = _allocate(new_nbytes, false);
        if (!new_block) {
            goto error;
        }
        memcpy(new_block, addr, old_nbytes);
        _release(&addr, old_nbytes);
        if (clean) {
            cleanse(new_block, old_nbytes, new_nbytes);
        }
        *addr_ptr = new_block;
        if (addr_changed) { *addr_changed = new_block != addr; }
        return true;

    } else {
        // grow using mremap
        if (addr != (void*) bm_page) {
            ERR("address %p is not aligned on page boundary\n", addr);
            abort();
        }
        void* new_addr = call_mremap(addr, old_nbytes, new_nbytes, clean);
        if (!new_addr) {
            goto error;
        }
        *addr_ptr = new_addr;
        if (addr_changed) { *addr_changed = new_addr != addr; }
        return true;
    }

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

Allocator pet_allocator = {
    .init       = _init,
    .allocate   = _allocate,
    .reallocate = _reallocate,
    .release    = _release,
    .dump       = dump,
    .trace      = false,
    .verbose    = false,
    .stats      = &stats
};
