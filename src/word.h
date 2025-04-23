#pragma once

#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Architecture-specific definition of WORD type for use in bitmaps.
 */

#if (PTRDIFF_WIDTH == 32) && (UINT_WIDTH >= 32)

    typedef uint32_t Word;
#   define WORD_WIDTH  32
#   define WORD_MAX    0xFFFF'FFFF

#elif PTRDIFF_WIDTH == 64

    // on 64-bit architecture Word is configurable with WORD_WIDTH, default is 64.

#   if defined(WORD_WIDTH) && (WORD_WIDTH == 32)

        typedef uint32_t Word;
#       define WORD_WIDTH  32
#       define WORD_MAX    0xFFFF'FFFF

#   else
        typedef uint64_t Word;
#       define WORD_WIDTH  64
#       define WORD_MAX    0xFFFF'FFFF'FFFF'FFFF

#   endif

#else
#   error Cannot define architecture-specific stuff. Please revise.
#endif


#if WORD_WIDTH == 32

    static inline Word count_trailing_zeros(Word value)
    {
        //return  stdc_trailing_zeros(value);
        return  __builtin_ctz(value);
    }

#else

    static inline Word count_trailing_zeros(Word value)
    {
        //return  stdc_trailing_zeros(value);
        return  __builtin_ctzl(value);
    }

#endif


#ifdef __cplusplus
}
#endif
