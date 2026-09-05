#include <assert.h>

#include <ultra64.h>

#include "video.h"

static s32 g_observed_black = -1;

void videoSetBlack(s32 black)
{
	g_observed_black = black;
}

int main(void)
{
	osViBlack(1);
	assert(g_observed_black == 1);

	osViBlack(0);
	assert(g_observed_black == 0);

	/* libultra treats every nonzero request as the same persistent state. */
	osViBlack(0xff);
	assert(g_observed_black == 1);

	return 0;
}
