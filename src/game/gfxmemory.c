#include <ultra64.h>
#include "constants.h"
#include "game/gfxmemory.h"
#include "game/stubs/game_175f50.h"
#include "bss.h"
#include "lib/args.h"
#include "lib/rzip.h"
#include "lib/dma.h"
#include "lib/memp.h"
#include "lib/rng.h"
#include "lib/str.h"
#include "data.h"
#include "types.h"
#include "platform.h"
#ifndef PLATFORM_N64
#include "system.h"
#endif

/**
 * This file handles memory usage for graphics related tasks.
 *
 * There are two pools, "gfx" and "vtx", which are used to store different data.
 *
 * The gfx pool (g_GfxBuffers) is sized based on the stage's -mgfx and -mgfxtra
 * arguments. It contains only the master display list's GBI bytecode.
 * The master gdl is passed through all rendering functions in the game engine,
 * where each appends to the display list.
 *
 * The vtx pool (g_VtxBuffers) is sized based on the stage's -mvtx argument.
 * It is used for auxiliary graphics data such as vertex arrays, matrices and
 * colours.
 *
 * Both the gfx and vtx pools are split into two buffers of equal size.
 * Only one buffer is active at a time - the other is being drawn to the screen
 * while the active one is being built. Each time a frame is finished the active
 * buffer index is swapped to the other one.
 *
 * Both the gfx and vtx pools have a third element in them, but this is just a
 * marker for the end of the second element's allocation.
 */

/**
 * On 64-bit platforms the Gfx struct is twice as large.
*/
#ifdef PLATFORM_64BIT
#define GFX_SIZE_MULTIPLIER 2
#else
#define GFX_SIZE_MULTIPLIER 1
#endif

u8 *g_GfxBuffers[NUM_GFXTASKS + 1];
u32 var800aa58c;
u8 *g_VtxBuffers[NUM_GFXTASKS + 1];
u8 *g_GfxMemPos;
u8 g_GfxActiveBufferIndex;
u32 g_GfxRequestedDisplayList;

u32 g_GfxSizesByPlayerCount[] = {
	0x00010000 * GFX_SIZE_MULTIPLIER,
	0x00018000 * GFX_SIZE_MULTIPLIER,
	0x00020000 * GFX_SIZE_MULTIPLIER,
	0x00028000 * GFX_SIZE_MULTIPLIER,
};

u32 g_VtxSizesByPlayerCount[] = {
	0x00010000,
	0x00018000,
	0x00020000,
	0x00028000,
};

s32 g_GfxNumSwapsPerBuffer[NUM_GFXTASKS] = {0, 1};
u32 g_GfxNumSwaps = 2;

/**
 * Allocate graphics memory from the heap. Presumably called on stage load.
 *
 * Comments in this function are strings that appear in an XBLA debug build.
 * They were likely in the N64 version but ifdeffed out.
 */
