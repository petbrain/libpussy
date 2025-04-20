#pragma once

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

void dump_bitmap(FILE* fp, uint8_t* data, unsigned size);

void dump_hex(FILE* fp, unsigned indent, uint8_t* addr, unsigned size, uint8_t* display_addr, bool aligned, bool with_chars);
void dump_hex_simple(FILE* fp, uint8_t* data, unsigned size);

#ifdef __cplusplus
}
#endif
