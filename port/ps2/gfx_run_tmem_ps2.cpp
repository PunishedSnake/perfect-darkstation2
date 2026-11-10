#include <PR/gbi.h>

#include "rdp_tmem_trace.h"
#include "rdp_tmem_live.h"

/*
 * gfx_pc.cpp is compiled on PS2 with gfx_run renamed to gfx_run_ps2_impl.
 *
 * This seam used to replay every display list through a second, standalone
 * TMEM model (gfxRdpTmemTraceDisplayList) before rendering. The live TMEM
 * model inside the frontend has since become authoritative, nothing on PS2
 * reads the replayed state, and the replay duplicated every LoadBlock/TLUT
 * copy each frame. It is now opt-in for diagnostics only.
 */
extern "C" void gfx_run_ps2_impl(Gfx *commands);

extern "C" void gfx_run(Gfx *commands)
{
#if defined(PD_PS2_TMEM_SHADOW_REPLAY)
    gfxRdpTmemTraceDisplayList(commands);
#endif
    gfx_run_ps2_impl(commands);
    /* Lazy block loads reference display-list source RAM; settle them
     * before the game can change it for the next frame. */
    gfxRdpTmemLiveFlush();
}