void gfxReset(void)
{
	s32 stack;

	if (argFindByPrefix(1, "-mgfx")) {
		// Argument specified master_dl_size\n
		s32 gfx;
		s32 gfxtra = 0;

		gfx = strtol(argFindByPrefix(1, "-mgfx"), NULL, 0) * 1024;

		if (argFindByPrefix(1, "-mgfxtra")) {
			// ******** Extra specified but are we in the correct game mode I wonder???\n
			if ((g_Vars.coopplayernum >= 0 || g_Vars.antiplayernum >= 0) && PLAYERCOUNT() == 2) {
				// ******** Extra Display List Memeory Required\n
				// ******** Shall steal from video buffer\n
				// ******** If you try and run hi-res then\n
				// ******** you're gonna shafted up the arse\n
				// ******** so don't blame me\n
				gfxtra = strtol(argFindByPrefix(1, "-mgfxtra"), NULL, 0) * 1024;
			} else {
				// ******** No we're not so there\n
			}
		}

		// ******** Original Amount required = %dK ber buffer\n
		// ******** Extra Amount required = %dK ber buffer\n
		// ******** Total of %dK (Double Buffered)\n
		g_GfxSizesByPlayerCount[PLAYERCOUNT() - 1] = (gfx + gfxtra) * GFX_SIZE_MULTIPLIER;
	}

	if (argFindByPrefix(1, "-mvtx")) {
		// Argument specified mtxvtx_size\n
		g_VtxSizesByPlayerCount[PLAYERCOUNT() - 1] = strtol(argFindByPrefix(1, "-mvtx"), NULL, 0) * 1024;
	}

	// %d Players : Allocating %d bytes for master dl's\n
	g_GfxBuffers[0] = mempAlloc(g_GfxSizesByPlayerCount[PLAYERCOUNT() - 1] * NUM_GFXTASKS, MEMPOOL_STAGE);
#ifndef PLATFORM_N64
	if (!g_GfxBuffers[0]) {
		sysFatalError("Could not allocate the stage display-list buffers (%u bytes each).",
			g_GfxSizesByPlayerCount[PLAYERCOUNT() - 1]);
	}
#endif
	g_GfxBuffers[1] = g_GfxBuffers[0] + g_GfxSizesByPlayerCount[PLAYERCOUNT() - 1];
	g_GfxBuffers[2] = g_GfxBuffers[1] + g_GfxSizesByPlayerCount[PLAYERCOUNT() - 1];

	// Allocating %d bytes for mtxvtx space\n
	g_VtxBuffers[0] = mempAlloc(g_VtxSizesByPlayerCount[PLAYERCOUNT() - 1] * NUM_GFXTASKS, MEMPOOL_STAGE);
#ifndef PLATFORM_N64
	if (!g_VtxBuffers[0]) {
		sysFatalError("Could not allocate the stage vertex buffers (%u bytes each).",
			g_VtxSizesByPlayerCount[PLAYERCOUNT() - 1]);
	}
#endif
	g_VtxBuffers[1] = g_VtxBuffers[0] + g_VtxSizesByPlayerCount[PLAYERCOUNT() - 1];
	g_VtxBuffers[2] = g_VtxBuffers[1] + g_VtxSizesByPlayerCount[PLAYERCOUNT() - 1];

	g_GfxActiveBufferIndex = 0;
	g_GfxRequestedDisplayList = false;
	g_GfxMemPos = g_VtxBuffers[0];
}

Gfx *gfxGetMasterDisplayList(void)
{
	g_GfxRequestedDisplayList = true;

	return (Gfx *)g_GfxBuffers[g_GfxActiveBufferIndex];
}

#ifndef PLATFORM_N64
static u32 gfxGetFreeVtxChecked(const char *context)
{
	uintptr_t start;
	uintptr_t end;
	uintptr_t pos;

	if (g_GfxActiveBufferIndex >= NUM_GFXTASKS) {
		sysFatalError("%s: invalid graphics buffer index %u.", context,
			g_GfxActiveBufferIndex);
	}

	start = (uintptr_t)g_VtxBuffers[g_GfxActiveBufferIndex];
	end = (uintptr_t)g_VtxBuffers[g_GfxActiveBufferIndex + 1];
	pos = (uintptr_t)g_GfxMemPos;

	if (!start || !end || end < start || pos < start || pos > end) {
		sysFatalError("%s: vertex arena cursor is outside its active buffer.", context);
	}

	return end - pos;
}

static void *gfxAllocateVtxBytes(u64 size, bool alignend, const char *context)
{
	uintptr_t pos = (uintptr_t)g_GfxMemPos;
	uintptr_t allocation = pos;
	u32 available = gfxGetFreeVtxChecked(context);
	u32 padding;

	if (size > available) {
		sysFatalError("%s: vertex arena exhausted (need %llu bytes, %u remain).",
			context, (unsigned long long)size, available);
	}

	pos += (uintptr_t)size;

	if (alignend) {
		padding = (16u - (u32)(pos & 15u)) & 15u;

		if (size + padding > available) {
			sysFatalError("%s: vertex arena exhausted by 16-byte alignment.", context);
		}

		pos += padding;
	}

	g_GfxMemPos = (u8 *)pos;

	return (void *)allocation;
}
#endif

Vtx *gfxAllocateVertices(u32 count)
{
#ifdef PLATFORM_N64
	void *ptr = g_GfxMemPos;
	g_GfxMemPos += count * sizeof(Vtx);
	g_GfxMemPos = (u8 *)ALIGN16((uintptr_t)g_GfxMemPos);

	return ptr;
#else
	return gfxAllocateVtxBytes((u64)count * sizeof(Vtx), true, "gfxAllocateVertices");
#endif
}

