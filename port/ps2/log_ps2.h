#ifndef PD_PS2_LOG_H
#define PD_PS2_LOG_H

/*
 * PS2 bring-up logger control plane.
 *
 * Text logs are deliberately buffered and flushed at coarse checkpoints.
 * This is not the future hot-path profiler. Whole-system trace events will use
 * a separate preallocated binary ring so printf formatting never contaminates
 * frame timing.
 */
void ps2LogFlush(void);

#endif
