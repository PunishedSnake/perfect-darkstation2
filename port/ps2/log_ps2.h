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
 * A checkpoint always flushes and periodically closes/reopens the file. Dense
 * callers are throttled because repeated mass: reopen cycles can stop making
 * progress. Keep these checkpoints out of frame/hot paths.
 */
void ps2LogCheckpoint(void);

/* Bypass checkpoint throttling for an explicit user snapshot or fatal error. */
void ps2LogCheckpointForce(void);

#ifdef __cplusplus
}
#endif

#endif
