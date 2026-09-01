#ifndef PERFECT_DARK_PS2_GS_VU1_WAIT_H
#define PERFECT_DARK_PS2_GS_VU1_WAIT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS2_GS_VU1_DMAC_STR_MASK    0x00000100u
#define PS2_GS_VU1_STAT_ACTIVE_MASK 0x1f00000fu
#define PS2_GS_VU1_STAT_ERROR_MASK  0x00003000u
#define PS2_GS_VU1_WAIT_TIMEOUT_US   100000u

enum Ps2GsVu1WaitState {
    PS2_GS_VU1_WAIT_IDLE = 0,
    PS2_GS_VU1_WAIT_ACTIVE,
    PS2_GS_VU1_WAIT_ERROR,
};

enum Ps2GsVu1WaitState ps2GsVu1ClassifyWait(
    uint32_t dmac_chcr, uint32_t vif_stat);
bool ps2GsVu1WaitTimedOut(
    uint64_t start_us, uint64_t now_us, uint64_t timeout_us);

#ifdef __cplusplus
}
#endif

#endif
