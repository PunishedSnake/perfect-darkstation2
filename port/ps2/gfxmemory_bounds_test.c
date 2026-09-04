#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "game/gfxmemory.h"

extern u8 *g_GfxBuffers[NUM_GFXTASKS + 1];
extern u8 *g_VtxBuffers[NUM_GFXTASKS + 1];
extern u8 *g_GfxMemPos;
extern u8 g_GfxActiveBufferIndex;

static jmp_buf g_FatalJump;
static char g_FatalMessage[256];

void sysFatalError(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(g_FatalMessage, sizeof(g_FatalMessage), fmt, ap);
	va_end(ap);
	longjmp(g_FatalJump, 1);
}

static int expectFatalContaining(const char *needle, void (*operation)(void))
{
	g_FatalMessage[0] = '\0';

	if (setjmp(g_FatalJump) == 0) {
		operation();
		fprintf(stderr, "expected fatal error containing '%s'\n", needle);
		return 0;
	}

	if (!strstr(g_FatalMessage, needle)) {
		fprintf(stderr, "fatal error '%s' did not contain '%s'\n", g_FatalMessage, needle);
		return 0;
	}

	return 1;
}

static void allocatePastVtxEnd(void)
{
	gfxAllocateVertices(1);
}

static void allocateAlignmentPastVtxEnd(void)
{
	gfxAllocate(17);
}

static void allocateNegativeColours(void)
{
	gfxAllocateColours(-1);
}

static void requireTooManyGfxCommands(void)
{
	gfxCheckMasterDisplayList((Gfx *)g_GfxBuffers[0] + 6, 3, "test trailer");
}

static void useEscapedGfxCursor(void)
{
	gfxCheckMasterDisplayList((Gfx *)g_GfxBuffers[0] + 9, 0, "test escaped");
}

int main(void)
{
	_Alignas(16) static u8 vtxarena[256];
	_Alignas(16) static Gfx gfxarena[8];
	u8 *ptr;
	u32 expected;
	u32 previous;

	g_GfxActiveBufferIndex = 0;
	g_VtxBuffers[0] = vtxarena;
	g_VtxBuffers[1] = vtxarena + sizeof(vtxarena);
	g_VtxBuffers[2] = g_VtxBuffers[1];
	g_GfxMemPos = vtxarena;

	ptr = (u8 *)gfxAllocateVertices(1);
	expected = (sizeof(Vtx) + 15u) & ~15u;

	if (ptr != vtxarena || g_GfxMemPos != vtxarena + expected) {
		fprintf(stderr, "vertex allocation returned an invalid range\n");
		return 1;
	}

	ptr = (u8 *)gfxAllocateColours(1);
	expected += (sizeof(Col) + 15u) & ~15u;

	if (ptr != vtxarena + ((sizeof(Vtx) + 15u) & ~15u)
			|| g_GfxMemPos != vtxarena + expected) {
		fprintf(stderr, "colour allocation did not preserve 16-byte alignment\n");
		return 1;
	}

	previous = expected;
	ptr = (u8 *)gfxAllocateLookAt(2);

#ifdef PLATFORM_64BIT
	expected += 2 * (sizeof(LookAt) * 2);
#else
	expected += 2 * (sizeof(LookAt) / 2);
#endif

	if (ptr != vtxarena + previous || g_GfxMemPos != vtxarena + expected) {
		fprintf(stderr, "LookAt allocation returned an invalid range\n");
		return 1;
	}

	previous = expected;
	ptr = gfxAllocateMatrix();
	expected += sizeof(Mtx);

	if (ptr != vtxarena + previous || g_GfxMemPos != vtxarena + expected) {
		fprintf(stderr, "matrix allocation returned an invalid range\n");
		return 1;
	}

	previous = expected;
	ptr = gfxAllocate(1);
	expected = (expected + 16u) & ~15u;

	if (ptr != vtxarena + previous || g_GfxMemPos != vtxarena + expected
			|| gfxGetFreeVtx() != sizeof(vtxarena) - expected) {
		fprintf(stderr, "generic allocation or free-space report is invalid\n");
		return 1;
	}

	g_GfxMemPos = g_VtxBuffers[1];

	if (!expectFatalContaining("gfxAllocateVertices", allocatePastVtxEnd)) {
		return 1;
	}

	g_GfxMemPos = vtxarena;
	g_VtxBuffers[1] = vtxarena + 17;

	if (!expectFatalContaining("alignment", allocateAlignmentPastVtxEnd)) {
		return 1;
	}

	if (g_GfxMemPos != vtxarena) {
		fprintf(stderr, "failed allocation advanced the vertex cursor\n");
		return 1;
	}

	g_VtxBuffers[1] = vtxarena + sizeof(vtxarena);

	if (!expectFatalContaining("negative", allocateNegativeColours)) {
		return 1;
	}

	g_GfxBuffers[0] = (u8 *)gfxarena;
	g_GfxBuffers[1] = (u8 *)(gfxarena + 8);
	g_GfxBuffers[2] = g_GfxBuffers[1];
	gfxCheckMasterDisplayList(gfxarena + 6, 2, "test exact fit");

	if (!expectFatalContaining("need 3 commands", requireTooManyGfxCommands)) {
		return 1;
	}

	if (!expectFatalContaining("outside", useEscapedGfxCursor)) {
		return 1;
	}

	puts("gfxmemory bounds: ok");
	return 0;
}
