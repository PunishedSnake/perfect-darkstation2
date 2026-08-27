#include "gs_command_budget.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static void test_packet_overhead_and_exact_fit(void)
{
    assert(ps2GsClassifyCommandReservation(0u, 64u, 63u) ==
        PS2_GS_COMMAND_FITS);
    assert(ps2GsClassifyCommandReservation(1u, 64u, 62u) ==
        PS2_GS_COMMAND_FITS);
    assert(ps2GsClassifyCommandReservation(1u, 64u, 63u) ==
        PS2_GS_COMMAND_SPILL);
}

static void test_spill_boundary(void)
{
    assert(ps2GsClassifyCommandReservation(60u, 64u, 3u) ==
        PS2_GS_COMMAND_FITS);
    assert(ps2GsClassifyCommandReservation(61u, 64u, 3u) ==
        PS2_GS_COMMAND_SPILL);
    assert(ps2GsClassifyCommandReservation(64u, 64u, 1u) ==
        PS2_GS_COMMAND_SPILL);
    assert(ps2GsClassifyCommandReservation(65u, 64u, 1u) ==
        PS2_GS_COMMAND_SPILL);
}

static void test_impossible_packet(void)
{
    assert(ps2GsClassifyCommandReservation(0u, 64u, 64u) ==
        PS2_GS_COMMAND_TOO_LARGE);
    assert(ps2GsClassifyCommandReservation(0u, 64u, UINT32_MAX) ==
        PS2_GS_COMMAND_TOO_LARGE);
    assert(ps2GsClassifyCommandReservation(0u, 0u, 1u) ==
        PS2_GS_COMMAND_TOO_LARGE);
    assert(ps2GsClassifyCommandReservation(0u, 64u, 0u) ==
        PS2_GS_COMMAND_TOO_LARGE);
}

int main(void)
{
    test_packet_overhead_and_exact_fit();
    test_spill_boundary();
    test_impossible_packet();
    puts("gs_command_budget tests passed");
    return 0;
}
