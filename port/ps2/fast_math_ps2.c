#include <math.h>

/*
 * Perfect Dark's original N64 build supplies sqrtf as one hardware SQRT.S
 * instruction (src/lib/ultra/gu/sqrtf.s).  The PS2 CMake frontier does not
 * assemble that N64 source, which otherwise lets Newlib satisfy hundreds of
 * game-side sqrtf calls with its much wider ISO/POSIX contract.
 *
 * R5900 COP1 has the same single-precision primitive and the game already
 * expects the hardware contract: callers use squared lengths, distances and
 * other non-negative finite inputs.  Keep this symbol deliberately tiny and
 * separate from global fast-math flags so NaN, signed-zero and reassociation
 * semantics in the rest of the renderer remain untouched.
 */
float sqrtf(float value)
{
    float result;

    __asm__ volatile (
        "sqrt.s %0, %1"
        : "=f" (result)
        : "f" (value));

    return result;
}
