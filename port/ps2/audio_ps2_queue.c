#include "audio_ps2_queue.h"

#include <limits.h>

bool ps2AudioDeriveQueued(
    uint32_t capacity_bytes,
    int available_result,
    uint32_t *queued_bytes,
    uint32_t *available_bytes)
{
    if (capacity_bytes == 0 || available_result < 0 ||
            (uint32_t)available_result > capacity_bytes ||
            !queued_bytes || !available_bytes) {
        return false;
    }

    *available_bytes = (uint32_t)available_result;
    *queued_bytes = capacity_bytes - *available_bytes;
    return true;
}

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

int32_t ps2AudioFrameBudget(
    int32_t requested_frames,
    uint32_t buffered_samples,
    uint32_t frame_samples,
    uint32_t queue_limit_samples,
    int32_t max_frames)
{
    if (requested_frames <= 1) {
        return requested_frames < 0 ? 0 : requested_frames;
    }

    int32_t budget = requested_frames;
    if (max_frames > 0 && budget > max_frames) {
        budget = max_frames;
    }

    if (frame_samples != 0) {
        const uint32_t space = buffered_samples >= queue_limit_samples
            ? 0u
            : queue_limit_samples - buffered_samples;
        const uint32_t fit = (space + frame_samples - 1u) / frame_samples;
        if ((uint32_t)budget > fit) {
            budget = (int32_t)fit;
        }
    }

    return budget < 1 ? 1 : budget;
}
