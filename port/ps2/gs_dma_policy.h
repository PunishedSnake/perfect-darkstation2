#ifndef PERFECT_DARK_PS2_GS_DMA_POLICY_H
#define PERFECT_DARK_PS2_GS_DMA_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/*
 * gsKit screen initialization submits GIF (DMAC channel 2), then uses
 * dmaKit_wait_fast/CPCOND0. CPCOND0 tests D_STAT completion flags for ALL
 * selected channels. An idle, never-submitted VIF1 channel need not have a
 * completion flag, so selecting it here can deadlock screen initialization.
 *
 * VIF1 ownership is synchronized explicitly by gs_vu1_queue, not by gsKit's
 * global fast-wait mask. Keep this GIF-only even in the VU1 diagnostic build.
 */
#define PS2_GS_BOOTSTRAP_FASTWAIT_CHANNELS (1u << 2)

struct Ps2GsBootstrapBufferPlan {
    uint8_t clear_buffer;
    uint8_t display_buffer;
    uint8_t next_draw_buffer;
};

/* gsKit clears buffer 0 during init; explicitly prime buffer 1 before scanout. */
static inline struct Ps2GsBootstrapBufferPlan ps2GsBootstrapBufferPlan(
    bool double_buffering)
{
    struct Ps2GsBootstrapBufferPlan plan;
    plan.clear_buffer = double_buffering ? 1u : 0u;
    plan.display_buffer = double_buffering ? 1u : 0u;
    plan.next_draw_buffer = 0u;
    return plan;
}

/*
 * A non-empty PATH3 submit acquires VIF1 ownership before publishing its GIF
 * chain. The following PATH1 submit therefore inherits a known-idle VIF1
 * channel and must not poll it a second time. An empty PATH3 arena makes no
 * ownership claim, so the PATH1 queue still performs its normal late wait
 * after building the next stream.
 */
static inline bool ps2GsPath1NeedsVifWait(
    bool path3_submit_drained_vif)
{
    return !path3_submit_drained_vif;
}

#endif
