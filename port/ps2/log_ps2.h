#ifndef PD_PS2_LOG_H
#define PD_PS2_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PS2 bring-up logger control plane.
 *
 * Text logs are deliberately buffered and flushed at coarse checkpoints.
 * This is not the future hot-path profiler. Whole-system trace events will use
 * a separate preallocated binary ring so printf formatting never contaminates
 * frame timing.
 */
void ps2LogFlush(void);

/*
 * Durable bring-up checkpoint.
 *
 * Current PS2SDK does not implement fsync(). On filesystem-backed launchers,
 * especially mass:, file size/directory metadata may remain stale until close.
 * A checkpoint therefore flushes, closes, and reopens the log in append mode.
 * Keep these checkpoints coarse; do not call this from frame/hot paths.
 */
void ps2LogCheckpoint(void);

#ifdef __cplusplus
}
#endif

#endif
