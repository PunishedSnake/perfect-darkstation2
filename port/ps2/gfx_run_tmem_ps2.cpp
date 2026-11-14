#include <PR/gbi.h>

#include "rdp_tmem_trace.h"
#include "rdp_tmem_live.h"
#include "rdp_tmem.h"
#include "texmem_epoch.h"

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
    /*
     * Texture identities verified in earlier runs stay valid until the game
     * writes memory a texture may live in (see texmem_epoch.h).
     */
    static unsigned int s_seen_texmem_epoch = ~0u;
    if (g_TexMemWriteEpoch != s_seen_texmem_epoch) {
        s_seen_texmem_epoch = g_TexMemWriteEpoch;
        gfxRdpTmemSourceMemoNewEpoch();
    }
    gfx_run_ps2_impl(commands);
    /* Lazy block loads reference display-list source RAM; settle them
     * before the game can change it for the next frame. */
    gfxRdpTmemLiveFlush();
}
