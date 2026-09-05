#include <assert.h>

#include "vi_unblack_timer.h"

int main(void)
{
	/* A permanent blackout is not a countdown. */
	assert(viUnblackTimerAfterRetrace(4, 2) == 4);

	/* viBlack(false) drains across both emulated framebuffers. */
	assert(viUnblackTimerAfterRetrace(2, 2) == 1);
	assert(viUnblackTimerAfterRetrace(1, 2) == 0);

	/* Unblanked and malformed negative states remain stable. */
	assert(viUnblackTimerAfterRetrace(0, 2) == 0);
	assert(viUnblackTimerAfterRetrace(-1, 2) == -1);

	return 0;
}
