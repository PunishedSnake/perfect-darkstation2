#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "timer_convert_ps2.h"

#define BUSCLK 147456000ull

/* Copy of PS2SDK ee/kernel/src/timer.c TimerBusClock2USec(). */
static uint64_t referenceMicroseconds(uint64_t clocks)
{
    const uint32_t seconds = (uint32_t)(clocks / BUSCLK);
    const uint32_t micro = (uint32_t)(1000 * 1000 *
        (clocks - (((int64_t)(int32_t)((BUSCLK * (uint64_t)seconds) >> 32)
                    << 32) | (uint32_t)(BUSCLK * seconds))) / BUSCLK);
    return (uint64_t)seconds * 1000000ull + (uint64_t)micro;
}

static uint64_t nextRandom(uint64_t *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return *state;
}

int main(void)
{
    /* Divide-by-9 across the full 64-bit domain. */
    uint64_t state = 0x5044535432ull;
    for (uint32_t i = 0u; i < 2000000u; ++i) {
        const uint64_t x = nextRandom(&state) >> (i % 64u);
        assert(ps2TimerDivide9(x) == x / 9u);
    }
    const uint64_t edges[] = {
        0u, 1u, 8u, 9u, 10u, 0xffffu, 0x10000u, 0xffffffffu,
        0x100000000ull, 0x8ffffffffull, 0x900000000ull,
        UINT64_MAX, UINT64_MAX - 8u, UINT64_MAX / 9u * 9u,
    };
    for (uint32_t i = 0u; i < sizeof(edges) / sizeof(edges[0]); ++i) {
        assert(ps2TimerDivide9(edges[i]) == edges[i] / 9u);
    }

    /*
     * Bus clocks to microseconds: the first hour densely around second
     * boundaries, then random points up to ~3.9 years of uptime. The PS2SDK
     * reference itself truncates seconds to 32 bits, far beyond this range.
     */
    for (uint64_t second = 0u; second < 3600u; ++second) {
        for (int64_t delta = -3; delta <= 3; ++delta) {
            const int64_t clocks = (int64_t)(second * BUSCLK) + delta;
            if (clocks < 0) {
                continue;
            }
            assert(ps2BusClocksToMicroseconds((uint64_t)clocks) ==
                referenceMicroseconds((uint64_t)clocks));
        }
    }
    const uint64_t limit = UINT64_MAX / 125u;
    for (uint32_t i = 0u; i < 2000000u; ++i) {
        const uint64_t clocks = nextRandom(&state) % limit >> (i % 40u);
        assert(ps2BusClocksToMicroseconds(clocks) ==
            referenceMicroseconds(clocks));
    }
    for (uint64_t clocks = 0u; clocks < 3000000u; ++clocks) {
        assert(ps2BusClocksToMicroseconds(clocks) ==
            referenceMicroseconds(clocks));
    }

    /* Microseconds to the N64 OS counter, as port/src/libultra.c defines it. */
    for (uint32_t i = 0u; i < 2000000u; ++i) {
        const uint64_t us = nextRandom(&state) % (UINT64_MAX / 46875u) >>
            (i % 48u);
        assert(ps2MicrosecondsToN64Count(us) == us * 46875u / 1000u);
    }
    for (uint64_t us = 0u; us < 1000000u; ++us) {
        assert(ps2MicrosecondsToN64Count(us) == us * 46875u / 1000u);
    }

    puts("PS2 timer conversion tests passed");
    return 0;
}
