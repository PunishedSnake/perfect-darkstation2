#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "controls_ps2.h"
#include "gfx_cc.h"
#include "gfx_ps2.h"
#include "gfx_window_ps2.h"
#include "gs_core.h"
#include "pad_ps2.h"
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
/* clip4 + tex2 + INPUT1 rgba4, matching Fast3D MODULATEIA VBO layout */
#define API_SCENE_TEXTURED_STRIDE 10
#define API_SCENE_STATUS_VERTEX_COUNT 12
#define API_SCENE_UNTEXTURED_STRIDE 7

#if defined(PERFECT_DARK_PS2_ENABLE_UNVALIDATED_CHANNEL_BLIT)
#define API_SCENE_ALPHA_DIAG_STRIDE 16
#define API_SCENE_ALPHA_DIAG_VERTICES 6
#endif

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
static uint32_t sCheckerTexture[API_SCENE_TEXTURE_W * API_SCENE_TEXTURE_H]
    __attribute__((aligned(64)));

/*
 * Current gfx_pc.cpp batches a clip-space float VBO before handing it to the
 * rendering API. Keep the same representation here so this hardware test
 * exercises the exact backend boundary that Fast3D will use next.
 */
static float sCubeVbo[API_SCENE_DRAW_VERTEX_COUNT * API_SCENE_TEXTURED_STRIDE];
static float sStatusVbo[API_SCENE_STATUS_VERTEX_COUNT * API_SCENE_UNTEXTURED_STRIDE];

#if defined(PERFECT_DARK_PS2_ENABLE_UNVALIDATED_CHANNEL_BLIT)
static uint32_t sAlphaDiagTexture0[API_SCENE_TEXTURE_W * API_SCENE_TEXTURE_H]
    __attribute__((aligned(64)));
static uint32_t sAlphaDiagTexture1[API_SCENE_TEXTURE_W * API_SCENE_TEXTURE_H]
    __attribute__((aligned(64)));
static uint32_t sAlphaDiagReference[API_SCENE_TEXTURE_W * API_SCENE_TEXTURE_H]
    __attribute__((aligned(64)));
static float sAlphaDiagGraphVbo[
    API_SCENE_ALPHA_DIAG_VERTICES * API_SCENE_ALPHA_DIAG_STRIDE];
static float sAlphaDiagReferenceVbo[
    API_SCENE_ALPHA_DIAG_VERTICES * API_SCENE_TEXTURED_STRIDE];
#endif

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
            uint32_t pixel;

            /*
             * RGBA8 source contract: opaque texels use alpha 0xff, transparent
             * texels 0x00. The backend must normalize this through GS TCC and
             * reject the transparent checker cells via native TEST/AREF.
             */
            if (cross) {
                pixel = 0xff28d8ffu;
            } else if (checker) {
                pixel = 0xffd8d8d8u;
            } else {
                pixel = 0x00302a28u;
            }

            sCheckerTexture[y * API_SCENE_TEXTURE_W + x] = pixel;
        }
    }
}

#if defined(PERFECT_DARK_PS2_ENABLE_UNVALIDATED_CHANNEL_BLIT)
static uint32_t packRgba8(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    return (uint32_t)r | ((uint32_t)g << 8) |
        ((uint32_t)b << 16) | ((uint32_t)a << 24);
}

static uint8_t gsModulateU8(uint8_t value, uint8_t factor)
{
    return (uint8_t)(((uint32_t)value * factor) >> 7);
}

static uint8_t gsLerpU8(uint8_t a, uint8_t b, uint8_t factor)
{
    const int delta = (int)b - (int)a;
    int value = (int)a + delta * (int)factor / 128;
    if (value < 0) value = 0;
    if (value > 255) value = 255;
    return (uint8_t)value;
}

