#include <stdbool.h>

#include <dmaKit.h>
#include <gsKit.h>

/*
 * Bring-up renderer only.
 *
 * This deliberately uses the current gsKit/dmaKit API instead of growing a
 * second graphics abstraction before the real GfxRenderingAPI backend exists.
 * It has one job: prove that an EE build can initialise GIF/GS and present a
 * deterministic visible frame on real hardware.
 */
bool ps2VideoDiagRun(int rom_status)
{
    GSGLOBAL *gs = gsKit_init_global();

    if (!gs) {
        return false;
    }

    /*
     * The diagnostic has no textures and no depth testing. CT16 plus no
     * Z-buffer keeps its GS-local-memory footprint deliberately small while
     * retaining gsKit's normal double-buffered presentation path.
     */
    gs->PSM = GS_PSM_CT16;
    gs->ZBuffering = GS_SETTING_OFF;
    gs->Dithering = GS_SETTING_ON;

    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC,
        D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);

    gsKit_init_screen(gs);
    gsKit_mode_switch(gs, GS_ONESHOT);
    gsKit_set_test(gs, GS_ZTEST_OFF);

    const u64 background = GS_SETREG_RGBAQ(0x08, 0x0c, 0x18, 0x00, 0x00);
    const u64 panel = GS_SETREG_RGBAQ(0x20, 0x2a, 0x42, 0x00, 0x00);
    const u64 white = GS_SETREG_RGBAQ(0xff, 0xff, 0xff, 0x00, 0x00);
    const u64 red = GS_SETREG_RGBAQ(0xff, 0x34, 0x34, 0x00, 0x00);
    const u64 green = GS_SETREG_RGBAQ(0x32, 0xd0, 0x74, 0x00, 0x00);
    const u64 blue = GS_SETREG_RGBAQ(0x38, 0x82, 0xf6, 0x00, 0x00);
    const u64 amber = GS_SETREG_RGBAQ(0xf5, 0xa5, 0x24, 0x00, 0x00);
    const u64 rom_color = rom_status > 0 ? green : (rom_status < 0 ? red : amber);

    const float width = (float)gs->Width;
    const float height = (float)gs->Height;
    const float margin_x = width * 0.08f;
    const float margin_y = height * 0.10f;

    for (;;) {
        gsKit_clear(gs, background);

        /* Frame and three state bars: EE/system, GS, optional ROM source. */
        gsKit_prim_sprite(gs,
            margin_x, margin_y,
            width - margin_x, height - margin_y,
            1, panel);

        const float bar_left = margin_x * 1.45f;
        const float bar_right = width - bar_left;
        const float bar_h = height * 0.055f;
        const float bar_gap = height * 0.035f;
        const float bar_y = margin_y * 1.55f;

        gsKit_prim_sprite(gs,
            bar_left, bar_y,
            bar_right, bar_y + bar_h,
            2, green);
        gsKit_prim_sprite(gs,
            bar_left, bar_y + bar_h + bar_gap,
            bar_right, bar_y + (bar_h * 2.0f) + bar_gap,
            2, blue);
        gsKit_prim_sprite(gs,
            bar_left, bar_y + (bar_h + bar_gap) * 2.0f,
            bar_right, bar_y + (bar_h * 3.0f) + (bar_gap * 2.0f),
            2, rom_color);

        /* A real Gouraud triangle makes this more than a framebuffer clear. */
        gsKit_prim_triangle_gouraud(gs,
            width * 0.50f, height * 0.43f,
            width * 0.28f, height * 0.78f,
            width * 0.72f, height * 0.78f,
            3,
            red, green, blue);

        /* Small white marker makes field/offset problems obvious on a CRT. */
        gsKit_prim_sprite(gs,
            width * 0.48f, height * 0.84f,
            width * 0.52f, height * 0.87f,
            4, white);

        gsKit_queue_exec(gs);
        gsKit_sync_flip(gs);
    }
}
