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

#ifdef __cplusplus
}
#endif

#endif
