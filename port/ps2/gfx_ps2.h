#ifndef PD_PS2_GFX_PS2_H
#define PD_PS2_GFX_PS2_H

#include "gfx_rendering_api.h"
#include "gfx_ps2_capture.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fast3D backend consumes the already-initialised project GS core. */
extern struct GfxRenderingAPI gfx_ps2_api;

struct GfxRdpTmemLiveTextureView;

/* Backend-private cache variant for a physically expanded mirror period. */
static inline uint8_t gfxPs2TextureMirrorVariant(uint8_t cms, uint8_t cmt)
{
    const uint8_t mirror_s = (cms & 1u) != 0u ? 1u : 0u;
    const uint8_t mirror_t = (cmt & 1u) != 0u ? 2u : 0u;
    return mirror_s | mirror_t;
}

/*
 * Fast3D supplies normalized coordinates relative to the uploaded logical
 * extent. GS STQ normalizes against TEX0.TW/TH, whose extent is the next power
 * of two. Keep the logical last texel at the same coordinate for NPOT uploads.
 */
static inline float gfxPs2TextureCoordinateScale(
    uint32_t logical_extent, bool expanded_mirror)
{
    if (logical_extent == 0u) {
        return 1.0f;
    }

    uint32_t physical_extent = logical_extent;
    if (expanded_mirror && physical_extent <= 512u) {
        physical_extent *= 2u;
    }

    uint32_t tex0_extent = 1u;
    while (tex0_extent < physical_extent && tex0_extent < 1024u) {
        tex0_extent <<= 1u;
    }
    if (physical_extent > tex0_extent) {
        physical_extent = tex0_extent;
    }

    return (float)physical_extent / (float)tex0_extent;
}

/* Fast3D/OpenGL areas use a bottom-left origin; GS uses a top-left origin. */
static inline int gfxPs2TopLeftY(
    int target_height, int bottom_y, int area_height)
{
    return target_height - bottom_y - area_height;
}

/* OpenGL's ZMODE_DEC offset (-2,-2) moves a decal toward the camera. */
static inline int gfxPs2ApplyDecalDepthBias(int depth, bool decal)
{
    if (!decal) {
        return depth;
    }
    return depth >= 65533 ? 65535 : depth + 2;
}

static inline bool gfxPs2DepthModeAllowsEqual(
    bool depth_source_primitive, uint16_t zmode)
{
    /* RDP ZMODE_INTER=0x400 and ZMODE_DEC=0xc00. */
    return depth_source_primitive || zmode == 0x0400u || zmode == 0x0c00u;
}

static inline bool gfxPs2DepthModeIsDecal(uint16_t zmode)
{
    return zmode == 0x0c00u;
}

static inline bool gfxPs2DepthModeUsesDecalBias(
    bool depth_test, bool depth_compare, uint16_t zmode)
{
    return depth_test && depth_compare && gfxPs2DepthModeIsDecal(zmode);
}

/* Exact runtime proof used to select the one-channel material graph. */
static inline uint32_t gfxPs2MaterialRgbChannelPasses(bool monochrome_rgb)
{
    return monochrome_rgb ? 1u : 3u;
}

/* Opaque diagnostics exclude draws whose visibility depends on alpha state. */
static inline bool gfxPs2OpaqueDiagnosticSkips(
    bool alpha_blend, bool texture_edge, bool invisible)
{
    return alpha_blend || texture_edge || invisible;
}

/* Unsupported direct-material draws retain TEXEL0 alpha when available. */
static inline bool gfxPs2DirectMaterialUsesTextureAlpha(
    bool plan_supported, bool plan_texture_alpha,
    bool alpha_output, bool texture0_used)
{
    return plan_supported
        ? plan_texture_alpha
        : alpha_output && texture0_used;
}

/* Union of two normalized coverage values: B + A * (1 - B). */
static inline float gfxPs2CoverageUnion(float a, float b)
{
    return b + a * (1.0f - b);
}

