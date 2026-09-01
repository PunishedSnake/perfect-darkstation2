#ifndef PERFECT_DARK_PS2_LOG_CHECKPOINT_POLICY_H
#define PERFECT_DARK_PS2_LOG_CHECKPOINT_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/* Avoid exhausting/leaking mass: open-file state during dense bring-up logs. */
#define PS2_LOG_DURABLE_INTERVAL_USEC UINT64_C(100000)

static inline bool ps2LogCheckpointShouldClose(
    uint64_t now_usec,
    uint64_t last_close_usec,
    bool has_closed,
    bool force)
{
    return force || !has_closed || now_usec < last_close_usec ||
        now_usec - last_close_usec >= PS2_LOG_DURABLE_INTERVAL_USEC;
}

#endif