void *gfxAllocateMatrix(void)
{
#ifdef PLATFORM_N64
	void *ptr = g_GfxMemPos;
	g_GfxMemPos += sizeof(Mtx);

	return ptr;
#else
	return gfxAllocateVtxBytes(sizeof(Mtx), false, "gfxAllocateMatrix");
#endif
}

/**
 * sizeof(LookAt) is 0x10 and it consists of two Light structs of 0x8 each.
 * The function allocates 0x8 for every count, so it could be allocating lights
 * instead, however it's only used for LookAts so it's named as LookAt.
 */
LookAt *gfxAllocateLookAt(s32 count)
{
#ifdef PLATFORM_N64
	void *ptr = g_GfxMemPos;
#ifdef PLATFORM_64BIT
	g_GfxMemPos += count * (sizeof(LookAt) * 2);
#else
	g_GfxMemPos += count * (sizeof(LookAt) / 2);
#endif

	return ptr;
#else
	if (count < 0) {
		sysFatalError("gfxAllocateLookAt: negative element count %d.", count);
	}

#ifdef PLATFORM_64BIT
	return gfxAllocateVtxBytes((u64)count * (sizeof(LookAt) * 2), false,
			"gfxAllocateLookAt");
#else
	return gfxAllocateVtxBytes((u64)count * (sizeof(LookAt) / 2), false,
			"gfxAllocateLookAt");
#endif
#endif
}

Col *gfxAllocateColours(s32 count)
{
#ifdef PLATFORM_N64
	void *ptr = g_GfxMemPos;
	count = ALIGN16(count * sizeof(Col));
	g_GfxMemPos += count;

	return ptr;
#else
	if (count < 0) {
		sysFatalError("gfxAllocateColours: negative element count %d.", count);
	}

	return gfxAllocateVtxBytes((u64)count * sizeof(Col), true, "gfxAllocateColours");
#endif
}

void *gfxAllocate(u32 size)
{
#ifdef PLATFORM_N64
	void *ptr = g_GfxMemPos;
	size = ALIGN16(size);
	g_GfxMemPos += size;

	return ptr;
#else
	return gfxAllocateVtxBytes(size, true, "gfxAllocate");
#endif
}

#ifndef PLATFORM_N64
void gfxCheckMasterDisplayList(Gfx *gdl, u32 required, const char *context)
{
	uintptr_t start;
	uintptr_t end;
	uintptr_t pos = (uintptr_t)gdl;
	u32 available;

	if (g_GfxActiveBufferIndex >= NUM_GFXTASKS) {
		sysFatalError("%s: invalid graphics buffer index %u.", context,
			g_GfxActiveBufferIndex);
	}

	start = (uintptr_t)g_GfxBuffers[g_GfxActiveBufferIndex];
	end = (uintptr_t)g_GfxBuffers[g_GfxActiveBufferIndex + 1];

	if (!start || !end || end < start || pos < start || pos > end
			|| (pos - start) % sizeof(Gfx) != 0) {
		sysFatalError("%s: master display-list cursor is outside its active buffer.", context);
	}

	available = (end - pos) / sizeof(Gfx);

	if (required > available) {
		sysFatalError("%s: master display list exhausted (need %u commands, %u remain).",
			context, required, available);
	}
}
#endif

void gfxSwapBuffers(void)
{
	g_GfxActiveBufferIndex ^= 1;
	g_GfxRequestedDisplayList = false;
	g_GfxMemPos = g_VtxBuffers[g_GfxActiveBufferIndex];
	g_GfxNumSwapsPerBuffer[g_GfxActiveBufferIndex] = g_GfxNumSwaps;
	g_GfxNumSwaps++;

	if (g_GfxNumSwaps == -1) {
		g_GfxNumSwaps = 2;
	}
}

s32 gfxGetFreeGfx(Gfx *gdl)
{
#ifndef PLATFORM_N64
	gfxCheckMasterDisplayList(gdl, 0, "gfxGetFreeGfx");
#endif

	return (Gfx *)g_GfxBuffers[g_GfxActiveBufferIndex + 1] - gdl;
}

u32 gfxGetFreeVtx(void)
{
#ifdef PLATFORM_N64
	return g_VtxBuffers[g_GfxActiveBufferIndex + 1] - g_GfxMemPos;
#else
	return gfxGetFreeVtxChecked("gfxGetFreeVtx");
#endif
}
