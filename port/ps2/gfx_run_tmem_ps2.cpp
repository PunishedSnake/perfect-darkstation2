#include <PR/gbi.h>

#include "rdp_tmem_trace.h"

/*
 * Transitional PS2 shadow seam.
 *
 * gfx_pc.cpp is compiled on PS2 with gfx_run renamed to gfx_run_ps2_impl.
 * This wrapper observes the exact same display list first, updates the shared
 * backend-independent TMEM model, then hands control to the unchanged Fast3D
 * renderer. The shadow cannot affect pixels yet by design.
 */
extern "C" void gfx_run_ps2_impl(Gfx *commands);

extern "C" void gfx_run(Gfx *commands)
{
    gfxRdpTmemTraceDisplayList(commands);
    gfx_run_ps2_impl(commands);
}
