#include <stdbool.h>
#include <string.h>

#include <gsKit.h>

#include "system.h"
#include "log_ps2.h"

#define SCENE_TEXTURE_W 64
#define SCENE_TEXTURE_H 64
#define SCENE_VERTEX_COUNT 8
#define SCENE_FACE_COUNT 6

/*
 * Renderer bring-up stage 1.
 *
 * This is intentionally a small GS baseline rather than the final Perfect Dark
 * backend. It proves the contracts we need before wiring Fast3D into the PS2:
 *
 *   - a resident texture uploaded once before the frame loop,
 *   - a real Z buffer and depth-tested overlapping geometry,
 *   - CPU-produced dynamic screen-space vertices,
 *   - repeated draw submission through gsKit's oneshot queue,
 *   - no allocation or texture upload in the frame loop.
 *
 * VIF/VU/MMI are deliberately absent. They only become candidates after a
 * real game workload identifies a measured geometry/transform bottleneck.
 */

struct scene_vec3 {
    float x;
    float y;
    float z;
};

struct scene_screen_vertex {
    float x;
    float y;
    int z;
};

struct scene_face {
    unsigned char index[4];
    u64 color;
};

static const struct scene_vec3 kCubeVertices[SCENE_VERTEX_COUNT] = {
    { -1.0f, -1.0f, -1.0f },
    {  1.0f, -1.0f, -1.0f },
    {  1.0f,  1.0f, -1.0f },
    { -1.0f,  1.0f, -1.0f },
    { -1.0f, -1.0f,  1.0f },
    {  1.0f, -1.0f,  1.0f },
    {  1.0f,  1.0f,  1.0f },
    { -1.0f,  1.0f,  1.0f },
};

/*
 * Faces intentionally stay in a fixed order. Correct visibility therefore
 * depends on the Z buffer rather than painter sorting.
 */
static const struct scene_face kCubeFaces[SCENE_FACE_COUNT] = {
    { { 0, 1, 2, 3 }, GS_SETREG_RGBAQ(0x80, 0x90, 0xff, 0x80, 0x00) },
    { { 5, 4, 7, 6 }, GS_SETREG_RGBAQ(0xff, 0x78, 0x78, 0x80, 0x00) },
    { { 4, 0, 3, 7 }, GS_SETREG_RGBAQ(0x78, 0xe0, 0x9a, 0x80, 0x00) },
    { { 1, 5, 6, 2 }, GS_SETREG_RGBAQ(0xff, 0xc8, 0x68, 0x80, 0x00) },
    { { 3, 2, 6, 7 }, GS_SETREG_RGBAQ(0xc5, 0x8c, 0xff, 0x80, 0x00) },
    { { 4, 5, 1, 0 }, GS_SETREG_RGBAQ(0x6f, 0xd7, 0xe8, 0x80, 0x00) },
};

/*
 * gsKit sends this buffer through the GIF DMA texture path. This alignment is
 * device/transport alignment for this one DMA source, not a general allocator
 * policy for the game.
 */
static u32 sCheckerTexture[SCENE_TEXTURE_W * SCENE_TEXTURE_H]
    __attribute__((aligned(64)));

static void buildCheckerTexture(void)
{
    for (int y = 0; y < SCENE_TEXTURE_H; ++y) {
        for (int x = 0; x < SCENE_TEXTURE_W; ++x) {
            const int checker = ((x >> 3) ^ (y >> 3)) & 1;
            const int cross = (x == SCENE_TEXTURE_W / 2) || (y == SCENE_TEXTURE_H / 2);
            u32 pixel;

            if (cross) {
                /* RGBA8888 in EE memory: R is the least-significant byte. */
                pixel = 0x8028d8ffu;
            } else if (checker) {
                pixel = 0x80e8e8e8u;
            } else {
                pixel = 0x80302a28u;
            }

            sCheckerTexture[y * SCENE_TEXTURE_W + x] = pixel;
        }
    }
}

