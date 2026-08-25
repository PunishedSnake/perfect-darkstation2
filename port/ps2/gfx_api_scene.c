#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <gsKit.h>

#include "gfx_cc.h"
#include "gfx_ps2.h"
#include "system.h"
#include "log_ps2.h"

#define API_SCENE_TEXTURE_W 64
#define API_SCENE_TEXTURE_H 64
#define API_SCENE_VERTEX_COUNT 8
#define API_SCENE_FACE_COUNT 6
#define API_SCENE_TRIANGLES_PER_FACE 2
#define API_SCENE_VERTICES_PER_TRIANGLE 3
#define API_SCENE_DRAW_VERTEX_COUNT \
    (API_SCENE_FACE_COUNT * API_SCENE_TRIANGLES_PER_FACE * API_SCENE_VERTICES_PER_TRIANGLE)
#define API_SCENE_TEXTURED_STRIDE 9
#define API_SCENE_STATUS_VERTEX_COUNT 6
#define API_SCENE_UNTEXTURED_STRIDE 7

struct api_scene_vec3 {
    float x;
    float y;
    float z;
};

struct api_scene_clip_vertex {
    float x;
    float y;
    float z;
    float w;
};

struct api_scene_face {
    unsigned char index[4];
    unsigned char r;
    unsigned char g;
    unsigned char b;
};

