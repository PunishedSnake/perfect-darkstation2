#ifndef PERFECT_DARK_PS2_AUDIO_QUEUE_H
#define PERFECT_DARK_PS2_AUDIO_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS2_AUDIO_STEREO_FRAME_BYTES 4u

enum Ps2AudioSubmitPlan {
    PS2_AUDIO_SUBMIT_NOW = 0,
    PS2_AUDIO_SUBMIT_AFTER_WAIT,
    PS2_AUDIO_DROP_EMPTY,
    PS2_AUDIO_DROP_MISALIGNED,
    PS2_AUDIO_DROP_LATENCY_LIMIT,
    PS2_AUDIO_DROP_TOO_LARGE,
};

/*
 * Decide ownership before touching the IOP service. queued_bytes and
 * available_bytes are one audsrv observation; pending_bytes is still owned by
 * the game until the selected submit completes.
 */
enum Ps2AudioSubmitPlan ps2AudioPlanSubmit(
    uint32_t queued_bytes,
    uint32_t available_bytes,
    uint32_t pending_bytes,
    uint32_t queue_limit_samples);

/*
 * audsrv exposes the free and occupied portions of one fixed-size ring as
 * separate synchronous RPC calls. Once the capacity has been observed at
 * startup, one current available observation is sufficient to reconstruct
 * the occupied byte count exactly.
 */
bool ps2AudioDeriveQueued(
    uint32_t capacity_bytes,
    int available_result,
    uint32_t *queued_bytes,
    uint32_t *available_bytes);

/*
 * The game synthesizes one audio manager frame (~368 samples) for every 60 Hz
 * tick that elapsed since the previous game frame. When the EE falls behind,
 * that turns into a death spiral: a 650 ms frame asks for ~39 audio frames,
 * almost all of which would be dropped by the latency limit anyway, and the
 * synthesis itself makes the next frame slower still. Bound the work to what
 * the SPU2 ring can accept and to a fixed number of frames per game tick.
 * At least one frame is always rendered so steady full-speed behaviour is
 * unchanged.
 */
#define PS2_AUDIO_MAX_FRAMES_PER_TICK 4

int32_t ps2AudioFrameBudget(
    int32_t requested_frames,
    uint32_t buffered_samples,
    uint32_t frame_samples,
    uint32_t queue_limit_samples,
    int32_t max_frames);

#ifdef __cplusplus
}
#endif

#endif