static inline bool gfxPs2Rgba32IsMonochrome(
    const uint8_t *rgba32, uint32_t texel_count)
{
    if (!rgba32 && texel_count != 0u) {
        return false;
    }
    for (uint32_t i = 0u; i < texel_count; ++i) {
        const uint8_t *texel = &rgba32[i * 4u];
        if (texel[0] != texel[1] || texel[0] != texel[2]) {
            return false;
        }
    }
    return true;
}

static inline bool gfxPs2N64Rgba16IsMonochrome(
    const uint8_t *rgba5551_be, uint32_t texel_count)
{
    if (!rgba5551_be && texel_count != 0u) {
        return false;
    }
    for (uint32_t i = 0u; i < texel_count; ++i) {
        const uint16_t texel =
            ((uint16_t)rgba5551_be[i * 2u] << 8u) |
            rgba5551_be[i * 2u + 1u];
        const uint16_t r = (texel >> 11u) & 0x1fu;
        const uint16_t g = (texel >> 6u) & 0x1fu;
        const uint16_t b = (texel >> 1u) & 0x1fu;
        if (r != g || r != b) {
            return false;
        }
    }
    return true;
}

/* Decode Fast3D's normalized last-texel-centre clamp into GS texel bounds. */
static inline bool gfxPs2TextureRegionClampMax(
    float normalized_bound, uint32_t logical_extent, uint16_t *maximum)
{
    if (!maximum || logical_extent == 0u ||
        !(normalized_bound >= 0.0f)) {
        return false;
    }

    const float last_center = normalized_bound * (float)logical_extent;
    if (!(last_center >= 0.5f) || last_center > 1023.5f) {
        return false;
    }

    const uint32_t extent = (uint32_t)(last_center + 0.5f);
    if (extent == 0u || extent > 1024u) {
        return false;
    }
    *maximum = (uint16_t)(extent - 1u);
    return true;
}

/* (SHADE + ENV) >= reference becomes SHADE >= reference - ENV. */
static inline uint8_t gfxPs2TextureEdgeAdjustedReference(
    uint8_t reference, uint8_t environment)
{
    return environment >= reference
        ? 0u : (uint8_t)(reference - environment);
}

static inline uint64_t gfxPs2TextureVariantIdentity(
    uint64_t identity, uint8_t format, uint8_t cms, uint8_t cmt,
    uint32_t palette_format)
{
    const uint64_t palette_variant = format == 2u
        ? (uint64_t)((palette_format >> 14u) & 3u) : 0u;
    const uint64_t variant = gfxPs2TextureMirrorVariant(cms, cmt) |
        (palette_variant << 2u);
    identity ^= variant + UINT64_C(0x9e3779b97f4a7c15) +
        (identity << 6u) + (identity >> 2u);
    return identity;
}

/* Set the physical upload variant before the native or RGBA32 fallback path. */
void gfxPs2SetTextureUploadMirror(uint8_t cms, uint8_t cmt);

/* Emit the current renderer/VU1 counters, optionally forcing durable storage. */
void gfxPs2LogRendererStats(int checkpoint);

/*
 * PS2-only import seam used by the generated live-TMEM frontend. Returns true
 * only when the exact logical view was consumed in a native GS format.
 */
bool gfxPs2UploadTmemTexture(const struct GfxRdpTmemLiveTextureView *view,
    uint8_t format, uint8_t size, uint32_t palette_format,
    bool gen_mipmaps);

/*
 * Capture an exact live-TMEM view even when the PS2 texture cache hits an
 * already-resident GS texture. This is trace-only and does no work unless a
 * one-frame renderer capture is active.
 */
bool gfxPs2TraceTmemTextureViewWanted(void);
void gfxPs2TraceTmemTextureView(uint32_t texture_id,
    const struct GfxRdpTmemLiveTextureView *view,
    uint8_t format, uint8_t size, uint32_t palette_format);

/* Full forensic hooks used only by the generated PS2 Fast3D frontend. */
struct Ps2GsVu1TransformVertex;