static void buildAlphaDiagTextures(void)
{
    const uint8_t shadeRgb = 96u;
    const uint8_t shadeAlpha = 48u;
    for (int y = 0; y < API_SCENE_TEXTURE_H; ++y) {
        for (int x = 0; x < API_SCENE_TEXTURE_W; ++x) {
            const uint8_t rampX = (uint8_t)(x * 255 / (API_SCENE_TEXTURE_W - 1));
            const uint8_t rampY = (uint8_t)(y * 255 / (API_SCENE_TEXTURE_H - 1));
            const uint8_t checker = ((x >> 3) ^ (y >> 3)) & 1 ? 224u : 32u;
            const uint8_t alpha0 = (uint8_t)((x * 3 + y * 5) & 0xff);
            const uint8_t alpha1 = (uint8_t)(255u - ((x * 7 + y) & 0xff));
            const uint8_t lod = (uint8_t)(x * 128 / (API_SCENE_TEXTURE_W - 1));
            const uint8_t t0[4] = { rampX, checker, rampY, alpha0 };
            const uint8_t t1[4] = {
                (uint8_t)(255u - rampY),
                rampX,
                (uint8_t)(255u - checker),
                alpha1,
            };
            uint8_t result[4];
            for (int channel = 0; channel < 3; ++channel) {
                result[channel] = gsLerpU8(
                    gsModulateU8(t0[channel], shadeRgb),
                    gsModulateU8(t1[channel], shadeRgb), lod);
            }
            const uint8_t alpha = gsLerpU8(
                gsModulateU8(t0[3], shadeAlpha),
                gsModulateU8(t1[3], shadeAlpha), lod);
            result[3] = alpha > 127u ? 255u : (uint8_t)(alpha * 2u);

            const int index = y * API_SCENE_TEXTURE_W + x;
            sAlphaDiagTexture0[index] =
                packRgba8(t0[0], t0[1], t0[2], t0[3]);
            sAlphaDiagTexture1[index] =
                packRgba8(t1[0], t1[1], t1[2], t1[3]);
            sAlphaDiagReference[index] = packRgba8(
                result[0], result[1], result[2], result[3]);
        }
    }
}

static void writeAlphaDiagGraphVertex(float **dst, float x, float y,
    float u, float v, float lod)
{
    float *p = *dst;
    *p++ = x; *p++ = y; *p++ = 0.25f; *p++ = 1.0f;
    *p++ = u; *p++ = v;
    *p++ = u; *p++ = v;
    *p++ = lod; *p++ = lod; *p++ = lod; *p++ = lod;
    *p++ = 0.75f; *p++ = 0.75f; *p++ = 0.75f; *p++ = 0.75f;
    *dst = p;
}

static void writeAlphaDiagReferenceVertex(float **dst, float x, float y,
    float u, float v)
{
    float *p = *dst;
    *p++ = x; *p++ = y; *p++ = 0.25f; *p++ = 1.0f;
    *p++ = u; *p++ = v;
    *p++ = 1.0f; *p++ = 1.0f; *p++ = 1.0f; *p++ = 1.0f;
    *dst = p;
}

static void buildAlphaDiagVbos(void)
{
    static const uint8_t indices[6] = { 0, 1, 2, 0, 2, 3 };
    static const float uv[4][2] = {
        { 0.0f, 1.0f }, { 1.0f, 1.0f },
        { 1.0f, 0.0f }, { 0.0f, 0.0f },
    };
    static const float graphPosition[4][2] = {
        { -0.92f, -0.62f }, { -0.06f, -0.62f },
        { -0.06f,  0.62f }, { -0.92f,  0.62f },
    };
    static const float referencePosition[4][2] = {
        { 0.06f, -0.62f }, { 0.92f, -0.62f },
        { 0.92f,  0.62f }, { 0.06f,  0.62f },
    };
    float *graph = sAlphaDiagGraphVbo;
    float *reference = sAlphaDiagReferenceVbo;
    for (int i = 0; i < 6; ++i) {
        const uint8_t index = indices[i];
        writeAlphaDiagGraphVertex(&graph,
            graphPosition[index][0], graphPosition[index][1],
            uv[index][0], uv[index][1], uv[index][0]);
        writeAlphaDiagReferenceVertex(&reference,
            referencePosition[index][0], referencePosition[index][1],
            uv[index][0], uv[index][1]);
    }
}

