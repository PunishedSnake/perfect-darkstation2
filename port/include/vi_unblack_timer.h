#ifndef PORT_VI_UNBLACK_TIMER_H
#define PORT_VI_UNBLACK_TIMER_H

#include <stdint.h>

/*
 * viBlack(false) deliberately leaves output blank for one complete rotation
 * through the emulated VI buffers. The original scheduler consumes this
 * countdown on retrace; the portable scheduler must do the same even though
 * it presents through a native video backend rather than osViSwapBuffer.
 */
static inline int32_t viUnblackTimerAfterRetrace(
		int32_t timer, int32_t framebuffer_count)
{
	if (timer > 0 && timer <= framebuffer_count) {
		timer--;
	}

	return timer;
}

#endif
