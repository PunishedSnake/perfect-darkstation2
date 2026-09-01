#include "gs_vu1_wait.h"

extern "C" enum Ps2GsVu1WaitState ps2GsVu1ClassifyWait(
    uint32_t dmac_chcr, uint32_t vif_stat)
{
    if ((vif_stat & PS2_GS_VU1_STAT_ERROR_MASK) != 0u) {
        return PS2_GS_VU1_WAIT_ERROR;
    }
    if ((dmac_chcr & PS2_GS_VU1_DMAC_STR_MASK) != 0u ||
        (vif_stat & PS2_GS_VU1_STAT_ACTIVE_MASK) != 0u) {
        return PS2_GS_VU1_WAIT_ACTIVE;
    }
    return PS2_GS_VU1_WAIT_IDLE;
}

extern "C" bool ps2GsVu1WaitTimedOut(
    uint64_t start_us, uint64_t now_us, uint64_t timeout_us)
{
    if (timeout_us == 0u) {
        return false;
    }
    if (now_us < start_us) {
        return true;
    }
    return now_us - start_us >= timeout_us;
}