static bool initSceneTexture(GSGLOBAL *gs, GSTEXTURE *texture)
{
    memset(texture, 0, sizeof(*texture));
    buildCheckerTexture();

    texture->Width = SCENE_TEXTURE_W;
    texture->Height = SCENE_TEXTURE_H;
    texture->PSM = GS_PSM_CT32;
    texture->Mem = sCheckerTexture;
    texture->Filter = GS_FILTER_NEAREST;

    const u32 textureBytes = gsKit_texture_size(
        texture->Width, texture->Height, texture->PSM);

    texture->Vram = gsKit_vram_alloc(gs, textureBytes, GSKIT_ALLOC_USERBUFFER);
    if (texture->Vram == GSKIT_ALLOC_ERROR) {
        sysLogPrintf(LOG_ERROR,
            "Renderer scene: GS VRAM allocation failed for %u-byte texture",
            textureBytes);
        return false;
    }

    sysLogPrintf(LOG_NOTE,
        "Renderer scene: texture %ux%u CT32 EE=%p GS_VRAM=%08x bytes=%u",
        texture->Width, texture->Height, texture->Mem, texture->Vram, textureBytes);
    ps2LogCheckpoint();

    /* Immutable scene texture: upload once, then consume from GS local memory. */
    gsKit_texture_upload(gs, texture);
    gsKit_set_clamp(gs, GS_CMODE_REPEAT);
    gsKit_set_texfilter(gs, GS_FILTER_NEAREST);

    sysLogPrintf(LOG_NOTE, "Renderer scene: procedural texture upload complete");
    ps2LogCheckpoint();
    return true;
}

static struct scene_screen_vertex projectVertex(
    const struct scene_vec3 *source,
    float sinY,
    float cosY,
    float sinX,
    float cosX,
    float centerX,
    float centerY,
    float focal)
{
    /* Y rotation, then X rotation, then a fixed camera translation. */
    const float rx = source->x * cosY + source->z * sinY;
    const float rz0 = -source->x * sinY + source->z * cosY;
    const float ry = source->y * cosX - rz0 * sinX;
    const float rz = source->y * sinX + rz0 * cosX;
    const float cameraZ = rz + 4.6f;
    const float invZ = 1.0f / cameraZ;

    struct scene_screen_vertex out;
    out.x = centerX + rx * focal * invZ;
    out.y = centerY - ry * focal * invZ;

    /*
     * gsKit's current GS_ZTEST_ON selects GEQUAL and gsKit_clear writes Z=0.
     * Reciprocal depth therefore maps nearer vertices to larger Z values.
     */
    out.z = (int)(60000.0f * invZ);
    if (out.z < 1) {
        out.z = 1;
    } else if (out.z > 65535) {
        out.z = 65535;
    }

    return out;
}

static void advanceRotation(float *sinValue, float *cosValue, float sinStep, float cosStep)
{
    const float oldSin = *sinValue;
    const float oldCos = *cosValue;

    *sinValue = oldSin * cosStep + oldCos * sinStep;
    *cosValue = oldCos * cosStep - oldSin * sinStep;
}