static const struct api_scene_vec3 kCubeVertices[API_SCENE_VERTEX_COUNT] = {
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
 * GS texture modulation uses 0x80 as unity. Keeping the face tints at or
 * below 0x80 makes saturation bugs visible instead of hiding them in white.
 */
static const struct api_scene_face kCubeFaces[API_SCENE_FACE_COUNT] = {
    { { 0, 1, 2, 3 }, 0x68, 0x70, 0x80 },
    { { 5, 4, 7, 6 }, 0x80, 0x58, 0x58 },
    { { 4, 0, 3, 7 }, 0x58, 0x80, 0x64 },
    { { 1, 5, 6, 2 }, 0x80, 0x70, 0x48 },
    { { 3, 2, 6, 7 }, 0x70, 0x58, 0x80 },
    { { 4, 5, 1, 0 }, 0x50, 0x78, 0x80 },
};

/*
 * This is device/transport alignment for one immutable GIF texture source,
 * not an allocator policy for arbitrary game data.
 */
static u32 sCheckerTexture[API_SCENE_TEXTURE_W * API_SCENE_TEXTURE_H]
    __attribute__((aligned(64)));

/*
 * Current gfx_pc.cpp batches a clip-space float VBO before handing it to the
 * rendering API. Keep the same representation here so this hardware test
 * exercises the exact backend boundary that Fast3D will use next.
 */
static float sCubeVbo[API_SCENE_DRAW_VERTEX_COUNT * API_SCENE_TEXTURED_STRIDE];
static float sStatusVbo[API_SCENE_STATUS_VERTEX_COUNT * API_SCENE_UNTEXTURED_STRIDE];

static float clampfLocal(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void buildCheckerTexture(void)
{
    for (int y = 0; y < API_SCENE_TEXTURE_H; ++y) {
        for (int x = 0; x < API_SCENE_TEXTURE_W; ++x) {
            const int checker = ((x >> 3) ^ (y >> 3)) & 1;
            const int cross = (x == API_SCENE_TEXTURE_W / 2) || (y == API_SCENE_TEXTURE_H / 2);
            u32 pixel;

            if (cross) {
                pixel = 0x8028d8ffu;
            } else if (checker) {
                pixel = 0x80d8d8d8u;
            } else {
                pixel = 0x80302a28u;
            }

            sCheckerTexture[y * API_SCENE_TEXTURE_W + x] = pixel;
        }
    }
}

static struct api_scene_clip_vertex projectVertex(
    const struct api_scene_vec3 *source,
    float sinY,
    float cosY,
    float sinX,
    float cosX,
    float aspect)
{
    const float rx = source->x * cosY + source->z * sinY;
    const float rz0 = -source->x * sinY + source->z * cosY;
    const float ry = source->y * cosX - rz0 * sinX;
    const float rz = source->y * sinX + rz0 * cosX;
    const float cameraZ = rz + 4.6f;

    /*
     * gfx_ps2 consumes current Fast3D's clip-space contract. W is therefore
     * real camera depth, not an already-divided screen coordinate. The backend
     * performs XY/W and emits Q=1/W for GS STQ interpolation.
     */
    const float focalY = 2.36f;
    const float focalX = focalY / aspect;
    const float nearZ = 2.5f;
    const float farZ = 7.0f;
    const float depth01 = clampfLocal((cameraZ - nearZ) / (farZ - nearZ), 0.0f, 1.0f);

    struct api_scene_clip_vertex out;
    out.x = rx * focalX;
    out.y = ry * focalY;
    out.z = depth01 * cameraZ;
    out.w = cameraZ;
    return out;
}

static void writeTexturedVertex(
    float **dst,
    const struct api_scene_clip_vertex *vertex,
    float u,
    float v,
    const struct api_scene_face *face)
{
    float *p = *dst;

    *p++ = vertex->x;
    *p++ = vertex->y;
    *p++ = vertex->z;
    *p++ = vertex->w;

    /* Current gfx_pc.cpp hands normalized UV to GfxRenderingAPI. */
    *p++ = u;
    *p++ = v;

    *p++ = (float)face->r / 128.0f;
    *p++ = (float)face->g / 128.0f;
    *p++ = (float)face->b / 128.0f;

    *dst = p;
}

static void buildCubeVbo(float sinY, float cosY, float sinX, float cosX, float aspect)
{
    struct api_scene_clip_vertex projected[API_SCENE_VERTEX_COUNT];
    float *dst = sCubeVbo;

    for (int i = 0; i < API_SCENE_VERTEX_COUNT; ++i) {
        projected[i] = projectVertex(&kCubeVertices[i], sinY, cosY, sinX, cosX, aspect);
    }

    for (int faceIndex = 0; faceIndex < API_SCENE_FACE_COUNT; ++faceIndex) {
        const struct api_scene_face *face = &kCubeFaces[faceIndex];
        const struct api_scene_clip_vertex *a = &projected[face->index[0]];
        const struct api_scene_clip_vertex *b = &projected[face->index[1]];
        const struct api_scene_clip_vertex *c = &projected[face->index[2]];
        const struct api_scene_clip_vertex *d = &projected[face->index[3]];

        writeTexturedVertex(&dst, a, 0.0f, 1.0f, face);
        writeTexturedVertex(&dst, b, 1.0f, 1.0f, face);
        writeTexturedVertex(&dst, c, 1.0f, 0.0f, face);

        writeTexturedVertex(&dst, a, 0.0f, 1.0f, face);
        writeTexturedVertex(&dst, c, 1.0f, 0.0f, face);
        writeTexturedVertex(&dst, d, 0.0f, 0.0f, face);
    }
}

static void writeUntexturedVertex(float **dst, float x, float y, float z, float r, float g, float b)
{
    float *p = *dst;
    *p++ = x;
    *p++ = y;
    *p++ = z;
    *p++ = 1.0f;
    *p++ = r;
    *p++ = g;
    *p++ = b;
    *dst = p;
}

static void buildStatusVbo(int romStatus)
{
    float r = 0.95f;
    float g = 0.65f;
    float b = 0.15f;
    float *dst = sStatusVbo;

    if (romStatus > 0) {
        r = 0.25f;
        g = 0.95f;
        b = 0.50f;
    } else if (romStatus < 0) {
        r = 1.0f;
        g = 0.25f;
        b = 0.25f;
    }

    /* Two triangles, far in depth, rendered through the untextured shader. */
    writeUntexturedVertex(&dst, -0.78f, -0.82f, 0.96f, r, g, b);
    writeUntexturedVertex(&dst,  0.78f, -0.82f, 0.96f, r, g, b);
    writeUntexturedVertex(&dst,  0.78f, -0.77f, 0.96f, r, g, b);
    writeUntexturedVertex(&dst, -0.78f, -0.82f, 0.96f, r, g, b);
    writeUntexturedVertex(&dst,  0.78f, -0.77f, 0.96f, r, g, b);
    writeUntexturedVertex(&dst, -0.78f, -0.77f, 0.96f, r, g, b);
}

static void advanceRotation(float *sinValue, float *cosValue, float sinStep, float cosStep)
{
    const float oldSin = *sinValue;
    const float oldCos = *cosValue;

    *sinValue = oldSin * cosStep + oldCos * sinStep;
    *cosValue = oldCos * cosStep - oldSin * sinStep;
}

bool ps2GfxApiSceneRun(GSGLOBAL *gs, int romStatus)
{
    if (!gs) {
        return false;
    }

    struct GfxRenderingAPI *api = &gfx_ps2_api;
    const uint64_t texturedShaderId =
        ((uint64_t)SHADER_TEXEL0 << 0) |
        ((uint64_t)SHADER_INPUT_1 << 8);
    const uint64_t untexturedShaderId =
        ((uint64_t)SHADER_INPUT_1 << 12);

    sysLogPrintf(LOG_NOTE,
        "GfxAPI scene: bind current GS and initialise Perfect Dark rendering API baseline");
    ps2LogCheckpoint();

    gfxPs2BindGs(gs);
    api->init();

    struct ShaderProgram *texturedShader =
        api->create_and_load_new_shader(texturedShaderId, 0);
    struct ShaderProgram *untexturedShader =
        api->create_and_load_new_shader(untexturedShaderId, 0);

    if (!texturedShader || !untexturedShader) {
        sysLogPrintf(LOG_ERROR, "GfxAPI scene: failed to create baseline combiner shaders");
        ps2LogCheckpoint();
        return false;
    }

    buildCheckerTexture();
    const uint32_t textureId = api->new_texture();
    if (!textureId) {
        sysLogPrintf(LOG_ERROR, "GfxAPI scene: failed to allocate logical texture id");
        ps2LogCheckpoint();
        return false;
    }

    api->select_texture(0, textureId, false);
    api->upload_texture((const uint8_t *)sCheckerTexture,
        API_SCENE_TEXTURE_W, API_SCENE_TEXTURE_H, false);
    api->set_sampler_parameters(0, false, 2, 2, false);

    api->set_depth_range(0.0f, 1.0f);
    api->set_depth_mode(true, true, true, false, 0);
    api->set_viewport(0, 0, gs->Width, gs->Height);
    api->set_scissor(0, 0, gs->Width, gs->Height);
    api->set_use_alpha(false, false);

    buildStatusVbo(romStatus);

    sysLogPrintf(LOG_NOTE,
        "GfxAPI scene: texture=%u shader_tex=%016llx shader_color=%016llx triangles=%d",
        textureId,
        (unsigned long long)texturedShaderId,
        (unsigned long long)untexturedShaderId,
        API_SCENE_DRAW_VERTEX_COUNT / 3);
    sysLogPrintf(LOG_NOTE,
        "GfxAPI scene: entering clip-space VBO -> GfxRenderingAPI -> STQ/Z16 frame loop");
    ps2LogCheckpoint();

    const float sinStepY = 0.011999712f;
    const float cosStepY = 0.999928001f;
    const float sinStepX = 0.007999915f;
    const float cosStepX = 0.999968000f;
    float sinY = 0.0f;
    float cosY = 1.0f;
    float sinX = 0.0f;
    float cosX = 1.0f;
    const float aspect = (float)gs->Width / (float)gs->Height;

    for (;;) {
        buildCubeVbo(sinY, cosY, sinX, cosX, aspect);

        api->start_frame();
        api->clear_framebuffer(true, true);

        api->load_shader(untexturedShader);
        api->draw_triangles(sStatusVbo,
            sizeof(sStatusVbo) / sizeof(sStatusVbo[0]), 2);

        api->load_shader(texturedShader);
        api->draw_triangles(sCubeVbo,
            sizeof(sCubeVbo) / sizeof(sCubeVbo[0]), API_SCENE_DRAW_VERTEX_COUNT / 3);

        api->end_frame();

        /* Presentation remains the window-manager owner's responsibility. */
        gsKit_sync_flip(gs);

        advanceRotation(&sinY, &cosY, sinStepY, cosStepY);
        advanceRotation(&sinX, &cosX, sinStepX, cosStepX);
    }
}