static uint64_t alphaDiagShaderId(void)
{
    return
        ((uint64_t)SHADER_TEXEL1 << 0) |
        ((uint64_t)SHADER_TEXEL0 << 4) |
        ((uint64_t)SHADER_INPUT_1 << 8) |
        ((uint64_t)SHADER_TEXEL0 << 12) |
        ((uint64_t)SHADER_TEXEL1 << 16) |
        ((uint64_t)SHADER_TEXEL0 << 20) |
        ((uint64_t)SHADER_INPUT_1 << 24) |
        ((uint64_t)SHADER_TEXEL0 << 28) |
        ((uint64_t)SHADER_COMBINED << 32) |
        ((uint64_t)SHADER_INPUT_2 << 40) |
        ((uint64_t)SHADER_COMBINED << 48) |
        ((uint64_t)SHADER_INPUT_2 << 56);
}
#endif

static struct api_scene_clip_vertex projectVertex(
    const struct api_scene_vec3 *source,
    float sinY,
    float cosY,
    float sinX,
    float cosX,
    float aspect,
    float offsetX,
    float cameraDistance)
{
    const float rx = source->x * cosY + source->z * sinY + offsetX;
    const float rz0 = -source->x * sinY + source->z * cosY;
    const float ry = source->y * cosX - rz0 * sinX;
    const float rz = source->y * sinX + rz0 * cosX;
    const float cameraZ = rz + cameraDistance;

    const float focalY = 2.36f;
    const float focalX = focalY / aspect;
    const float nearZ = 2.5f;
    const float farZ = 7.5f;
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
    *p++ = u;
    *p++ = v;
    *p++ = (float)face->r / 128.0f;
    *p++ = (float)face->g / 128.0f;
    *p++ = (float)face->b / 128.0f;
    *p++ = 1.0f;

    *dst = p;
}