static void drawCube(
    GSGLOBAL *gs,
    GSTEXTURE *texture,
    float sinY,
    float cosY,
    float sinX,
    float cosX)
{
    struct scene_screen_vertex projected[SCENE_VERTEX_COUNT];
    const float centerX = (float)gs->Width * 0.50f;
    const float centerY = (float)gs->Height * 0.49f;
    const float focal = (float)gs->Height * 1.18f;

    for (int i = 0; i < SCENE_VERTEX_COUNT; ++i) {
        projected[i] = projectVertex(
            &kCubeVertices[i], sinY, cosY, sinX, cosX,
            centerX, centerY, focal);
    }

    const float u0 = 0.0f;
    const float v0 = 0.0f;
    const float u1 = (float)(SCENE_TEXTURE_W - 1);
    const float v1 = (float)(SCENE_TEXTURE_H - 1);

    for (int faceIndex = 0; faceIndex < SCENE_FACE_COUNT; ++faceIndex) {
        const struct scene_face *face = &kCubeFaces[faceIndex];
        const struct scene_screen_vertex *a = &projected[face->index[0]];
        const struct scene_screen_vertex *b = &projected[face->index[1]];
        const struct scene_screen_vertex *c = &projected[face->index[2]];
        const struct scene_screen_vertex *d = &projected[face->index[3]];

        gsKit_prim_quad_goraud_texture_3d(
            gs, texture,
            a->x, a->y, a->z, u0, v1,
            b->x, b->y, b->z, u1, v1,
            c->x, c->y, c->z, u1, v0,
            d->x, d->y, d->z, u0, v0,
            face->color, face->color, face->color, face->color);
    }
}

static void drawBackdrop(GSGLOBAL *gs, int romStatus)
{
    const u64 panel = GS_SETREG_RGBAQ(0x12, 0x18, 0x28, 0x80, 0x00);
    const u64 pass = GS_SETREG_RGBAQ(0x32, 0xd0, 0x74, 0x80, 0x00);
    const u64 fail = GS_SETREG_RGBAQ(0xff, 0x45, 0x45, 0x80, 0x00);
    const u64 unknown = GS_SETREG_RGBAQ(0xf5, 0xa5, 0x24, 0x80, 0x00);
    const u64 status = romStatus > 0 ? pass : (romStatus < 0 ? fail : unknown);

    /* These are intentionally behind the 3D object in reciprocal-Z space. */
    gsKit_prim_sprite(gs,
        (float)gs->Width * 0.10f, (float)gs->Height * 0.10f,
        (float)gs->Width * 0.90f, (float)gs->Height * 0.90f,
        1, panel);

    gsKit_prim_sprite(gs,
        (float)gs->Width * 0.12f, (float)gs->Height * 0.86f,
        (float)gs->Width * 0.88f, (float)gs->Height * 0.88f,
        2, status);
}

bool ps2RendererSceneRun(GSGLOBAL *gs, int romStatus)
{
    GSTEXTURE texture;

    if (!gs) {
        return false;
    }

    sysLogPrintf(LOG_NOTE,
        "Renderer scene: begin depth-tested textured cube; no per-frame allocation/upload");
    ps2LogCheckpoint();

    if (!initSceneTexture(gs, &texture)) {
        return false;
    }

    /*
     * These constants are sin/cos of small fixed steps. Updating the basis by
     * recurrence avoids adding libm work to every diagnostic frame. This is a
     * bring-up choice, not a claim that the game should use this math path.
     */
    const float sinStepY = 0.011999712f;
    const float cosStepY = 0.999928001f;
    const float sinStepX = 0.007999915f;
    const float cosStepX = 0.999968000f;
    float sinY = 0.0f;
    float cosY = 1.0f;
    float sinX = 0.0f;
    float cosX = 1.0f;

    sysLogPrintf(LOG_NOTE,
        "Renderer scene: entering frame loop Z16 + CT16 framebuffer + CT32 resident texture");
    ps2LogCheckpoint();

    const u64 background = GS_SETREG_RGBAQ(0x05, 0x08, 0x12, 0x80, 0x00);

    for (;;) {
        /*
         * Current gsKit_clear temporarily switches ZTST to ALWAYS and emits
         * Z=0 over the frame, which clears both color and our GEQUAL Z baseline.
         */
        gsKit_clear(gs, background);
        drawBackdrop(gs, romStatus);
        drawCube(gs, &texture, sinY, cosY, sinX, cosX);

        gsKit_queue_exec(gs);
        gsKit_sync_flip(gs);

        advanceRotation(&sinY, &cosY, sinStepY, cosStepY);
        advanceRotation(&sinX, &cosX, sinStepX, cosStepX);
    }
}
