#ifndef PERFECT_DARK_PS2_GS_DMA_POLICY_H
#define PERFECT_DARK_PS2_GS_DMA_POLICY_H

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

#endif