/*
 * Shared-vertex fast path. When the current shader and backend state allow
 * it, gfxPs2DirectQuery() describes how the frontend turns one loaded vertex
 * into a VU1 transform input record (and sets the clip guard band); the
 * frontend then submits batches of whole, guard-band-accepted triangles
 * through gfxPs2DirectDraw(). Everything else keeps the float-VBO path.
 */
struct GfxPs2DirectContext {
    uint8_t trilerp;     /* same-tap opaque trilerp submission */
    uint8_t color_only;  /* untextured: records carry no texture coordinates */
    uint8_t fog;         /* write the fog coefficient (fog_lut) */
    uint8_t use_alpha;   /* shader parses an alpha per input */
    uint8_t num_inputs;  /* shader combiner inputs */
    int8_t rgb_input;    /* input feeding RGB through rgb_lut; -1: rgb_const */
    int8_t alpha_input;  /* input feeding A through alpha_lut; -1: alpha_const */
    uint8_t rgb_const;
    uint8_t alpha_const;
    const uint8_t *rgb_lut;    /* indexed by the input byte */
    const uint8_t *alpha_lut;
    const uint32_t *fog_lut;   /* xyz_control value per fog byte */
    float scale_s;
    float scale_t;
    float guard_x;
    float guard_y;
};

bool gfxPs2DirectQuery(struct GfxPs2DirectContext *ctx);

/*
 * Alpha-trilerp shared-vertex path: the frontend hands one vertex's VBO
 * floats to gfxPs2DirectFillAT once per (vertex slot, state), then appends
 * triangles of cached slots and submits the batch with gfxPs2DirectDrawAT.
 * gfxPs2DirectQueryAT returns 0 when the current state is not eligible.
 */
int gfxPs2DirectQueryAT(void);
/* Frontend: both texture taps use identical coordinate transforms. */
void gfxPs2DirectSetATSameCoords(bool same);
/* After gfxPs2DirectQueryAT: fills only read input 1 (shade) and tap 0. */
bool gfxPs2DirectATSameSampleLane(void);
void gfxPs2DirectUpdateGuard(float *guard_x, float *guard_y);
void gfxPs2DirectFillAT(uint32_t slot, const float *vbo_vertex);
/* The same fill from the loaded vertex's raw values: clip-space position,
 * texture coordinates, the fog byte and the combiner inputs as bytes
 * (missing inputs and alphas are 255). Bit-identical to the float path. */
struct Ps2ATSource {
    float pos[4];
    float u[2];
    float v[2];
    uint8_t fog;
    uint8_t in[3][4];
};
void gfxPs2DirectFillATSource(uint32_t slot, const struct Ps2ATSource *src);
/* Unit table (i / 255) the frontend converts bytes with, so the backend's
 * byte lookups reproduce the float path exactly. */
void gfxPs2DirectSetUnitTable(const float *u8_unit);
uint32_t gfxPs2DirectATCapacity(void);
uint32_t gfxPs2DirectATCount(void);
void gfxPs2DirectAppendAT(uint32_t a, uint32_t b, uint32_t c);
/* First of 24 scratch cache slots for clipped-polygon vertices. */
#define PS2_AT_SCRATCH_SLOT 128u
void gfxPs2DirectDrawAT(const uint8_t fog_rgb[3]);
void gfxPs2DirectDraw(const struct Ps2GsVu1TransformVertex *vertices,
    uint32_t vertex_count, bool trilerp, bool w_ok, const uint8_t fog_rgb[3]);

void gfxPs2TraceGfxCommandsImpl(
    const void *commands, uint32_t depth, uint32_t entry_count);
extern int g_ps2RendererTraceCapturing;
/* Called for every display-list command: keep the idle cost to a branch. */
static inline void gfxPs2TraceGfxCommands(
    const void *commands, uint32_t depth, uint32_t entry_count)
{
    if (g_ps2RendererTraceCapturing) {
        gfxPs2TraceGfxCommandsImpl(commands, depth, entry_count);
    }
}
void gfxPs2TraceGfxSource(uint16_t kind, const void *data,
    uint32_t size, uint64_t metadata);

#ifdef __cplusplus
}
#endif

#endif
