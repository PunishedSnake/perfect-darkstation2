#include "audio_ps2_queue.h"

#include <limits.h>

enum Ps2AudioSubmitPlan ps2AudioPlanSubmit(
    uint32_t queued_bytes,
    uint32_t available_bytes,
    uint32_t pending_bytes,
    uint32_t queue_limit_samples)
{
    if (pending_bytes == 0) {
        return PS2_AUDIO_DROP_EMPTY;
    }

    if ((pending_bytes % PS2_AUDIO_STEREO_FRAME_BYTES) != 0) {
        return PS2_AUDIO_DROP_MISALIGNED;
    }

    const uint32_t limit_bytes =
        queue_limit_samples > UINT32_MAX / PS2_AUDIO_STEREO_FRAME_BYTES
            ? UINT32_MAX
            : queue_limit_samples * PS2_AUDIO_STEREO_FRAME_BYTES;

    if (queue_limit_samples == 0 || queued_bytes >= limit_bytes) {
        return PS2_AUDIO_DROP_LATENCY_LIMIT;
    }

    /* audsrv_wait_audio cannot ever satisfy a chunk larger than its ring. */
    const uint64_t service_capacity =
        (uint64_t)queued_bytes + (uint64_t)available_bytes;
    if ((uint64_t)pending_bytes > service_capacity) {
        return PS2_AUDIO_DROP_TOO_LARGE;
    }

    return pending_bytes <= available_bytes
        ? PS2_AUDIO_SUBMIT_NOW
        : PS2_AUDIO_SUBMIT_AFTER_WAIT;
}
