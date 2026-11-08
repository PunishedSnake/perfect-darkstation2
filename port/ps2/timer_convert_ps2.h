#ifndef PERFECT_DARK_PS2_TIMER_CONVERT_PS2_H
#define PERFECT_DARK_PS2_TIMER_CONVERT_PS2_H

#include <stdint.h>

/*
 * Exact, division-light timer conversions for the R5900.
 *
 * The R5900 implements 64-bit add/shift but not 64-bit multiply or divide,
 * so PS2SDK TimerBusClock2USec() costs two libgcc __udivdi3 calls plus a
 * 64-bit multiply. These helpers return bit-identical results using shifts
 * and at most three native 32-bit DIVU instructions (one in the first
 * ~8 minutes of uptime, two thereafter). Host-tested by timer_convert_ps2_test.
 */

/* floor(x / 9) for any 64-bit x, via base-2^16 long division with 32-bit DIVU. */
static inline uint64_t ps2TimerDivide9(uint64_t x)
{
    const uint32_t hi = (uint32_t)(x >> 32);
    const uint32_t lo = (uint32_t)x;
    uint32_t q_hi = 0u;
    uint32_t r = 0u;
    if (hi != 0u) {
        q_hi = hi / 9u;
        r = hi - q_hi * 9u;
    }
    /* r < 9, so each partial dividend stays below 9 * 2^16. */
    const uint32_t m = (r << 16) | (lo >> 16);
    const uint32_t q1 = m / 9u;
    const uint32_t r1 = m - q1 * 9u;
    const uint32_t n = (r1 << 16) | (lo & 0xffffu);
    const uint32_t q0 = n / 9u;
    return ((uint64_t)q_hi << 32) | ((uint64_t)q1 << 16) | (uint64_t)q0;
}

/*
 * BUSCLK (147.456 MHz) ticks to microseconds, equal to
 * seconds * 1000000 + microseconds from TimerBusClock2USec():
 *   1e6 / 147456000 = 125 / 18432 = 125 / (2^11 * 9).
 * Exact while clocks * 125 fits in 64 bits (about 3.9 years of uptime).
 */
static inline uint64_t ps2BusClocksToMicroseconds(uint64_t clocks)
{
    /* clocks * 125 = clocks * 128 - clocks * 2 - clocks */
    const uint64_t scaled = (clocks << 7) - (clocks << 1) - clocks;
    return ps2TimerDivide9(scaled >> 11);
}

/*
 * Microseconds to the N64 46.875 MHz OS counter, equal to
 * us * 46875 / 1000 = floor(us * 375 / 8). Exact while us * 46875 fits in
 * 64 bits, the same domain as the original expression.
 */
static inline uint64_t ps2MicrosecondsToN64Count(uint64_t microseconds)
{
    /* us * 375 = us * 256 + us * 128 - us * 8 - us */
    const uint64_t scaled = (microseconds << 8) + (microseconds << 7) -
        (microseconds << 3) - microseconds;
    return scaled >> 3;
}

#endif
