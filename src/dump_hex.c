#include <limits.h>
//#include <stdbit.h> not in libc yet, using __builtin_* functions
#include <stddef.h>
#include <string.h>

#include "dump.h"

static inline unsigned first_leading_one(size_t value)
{
    static_assert(sizeof(size_t) == sizeof(unsigned long));
    //return stdc_first_leading_one(value);
    return ULONG_WIDTH - __builtin_clzl(value) - 1;
}

static void print_indent(FILE* fp, int indent)
{
    for (int i = 0; i < indent; i++ ) {
        fputc(' ', fp);
    }
}

static char hexdigits[] = "0123456789ABCDEF";

static void print_addr(FILE* fp, uint8_t* addr, unsigned addr_width)
{
    unsigned shift = addr_width << 2;
    for (unsigned i = 0; i < addr_width; i++) {
        shift -= 4;
        fputc(hexdigits[(((ptrdiff_t) addr) >> shift) & 15], fp);
    }
    fputc(':', fp);
    fputc(' ', fp);
}

static void print_hex(FILE* fp, uint8_t data)
{
    fputc(hexdigits[data >> 4], fp);
    fputc(hexdigits[data & 15], fp);
}

static void print_row(FILE* fp, uint8_t* addr, uint8_t* display_addr, unsigned addr_width, bool with_chars)
{
    print_addr(fp, display_addr, addr_width);
    for(unsigned i = 0; i < 16; i++) {
        if (i == 8) {
            fputs("- ", fp);
        }
        print_hex(fp, addr[i]);
        fputc(' ', fp);
    }
    if (with_chars) {
        for(unsigned i = 0; i < 16; i++) {
            uint8_t c = addr[i];
            if (c < 32 || c > 127) {
                c = '.';
            }
            fputc(c, fp);
        }
    }
    fputc('\n', fp);
}

static void print_same_rows(FILE* fp, unsigned indent, unsigned num_same_rows,
                            uint8_t* row, uint8_t* display_addr, unsigned addr_width, bool with_chars)
{
    if (num_same_rows > 3) {
        print_indent(fp, indent);
        fprintf(fp, "-- %u same rows --\n", num_same_rows - 1);
        print_indent(fp, indent);
        print_row(fp, row, display_addr - 16, addr_width, with_chars);
    } else if (num_same_rows) {
        do {
            print_indent(fp, indent);
            print_row(fp, row, display_addr - (16 * num_same_rows), addr_width, with_chars);
        } while (num_same_rows--);
    }
}

void dump_hex(FILE* fp, unsigned indent, uint8_t* addr, unsigned size, uint8_t* display_addr, bool aligned, bool with_chars)
{
//fprintf(fp, "DUMP: addr=%p, size=%u, display_addr=%p, ", addr, size, display_addr);
    unsigned offset;
    if (aligned) {
        offset = (unsigned) (((size_t) addr) & 15);
        addr -= offset;
        display_addr -= offset;
        size += offset;
    } else {
        offset = 0;
    }
//fprintf(fp, "offset=%u, remainder=%u, size=%u\n", offset, size & 15, size - (size & 15));
    uint8_t* max_addr = display_addr + size;
    unsigned addr_width = (first_leading_one((size_t) max_addr) + 3) >> 2;
    if (addr_width < 4) {
        addr_width = 4;
    }

    unsigned i = 0;

    if (offset) {
        // print row with blank leading and trailing bytes
        print_indent(fp, indent);
        print_addr(fp, display_addr, addr_width);
        unsigned sz = (size < 16)? size : 16;
        unsigned j = 0;
        for(; j < offset; j++) {
            if (j == 8) {
                fputs("  ", fp);
            }
            fputs("   ", fp);
        }
        for(; j < sz; j++) {
            if (j == 8) {
                fputs("- ", fp);
            }
            print_hex(fp, addr[j]);
            fputc(' ', fp);
        }
        for(; j < 16; j++) {
            if (j == 8) {
                fputs("  ", fp);
            }
            fputs("   ", fp);
        }
        if (with_chars) {
            fputc(' ', fp);
            for(j = 0; j < offset; j++) {
                fputc(' ', fp);
            }
            for(; j < sz; j++) {
                uint8_t c = addr[j];
                if (c < 32 || c > 127) {
                    c = '.';
                }
                fputc(c, fp);
            }
        }
        fputc('\n', fp);
        if (size < 16) {
            return;
        }
        i += 16;
        addr += 16;
        display_addr += 16;
    }

    unsigned remainder = size & 15;
    size -= remainder;

    // print full rows
    unsigned num_rows = 0;
    unsigned num_same_rows = 0;
    uint8_t prev_row[16];
    while (i < size) {
        if (num_rows) {
            // coalesce duplicate rows
            if (memcmp(addr, prev_row, 16) == 0) {
                num_same_rows++;
                goto _continue;
            }
            print_same_rows(fp, indent, num_same_rows, prev_row, display_addr, addr_width, with_chars);
            num_same_rows = 0;
        }
        print_indent(fp, indent);
        print_row(fp, addr, display_addr, addr_width, with_chars);
        memcpy(prev_row, addr, 16);

_continue:
        i += 16;
        addr += 16;
        display_addr += 16;
        num_rows++;
    }
    print_same_rows(fp, indent, num_same_rows, prev_row, display_addr, addr_width, with_chars);

    // print last incomplete row
    if (remainder) {
        print_indent(fp, indent);
        print_addr(fp, display_addr, addr_width);
        unsigned j = 0;
        for(; j < remainder; j++) {
            if (j == 8) {
                fputs("- ", fp);
            }
            print_hex(fp, addr[j]);
            fputc(' ', fp);
        }
        for(; j < 16; j++) {
            if (j == 8) {
                fputs("  ", fp);
            }
            fputs("   ", fp);
        }
        if (with_chars) {
            fputc(' ', fp);
            for(j = 0; j < remainder; j++) {
                uint8_t c = addr[j];
                if (c < 32 || c > 127) {
                    c = '.';
                }
                fputc(c, fp);
            }
        }
        fputc('\n', fp);
    }
}

void dump_hex_simple(FILE* fp, uint8_t* data, unsigned size)
{
    dump_hex(fp, 0, data, size, data, true, true);
}