static void buildCubeVbo(
    float sinY,
    float cosY,
    float sinX,
    float cosX,
    float aspect,
    float offsetX,
    float cameraDistance)
{
    struct api_scene_clip_vertex projected[API_SCENE_VERTEX_COUNT];
    float *dst = sCubeVbo;

    for (int i = 0; i < API_SCENE_VERTEX_COUNT; ++i) {
        projected[i] = projectVertex(
            &kCubeVertices[i], sinY, cosY, sinX, cosX, aspect, offsetX, cameraDistance);
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

static void writeStatusQuad(
    float **dst, float y0, float y1, float r, float g, float b)
{
    writeUntexturedVertex(dst, -0.78f, y0, 0.96f, r, g, b);
    writeUntexturedVertex(dst,  0.78f, y0, 0.96f, r, g, b);
    writeUntexturedVertex(dst,  0.78f, y1, 0.96f, r, g, b);
    writeUntexturedVertex(dst, -0.78f, y0, 0.96f, r, g, b);
    writeUntexturedVertex(dst,  0.78f, y1, 0.96f, r, g, b);
    writeUntexturedVertex(dst, -0.78f, y1, 0.96f, r, g, b);
}

static void buildStatusVbo(int romStatus, const struct Ps2ShooterControls *controls)
{
    float *dst = sStatusVbo;
    float romR = 0.95f;
    float romG = 0.65f;
    float romB = 0.15f;

    if (romStatus > 0) {
        romR = 0.25f;
        romG = 0.95f;
        romB = 0.50f;
    } else if (romStatus < 0) {
        romR = 1.0f;
        romG = 0.25f;
        romB = 0.25f;
    }

    writeStatusQuad(&dst, -0.84f, -0.79f, romR, romG, romB);

    float inputR = 0.18f;
    float inputG = 0.18f;
    float inputB = 0.22f;
    if (controls && controls->connected) {
        inputR = 0.20f;
        inputG = 0.80f;
        inputB = 0.95f;

        if (controls->held & PS2_ACTION_AIM) {
            inputR = 0.30f;
            inputG = 0.50f;
            inputB = 1.0f;
        }
        if (controls->held & PS2_ACTION_ALT_FIRE) {
            inputR = 0.95f;
            inputG = 0.25f;
            inputB = 0.85f;
        }
        if (controls->held & PS2_ACTION_FIRE) {
            inputR = 1.0f;
            inputG = 0.18f;
            inputB = 0.12f;
        }
    }

    writeStatusQuad(&dst, -0.73f, -0.68f, inputR, inputG, inputB);
}

static void advanceRotation(float *sinValue, float *cosValue, float sinStep, float cosStep)
{
    const float oldSin = *sinValue;
    const float oldCos = *cosValue;

    *sinValue = oldSin * cosStep + oldCos * sinStep;
    *cosValue = oldCos * cosStep - oldSin * sinStep;
}

static void advanceRotationDelta(float *sinValue, float *cosValue, float delta)
{
    delta = clampfLocal(delta, -0.04f, 0.04f);
    const float sinStep = delta;
    const float cosStep = 1.0f - 0.5f * delta * delta;
    advanceRotation(sinValue, cosValue, sinStep, cosStep);

    /* Cheap near-unit renormalisation. One Newton step is enough for tiny deltas. */
    const float length2 = *sinValue * *sinValue + *cosValue * *cosValue;
    const float correction = 1.5f - 0.5f * length2;
    *sinValue *= correction;
    *cosValue *= correction;
}

bool ps2GfxApiSceneRun(int romStatus)
{
    if (!ps2GsCoreIsReady()) {
        return false;
    }

    const int width = ps2GsCoreGetWidth();
    const int height = ps2GsCoreGetHeight();
    if (width <= 0 || height <= 0) {
        return false;
    }

    struct GfxRenderingAPI *api = &gfx_ps2_api;
    struct GfxWindowManagerAPI *wapi = &gfx_window_ps2_api;
    /* G_CC_MODULATEIA: RGB=TEXEL0*INPUT1, A=TEXEL0*INPUT1. */
    const uint64_t texturedShaderId =
        ((uint64_t)SHADER_TEXEL0 << 0) |
        ((uint64_t)SHADER_INPUT_1 << 8) |
        ((uint64_t)SHADER_TEXEL0 << 16) |
        ((uint64_t)SHADER_INPUT_1 << 24);
    const uint32_t texturedShaderOptions =
        SHADER_OPT_ALPHA | SHADER_OPT_ALPHA_THRESHOLD;
    const uint64_t untexturedShaderId =
        ((uint64_t)SHADER_INPUT_1 << 12);
    const struct GfxWindowInitSettings windowSettings = {
        "Perfect DarkStation 2",
        (uint32_t)width,
        (uint32_t)height,
        0,
        0,
        true,
        true,
        true,
        false,
        false,
    };

    sysLogPrintf(LOG_NOTE,
        "GfxAPI scene: initialise Fast3D PS2 backend against GS core");
    ps2LogCheckpoint();

    wapi->init(&windowSettings);
    api->init();

    struct ShaderProgram *texturedShader =
        api->create_and_load_new_shader(texturedShaderId, texturedShaderOptions);
    struct ShaderProgram *untexturedShader =
        api->create_and_load_new_shader(untexturedShaderId, 0);

#if defined(PERFECT_DARK_PS2_ENABLE_UNVALIDATED_CHANNEL_BLIT)
    const uint64_t alphaDiagId = alphaDiagShaderId();
    const uint32_t alphaDiagOptions = SHADER_OPT_2CYC | SHADER_OPT_ALPHA;
    struct ShaderProgram *alphaDiagShader =
        api->create_and_load_new_shader(alphaDiagId, alphaDiagOptions);
    struct ShaderProgram *alphaDiagReferenceShader =
        api->create_and_load_new_shader(texturedShaderId, SHADER_OPT_ALPHA);
#endif

    if (!texturedShader || !untexturedShader
#if defined(PERFECT_DARK_PS2_ENABLE_UNVALIDATED_CHANNEL_BLIT)
        || !alphaDiagShader || !alphaDiagReferenceShader
#endif
    ) {
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

#if defined(PERFECT_DARK_PS2_ENABLE_UNVALIDATED_CHANNEL_BLIT)
    buildAlphaDiagTextures();
    buildAlphaDiagVbos();
    const uint32_t alphaDiagTexture0 = api->new_texture();
    const uint32_t alphaDiagTexture1 = api->new_texture();
    const uint32_t alphaDiagReferenceTexture = api->new_texture();
    if (!alphaDiagTexture0 || !alphaDiagTexture1 ||
        !alphaDiagReferenceTexture) {
        sysLogPrintf(LOG_ERROR,
            "GfxAPI scene: failed to allocate alpha-trilerp A/B textures");
        ps2LogCheckpoint();
        return false;
    }

    api->select_texture(0, alphaDiagTexture0, false);
    api->upload_texture((const uint8_t *)sAlphaDiagTexture0,
        API_SCENE_TEXTURE_W, API_SCENE_TEXTURE_H, false);
    api->set_sampler_parameters(0, false, 2, 2, false);
    api->select_texture(0, alphaDiagTexture1, false);
    api->upload_texture((const uint8_t *)sAlphaDiagTexture1,
        API_SCENE_TEXTURE_W, API_SCENE_TEXTURE_H, false);
    api->set_sampler_parameters(0, false, 2, 2, false);
    api->select_texture(0, alphaDiagReferenceTexture, false);
    api->upload_texture((const uint8_t *)sAlphaDiagReference,
        API_SCENE_TEXTURE_W, API_SCENE_TEXTURE_H, false);
    api->set_sampler_parameters(0, false, 2, 2, false);
    api->select_texture(0, textureId, false);
#endif

    api->set_depth_range(0.0f, 1.0f);
    api->set_depth_mode(true, true, true, false, 0);
    api->set_viewport(0, 0, width, height);
    api->set_scissor(0, 0, width, height);
    api->set_use_alpha(true, false);

    sysLogPrintf(LOG_NOTE,
        "GfxAPI scene: texture=%u shader_tex=%016llx/%08x shader_color=%016llx triangles=%d",
        textureId,
        (unsigned long long)texturedShaderId,
        (unsigned int)texturedShaderOptions,
        (unsigned long long)untexturedShaderId,
        API_SCENE_DRAW_VERTEX_COUNT / 3);
    sysLogPrintf(LOG_NOTE,
        "GfxAPI scene: MODULATEIA alpha-cutout smoke expects transparent checker cells to be discarded");
    sysLogPrintf(LOG_NOTE,
        "GfxAPI scene: DS2 smoke right=rotate left=move/zoom R1=fire+rumble L1=precision Cross=reset");
#if defined(PERFECT_DARK_PS2_ENABLE_UNVALIDATED_CHANNEL_BLIT)
    sysLogPrintf(LOG_WARNING,
        "GfxAPI scene: Select toggles unvalidated alpha-trilerp A/B; left=GS graph right=CPU reference");
#endif
    sysLogPrintf(LOG_NOTE,
        "GfxAPI scene: entering clip-space VBO -> Fast3D adapter -> GS core frame loop");
    ps2LogCheckpoint();

    const float sinStepY = 0.011999712f;
    const float cosStepY = 0.999928001f;
    const float sinStepX = 0.007999915f;
    const float cosStepX = 0.999968000f;
    float sinY = 0.0f;
    float cosY = 1.0f;
    float sinX = 0.0f;
    float cosX = 1.0f;
    float offsetX = 0.0f;
    float cameraDistance = 4.6f;
    const float aspect = (float)width / (float)height;
#if defined(PERFECT_DARK_PS2_ENABLE_UNVALIDATED_CHANNEL_BLIT)
    /*
     * A diagnostic binary must identify itself without relying on controller
     * input.  Start on the A/B screen and let Select return to the baseline
     * cube.  This also makes accidentally launching the ordinary bootstrap
     * immediately obvious on real hardware.
     */
    bool alphaDiagActive = true;
#endif

    for (;;) {
        wapi->handle_events();
        if (!wapi->start_frame()) {
            continue;
        }

        /*
         * Input is sampled at the beginning of the frame and consumed by this
         * frame's render state. Do not add a render-ahead queue to a controller
         * correctness test unless a later measured scheduler requires one.
         */
        ps2ShooterControlsUpdate();
        const struct Ps2ShooterControls *controls = ps2ShooterControlsGet(0);

        if (controls && controls->connected) {
#if defined(PERFECT_DARK_PS2_ENABLE_UNVALIDATED_CHANNEL_BLIT)
            if (controls->pressed & PS2_ACTION_DEBUG) {
                alphaDiagActive = !alphaDiagActive;
                sysLogPrintf(LOG_WARNING,
                    "GfxAPI alpha-trilerp A/B %s: left=GS graph right=CPU reference",
                    alphaDiagActive ? "enabled" : "disabled");
                ps2LogCheckpoint();
            }
#endif
            if (controls->pressed & PS2_ACTION_USE) {
                sinY = 0.0f;
                cosY = 1.0f;
                sinX = 0.0f;
                cosX = 1.0f;
                offsetX = 0.0f;
                cameraDistance = 4.6f;
            }

            const float lookScale = (controls->held & PS2_ACTION_AIM) ? 0.010f : 0.024f;
            advanceRotationDelta(&sinY, &cosY, controls->look_x * lookScale);
            advanceRotationDelta(&sinX, &cosX, controls->look_y * lookScale);

            offsetX = clampfLocal(offsetX + controls->move_x * 0.025f, -1.4f, 1.4f);
            cameraDistance = clampfLocal(
                cameraDistance - controls->move_y * 0.035f, 3.8f, 6.5f);

            uint8_t rumble = 0;
            if (controls->held & PS2_ACTION_ALT_FIRE) {
                rumble = 180;
            }
            if (controls->held & PS2_ACTION_FIRE) {
                rumble = 96;
            }
            ps2PadSetRumble(0, 0, rumble);
        } else {
            /* Preserve the old unattended graphics smoke when no pad is present. */
            advanceRotation(&sinY, &cosY, sinStepY, cosStepY);
            advanceRotation(&sinX, &cosX, sinStepX, cosStepX);
        }

        buildStatusVbo(romStatus, controls);
        buildCubeVbo(sinY, cosY, sinX, cosX, aspect, offsetX, cameraDistance);

        api->start_frame();
        api->set_depth_mode(true, true, true, false, 0);
        api->clear_framebuffer(true, true);

        api->load_shader(untexturedShader);
        api->draw_triangles(sStatusVbo,
            sizeof(sStatusVbo) / sizeof(sStatusVbo[0]), API_SCENE_STATUS_VERTEX_COUNT / 3);

#if defined(PERFECT_DARK_PS2_ENABLE_UNVALIDATED_CHANNEL_BLIT)
        if (alphaDiagActive) {
            api->set_depth_mode(false, false, false, false, 0);
            api->set_use_alpha(true, false);
            api->select_texture(0, alphaDiagTexture0, false);
            api->set_sampler_parameters(0, false, 2, 2, false);
            api->select_texture(1, alphaDiagTexture1, false);
            api->set_sampler_parameters(1, false, 2, 2, false);
            api->load_shader(alphaDiagShader);
            api->draw_triangles(sAlphaDiagGraphVbo,
                sizeof(sAlphaDiagGraphVbo) / sizeof(sAlphaDiagGraphVbo[0]),
                API_SCENE_ALPHA_DIAG_VERTICES / 3);

            api->select_texture(0, alphaDiagReferenceTexture, false);
            api->set_sampler_parameters(0, false, 2, 2, false);
            api->load_shader(alphaDiagReferenceShader);
            api->draw_triangles(sAlphaDiagReferenceVbo,
                sizeof(sAlphaDiagReferenceVbo) /
                    sizeof(sAlphaDiagReferenceVbo[0]),
                API_SCENE_ALPHA_DIAG_VERTICES / 3);
        } else
#endif
        {
            api->select_texture(0, textureId, false);
            api->set_sampler_parameters(0, false, 2, 2, false);
            api->load_shader(texturedShader);
            api->draw_triangles(sCubeVbo,
                sizeof(sCubeVbo) / sizeof(sCubeVbo[0]),
                API_SCENE_DRAW_VERTEX_COUNT / 3);
        }

        api->end_frame();
        wapi->swap_buffers_begin();
        api->finish_render();
        wapi->swap_buffers_end();
    }
}
