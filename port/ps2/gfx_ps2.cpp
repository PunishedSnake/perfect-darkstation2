#include <stdbool.h>
#include <float.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef PD_PS2_GIT_COMMIT
#define PD_PS2_GIT_COMMIT "unknown"
#endif

#include "gfx_cc.h"
#include "gfx_rendering_api.h"
#include "gfx_ps2.h"
#include "gfx_ps2_combiner.h"
#include "gfx_ps2_pass_graph.h"
#include "gs_clip.h"
#include "gs_core.h"
#include "gs_vu1_batch.h"
#include "gs_vu1_transform.h"
#include "log_ps2.h"
#include "ps2_renderer_stats.h"
#include "renderer_trace.h"
#include "rdp_tmem_live.h"
#include "system.h"

/*
 * Perfect Dark Fast3D compatibility adapter for PS2.
 *
 * The adapter accepts the clip-space VBO contract produced by current
 * port/fast3d/gfx_pc.cpp and translates the supported material subset into
 * GS-ready vertices plus opaque GS texture handles. Device lifetime, frame
 * ownership, GS register state, texture residency and primitive submission live
 * below this file in gs_core.
 *
 * Color and alpha equations are classified independently. Exact two-cycle
 * equations whose final channel is independent of COMBINED, or is a pure
 * COMBINED pass-through, are reduced to the corresponding live cycle. Other
 * two-cycle equations remain rejected until their GS multipass plan exists.
 * gfx_pc.cpp already resolves SHADE/PRIMITIVE/ENVIRONMENT/etc. into compact
 * INPUTn VBO channels, so INPUT1 is intentionally semantic-agnostic here.
 *
 * Current exact fixed-function color recipes:
 *   - INPUT1
 *   - TEXEL0
 *   - TEXEL0 * INPUT1
 *   - lerp(TEXEL0, TEXEL1, INPUT1), optionally multiplied by INPUT2,
 *     reconstructed as two ordered GS passes for opaque output
 *   - lerp(INPUT1, TEXEL0, INPUT2), optionally multiplied by INPUT3,
 *     reconstructed as a solid base plus textured GS pass for opaque output
 *   - lerp(INPUT2, INPUT1, TEXEL0), with independent alpha, reconstructed
 *     channel-wise through two tiled CT32 targets for BLENDIA/CUSTOM_27
 *   - TEXEL0 * TEXEL1 * INPUT1, including independent texture alpha,
 *     reconstructed through the same two CT32 targets for INTERFERENCE
 *   - INPUT1 RGB with TEXEL0 or TEXEL0 * INPUT1 alpha, reconstructed in one
 *     tiled CT32 target so texture RGB cannot contaminate the color equation
 *
 * Current exact alpha recipes:
 *   - opaque/one/zero
 *   - INPUT1
 *   - TEXEL0
 *   - TEXEL0 * INPUT1
 *   - INPUT2 * INPUT1 * (1 - INPUT1), reconstructed through GS ALPHA
 *   - INPUT3 + TEXEL0 * (INPUT1 - INPUT2), reduced to signed TEX_EDGE tests
 *   - lerp(INPUT2, INPUT1, TEXEL0), reconstructed in a scalar alpha lane
 *   - TEXEL1 * INPUT1 beside trilerped RGB, captured by an alpha-only draw
 *   - trilerped alpha * INPUT2 + INPUT3, with the additive term accumulated
 *     in the tiled scalar target before final GS alpha normalization
 *
 * Alpha-bearing TEXEL0/TEXEL1 trilerp has a tiled CT32 execution graph. Its
 * low-lane channel shuffle passed the deterministic image A/B on physical PS2
 * hardware and is available to ordinary gameplay builds.
 *
 * GS texture MODULATE uses 0x80 as unity and multiplies with >>7. RGBA32 alpha
 * remains in 0..255, while native PSMCT16 alpha is expanded to the same range
 * through TEXA. When texture alpha participates (TCC=RGBA), fragment alpha is
 * therefore scaled to 0..0x40 so MODULATE returns to the GS-native 0..0x80
 * range without expanding every RGBA16 texture to RGBA32.
 *
 * Fog maps to the GS native FOGCOL + per-vertex XYZF2 path. Fast3D's factor is
 * the fog contribution, while GS F is the source-color contribution, so the
 * conversion is F = 255 * (1 - factor).
 *
 * Unsupported combiners are retained in the shader table so shader_get_info()
 * still reports the exact upstream VBO layout, but draw submission rejects the
 * unsupported recipe rather than silently rendering a wrong approximation.
 * Rejected batches and triangles are counted, and the first rejection forces a
 * durable log checkpoint for retail-hardware diagnosis.
 */

#define PS2_GFX_MAX_SHADERS 128
#if defined(PERFECT_DARK_PS2_VU1_COLOR_BATCH)
#define PS2_GFX_TRANSLATE_VERTS PS2_GS_VU1_MAX_TEXTURED_VERTICES
#else
#define PS2_GFX_TRANSLATE_VERTS 96
#endif
#define PS2_GFX_TEXTURE_STATE_SLOTS 65
/* Fast3D threshold is 8/256. GS alpha unity is 0x80, hence reference 4. */
#define PS2_GFX_ALPHA_THRESHOLD 4u
/* Portable texture-edge threshold is >0.19; quantized GS alpha accepts >=25. */
#define PS2_GFX_TEXTURE_EDGE_THRESHOLD 25u
#define PS2_GFX_N64_FMT_RGBA 0u
#define PS2_GFX_N64_FMT_CI 2u
#define PS2_GFX_N64_FMT_IA 3u
#define PS2_GFX_N64_FMT_I 4u
#define PS2_GFX_N64_SIZ_4B 0u
#define PS2_GFX_N64_SIZ_8B 1u
#define PS2_GFX_N64_SIZ_16B 2u
#define PS2_GFX_N64_SIZ_32B 3u
#define PS2_GFX_N64_TT_RGBA16 (2u << 14)
#define PS2_GFX_N64_TT_IA16 (3u << 14)

static_assert(PS2_GFX_TRANSLATE_VERTS >=
    PS2_GS_CLIP_MAX_OUTPUT_VERTICES,
    "one clipped triangle must fit in the translation batch");

/* GS packed-register IDs consumed by the packet-ready core boundary. */
#define PS2_GS_REG_RGBAQ 0x01u
#define PS2_GS_REG_ST    0x02u
#define PS2_GS_REG_XYZF2 0x04u
#define PS2_GS_REG_XYZ2  0x05u

struct ShaderProgram {
    bool used;
    bool warned_rejected_draw;
    uint64_t shader_id0;
    uint32_t shader_id1;
    struct CCFeatures features;
    struct Ps2CombinerPlan plan;
};

struct Ps2Viewport {
    int x;
    int y;
    int width;
    int height;
};

struct Ps2TextureSamplerState {
    uint32_t cms;
    uint32_t cmt;
    uint32_t logical_width;
    uint32_t logical_height;
    float coordinate_scale_s;
    float coordinate_scale_t;
    bool expanded_mirror_s;
    bool expanded_mirror_t;
    bool monochrome_rgb;
    uint8_t source_format;
    uint8_t source_size;
    uint16_t source_reserved;
    uint32_t palette_format;
    uint32_t palette_count;
    uint32_t upload_serial;
    uint64_t source_hash;
    uint64_t palette_hash;
    uint64_t content_identity;
};

struct Ps2TextureRegionClampState {
    bool region_s;
    bool region_t;
    uint16_t max_u;
    uint16_t max_v;
};

struct Ps2AlphaTrilerpVertex {
    float x;
    float y;
    float inv_w;
    float tex_u[2];
    float tex_v[2];
    int z;
    uint8_t shade_r;
    uint8_t shade_g;
    uint8_t shade_b;
    uint8_t shade_a;
    uint8_t lod;
    uint8_t independent_alpha;
    uint8_t fog;
    uint8_t primitive_alpha;
    uint8_t alpha_add;
    uint8_t final_alpha;
    uint8_t direct_alpha;
    float signed_alpha_delta;
};

struct Ps2Tex0FactorLerpVertex {
    float x;
    float y;
    float inv_w;
    float tex_u[2];
    float tex_v[2];
    int z;
    uint8_t input1[4];
    uint8_t input2[4];
    uint8_t tex0_alpha_input;
    uint8_t fog;
};

struct Ps2IndependentTex0AlphaVertex {
    float x;
    float y;
    float inv_w;
    float tex_u;
    float tex_v;
    int z;
    uint8_t input[4];
    uint8_t fog;
};

struct Ps2InterferenceVertex {
    float x;
    float y;
    float inv_w;
    float tex_u[2];
    float tex_v[2];
    int z;
    uint8_t shade[4];
    uint8_t fog;
};

static struct ShaderProgram s_shaders[PS2_GFX_MAX_SHADERS];
static struct ShaderProgram *s_shader;
static Ps2GsTextureHandle s_selected_texture[2];
static int s_active_texture_tile;
static struct Ps2Viewport s_viewport;
static struct Ps2Viewport s_scissor;
static float s_depth_near = 0.0f;
static float s_depth_far = 1.0f;
static bool s_depth_test = true;
static bool s_depth_update = true;
static bool s_depth_compare = true;
static bool s_depth_compare_equal;
static bool s_depth_decal;
static bool s_alpha_blend;
static bool s_modulate;
static uint32_t s_sampler_cms[2];
static uint32_t s_sampler_cmt[2];
static bool s_sampler_linear[2];
static struct Ps2TextureSamplerState
    s_texture_sampler[PS2_GFX_TEXTURE_STATE_SLOTS];
static struct Ps2TextureRegionClampState s_draw_region_clamp[2];
static enum FilteringMode s_filter_mode = FILTER_LINEAR;
static enum MipmapFilteringMode s_mipmap_filter = MIPMAP_DISABLED;
static int s_anisotropy = 1;
static uint32_t s_trace_draw_id;
static uint32_t s_texture_upload_serial;
static uint64_t s_trace_tmem_snapshot_identity[PS2_GFX_TEXTURE_STATE_SLOTS];

static bool s_warned_framebuffer;
static bool s_warned_mipmap;
static bool s_warned_mirror_extent;
static bool s_warned_region_clamp_extent;
static bool s_upload_mirror_s;
static bool s_upload_mirror_t;
static bool s_logged_native_rgba16;
static bool s_logged_native_rgba32;
static bool s_logged_native_ia16;
static bool s_logged_native_mirror;
static bool s_logged_native_ci4;
static bool s_logged_native_ci8;
static bool s_logged_native_ci_ia16[2];
static bool s_logged_native_intensity[4];
static bool s_warned_alpha_trilerp_workspace;
static bool s_warned_alpha_trilerp_modulate;
static bool s_warned_independent_alpha_workspace;
static bool s_warned_independent_alpha_modulate;
static bool s_warned_tex0_factor_workspace;
static bool s_warned_tex0_factor_modulate;
static bool s_logged_tex0_factor_scalar;
static bool s_logged_tex0_factor_vector;
static bool s_warned_interference_workspace;
static bool s_warned_interference_modulate;
static bool s_logged_interference_scalar;
static bool s_logged_interference_vector;
static bool s_warned_independent_tex0_alpha_workspace;
static bool s_warned_independent_tex0_alpha_modulate;
static bool s_checkpointed_unsupported_shader;
static bool s_pending_unsupported_shader_checkpoint;
static Ps2GsRenderTargetHandle s_alpha_trilerp_color_target;
static Ps2GsRenderTargetHandle s_alpha_trilerp_scalar_target;
static uint8_t s_draw_fog_r;
static uint8_t s_draw_fog_g;
static uint8_t s_draw_fog_b;
static uint8_t s_draw_texture_edge_reference =
    PS2_GFX_TEXTURE_EDGE_THRESHOLD;
static uint64_t s_perf_renderer_build_start_us;
static bool s_perf_renderer_build_active;

static struct Ps2GsTexturedVertex s_stq_vertices[2][PS2_GFX_TRANSLATE_VERTS];
static struct Ps2GsColorVertex s_color_vertices[PS2_GFX_TRANSLATE_VERTS];
#if defined(PERFECT_DARK_PS2_GEOMETRY_BASELINE) || \
    defined(PERFECT_DARK_PS2_MATERIAL_BASELINE)
static const uint8_t s_geometry_baseline_palette[][3] = {
    { 0x80u, 0x20u, 0x20u },
    { 0x20u, 0x80u, 0x20u },
    { 0x20u, 0x20u, 0x80u },
    { 0x80u, 0x80u, 0x20u },
    { 0x20u, 0x80u, 0x80u },
    { 0x80u, 0x20u, 0x80u },
};
#endif
#if defined(PERFECT_DARK_PS2_VU1_COLOR_BATCH)
static struct Ps2GsVu1TransformVertex
    s_vu1_transform_vertices[PS2_GFX_TRANSLATE_VERTS];
#endif
static struct Ps2AlphaTrilerpVertex
    s_alpha_trilerp_vertices[PS2_GFX_TRANSLATE_VERTS];
static struct Ps2Tex0FactorLerpVertex
    s_tex0_factor_vertices[PS2_GFX_TRANSLATE_VERTS];
static struct Ps2IndependentTex0AlphaVertex
    s_independent_tex0_alpha_vertices[PS2_GFX_TRANSLATE_VERTS];
static struct Ps2InterferenceVertex
    s_interference_vertices[PS2_GFX_TRANSLATE_VERTS];
static float s_clipped_vbo[
    PS2_GFX_TRANSLATE_VERTS * PS2_GS_CLIP_MAX_VERTEX_FLOATS];

static const char *ps2_get_name(void)
{
    return "PlayStation 2 GS";
}

static int ps2_get_max_texture_size(void)
{
    /* GS texture coordinates and TEX0 support up to 1024 texels per axis. */
    return 1024;
}

static struct GfxClipParameters ps2_get_clip_parameters(void)
{
    /*
     * Request the current Fast3D path to remap clip Z from [-W,+W] to [0,W].
     * Y remains in the upstream convention and is converted to GS top-down
     * screen coordinates during clip->viewport translation below.
     */
    struct GfxClipParameters params = { true, false };
    return params;
}

static void ps2_log_shader_recipe(int slot, const struct ShaderProgram *prg)
{
    const struct CCFeatures *f = &prg->features;

    const struct Ps2CombinerPlan *plan = &prg->plan;
    const uint8_t color_cycle = plan->color_cycle;
    const uint8_t alpha_cycle = plan->alpha_cycle;

    sysLogPrintf(plan->supported ? LOG_NOTE : LOG_WARNING,
        "GfxPS2 shader %02d id=%016llx/%08x supported=%d tex=%d%d inputs=%d "
        "cycle=%u/%u recipe=%d/%d graph=%d gate=%d tcc=%d rgb=[%u,%u,%u,%u] a=[%u,%u,%u,%u] "
        "opts=a%d f%d e%d n%d 2c%d at%d inv%d g%d b%d",
        slot,
        (unsigned long long)prg->shader_id0,
        (unsigned int)prg->shader_id1,
        plan->supported ? 1 : 0,
        f->used_textures[0] ? 1 : 0,
        f->used_textures[1] ? 1 : 0,
        f->num_inputs,
        color_cycle,
        alpha_cycle,
        (int)plan->color_recipe,
        (int)plan->alpha_recipe,
        (int)plan->pass_graph,
        plan->hardware_validation_required ? 1 : 0,
        plan->texture_alpha ? 1 : 0,
        f->c[color_cycle][0][0], f->c[color_cycle][0][1],
        f->c[color_cycle][0][2], f->c[color_cycle][0][3],
        f->c[alpha_cycle][1][0], f->c[alpha_cycle][1][1],
        f->c[alpha_cycle][1][2], f->c[alpha_cycle][1][3],
        f->opt_alpha ? 1 : 0,
        f->opt_fog ? 1 : 0,
        f->opt_texture_edge ? 1 : 0,
        f->opt_noise ? 1 : 0,
        f->opt_2cyc ? 1 : 0,
        f->opt_alpha_threshold ? 1 : 0,
        f->opt_invisible ? 1 : 0,
        f->opt_grayscale ? 1 : 0,
        f->opt_blur ? 1 : 0);
}

static struct ShaderProgram *ps2_lookup_shader(uint64_t shader_id0, uint32_t shader_id1)
{
    for (int i = 0; i < PS2_GFX_MAX_SHADERS; ++i) {
        if (s_shaders[i].used && s_shaders[i].shader_id0 == shader_id0 &&
            s_shaders[i].shader_id1 == shader_id1) {
            return &s_shaders[i];
        }
    }
    return NULL;
}

static void ps2_unload_shader(struct ShaderProgram *old_prg)
{
    (void)old_prg;
}

static void ps2_trace_shader(const struct ShaderProgram *prg)
{
    if (prg) {
        const uint16_t trace_flags =
            (prg->plan.supported ?
                (uint16_t)PS2_TRACE_FLAG_SUPPORTED : 0u) |
            (prg->plan.textured ?
                (uint16_t)PS2_TRACE_FLAG_TEXTURED : 0u);
        const uint64_t recipes =
            (uint64_t)(uint32_t)prg->plan.color_recipe |
            ((uint64_t)(uint32_t)prg->plan.alpha_recipe << 16u) |
            ((uint64_t)(uint32_t)prg->plan.pass_graph << 32u);
        ps2RendererTraceRecord(PS2_TRACE_SHADER, trace_flags,
            prg->shader_id0, prg->shader_id1, recipes,
            (uint64_t)prg->features.num_inputs);
    }
}

static void ps2_load_shader(struct ShaderProgram *new_prg)
{
    s_shader = new_prg;
    ps2_trace_shader(new_prg);

    const bool threshold = new_prg && new_prg->plan.supported &&
                           new_prg->features.opt_alpha_threshold;
    ps2GsCoreSetAlphaTest(threshold, threshold ? PS2_GFX_ALPHA_THRESHOLD : 0u);

    const bool fog = new_prg && new_prg->plan.supported && new_prg->features.opt_fog;
    ps2GsCoreSetFog(fog, 0u, 0u, 0u);

    const bool texture_alpha = new_prg && new_prg->plan.supported &&
                               new_prg->plan.texture_alpha;
    ps2GsCoreSetTextureAlpha(texture_alpha);
}

static struct ShaderProgram *ps2_create_and_load_new_shader(uint64_t shader_id0, uint32_t shader_id1)
{
    struct ShaderProgram *existing = ps2_lookup_shader(shader_id0, shader_id1);
    if (existing) {
        ps2_load_shader(existing);
        return existing;
    }

    for (int i = 0; i < PS2_GFX_MAX_SHADERS; ++i) {
        if (!s_shaders[i].used) {
            struct ShaderProgram *prg = &s_shaders[i];
            memset(prg, 0, sizeof(*prg));
            prg->used = true;
            prg->shader_id0 = shader_id0;
            prg->shader_id1 = shader_id1;
            gfx_cc_get_features(shader_id0, shader_id1, &prg->features);
            ps2GfxPlanCombiner(&prg->features, &prg->plan);

            ps2_log_shader_recipe(i, prg);

            ps2_load_shader(prg);
            return prg;
        }
    }

    sysLogPrintf(LOG_ERROR, "GfxPS2 shader pool exhausted (%d)", PS2_GFX_MAX_SHADERS);
    return NULL;
}

static void ps2_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2])
{
    if (!prg) {
        *num_inputs = 0;
        used_textures[0] = false;
        used_textures[1] = false;
        return;
    }

    *num_inputs = (uint8_t)prg->features.num_inputs;
    used_textures[0] = prg->features.used_textures[0];
    used_textures[1] = prg->features.used_textures[1];
}

static void ps2_clear_shaders(void)
{
    memset(s_shaders, 0, sizeof(s_shaders));
    s_shader = NULL;
}

static uint32_t ps2_new_texture(void)
{
    const uint32_t texture_id = (uint32_t)ps2GsCoreCreateTexture();
    if (texture_id < PS2_GFX_TEXTURE_STATE_SLOTS) {
        s_texture_sampler[texture_id] = {};
    }
    return texture_id;
}

static bool ps2_is_power_of_two(uint32_t value)
{
    return value != 0u && (value & (value - 1u)) == 0u;
}

static void ps2_effective_upload_mirror(
    uint32_t width, uint32_t height, bool *mirror_s, bool *mirror_t)
{
    *mirror_s = s_upload_mirror_s && width <= 512u &&
        ps2_is_power_of_two(width);
    *mirror_t = s_upload_mirror_t && height <= 512u &&
        ps2_is_power_of_two(height);
    if ((*mirror_s != s_upload_mirror_s ||
         *mirror_t != s_upload_mirror_t) &&
        !s_warned_mirror_extent) {
        sysLogPrintf(LOG_WARNING,
            "GfxPS2 mirror period is not GS-expandable (%ux%u); retaining compatibility sampling",
            (unsigned int)width, (unsigned int)height);
        s_warned_mirror_extent = true;
    }
}

static void ps2_record_texture_mirror(
    Ps2GsTextureHandle handle, uint32_t width, uint32_t height,
    bool mirror_s, bool mirror_t)
{
    if (handle < PS2_GFX_TEXTURE_STATE_SLOTS) {
        s_texture_sampler[handle].logical_width = width;
        s_texture_sampler[handle].logical_height = height;
        s_texture_sampler[handle].expanded_mirror_s = mirror_s;
        s_texture_sampler[handle].expanded_mirror_t = mirror_t;
        s_texture_sampler[handle].coordinate_scale_s =
            gfxPs2TextureCoordinateScale(width, mirror_s) *
            (mirror_s ? 0.5f : 1.0f);
        s_texture_sampler[handle].coordinate_scale_t =
            gfxPs2TextureCoordinateScale(height, mirror_t) *
            (mirror_t ? 0.5f : 1.0f);
    }
    if ((mirror_s || mirror_t) && !s_logged_native_mirror) {
        sysLogPrintf(LOG_NOTE,
            "GfxPS2 native mirror-wrap: reflected 2x GS residency, axes=%s%s",
            mirror_s ? "S" : "", mirror_t ? "T" : "");
        s_logged_native_mirror = true;
    }
}

static void ps2_record_texture_provenance(
    Ps2GsTextureHandle handle, uint8_t format, uint8_t size,
    uint32_t palette_format, uint32_t palette_count,
    uint64_t source_hash, uint64_t palette_hash,
    uint64_t content_identity)
{
    if (handle >= PS2_GFX_TEXTURE_STATE_SLOTS) {
        return;
    }
    struct Ps2TextureSamplerState *sampler = &s_texture_sampler[handle];
    sampler->source_format = format;
    sampler->source_size = size;
    sampler->palette_format = palette_format;
    sampler->palette_count = palette_count;
    sampler->upload_serial = ++s_texture_upload_serial;
    sampler->source_hash = source_hash;
    sampler->palette_hash = palette_hash;
    sampler->content_identity = content_identity;
}

static void ps2_apply_texture_clamp(int tile)
{
    if (tile < 0 || tile > 1) {
        return;
    }
    const struct Ps2TextureRegionClampState *region =
        &s_draw_region_clamp[tile];
    ps2GsCoreSetTextureRegionClamp(
        s_sampler_cms[tile], s_sampler_cmt[tile],
        region->region_s, region->max_u,
        region->region_t, region->max_v);
}

static bool ps2_decode_texture_region_clamp(
    Ps2GsTextureHandle handle, bool s_axis,
    float normalized_bound, uint16_t *maximum)
{
    const uint32_t extent = handle < PS2_GFX_TEXTURE_STATE_SLOTS
        ? (s_axis ? s_texture_sampler[handle].logical_width
                  : s_texture_sampler[handle].logical_height)
        : 0u;
    if (gfxPs2TextureRegionClampMax(
            normalized_bound, extent, maximum)) {
        return true;
    }

    if (!s_warned_region_clamp_extent) {
        sysLogPrintf(LOG_WARNING,
            "GfxPS2 REGION_CLAMP bound is outside the GS 0..1023 texel contract");
        s_warned_region_clamp_extent = true;
    }
    return false;
}

static void ps2_select_texture(int tile, uint32_t texture_id, bool linear_filter)
{
    if (tile < 0 || tile > 1 || texture_id > UINT16_MAX) {
        return;
    }

    Ps2GsTextureHandle handle = (Ps2GsTextureHandle)texture_id;
    if (!ps2GsCoreTextureExists(handle)) {
        return;
    }

    s_selected_texture[tile] = handle;
    s_active_texture_tile = tile;
    s_sampler_linear[tile] = linear_filter;
    if (texture_id < PS2_GFX_TEXTURE_STATE_SLOTS) {
        s_sampler_cms[tile] = s_texture_sampler[texture_id].cms;
        s_sampler_cmt[tile] = s_texture_sampler[texture_id].cmt;
        ps2GsCoreSetTextureClamp(
            s_sampler_cms[tile], s_sampler_cmt[tile]);
    }
    ps2GsCoreSetTextureFilter(handle, linear_filter);
    ps2RendererTraceRecord(PS2_TRACE_TEXTURE_SELECT,
        linear_filter ? 1u : 0u, (uint32_t)tile, texture_id,
        s_sampler_cms[tile], s_sampler_cmt[tile]);
}

static uint64_t ps2_trace_hash(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0u; i < size; ++i) {
        hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
    }
    return hash;
}

static void ps2_trace_build_info(void)
{
    if (!ps2RendererTraceIsCapturing()) {
        return;
    }

    static const char commit[] = PD_PS2_GIT_COMMIT;
    const uint32_t bytes = (uint32_t)sizeof(commit);
    uint32_t offset = 0u;
    const bool stored = ps2RendererTraceAppendBlob(
        commit, bytes, 1u, &offset);
    ps2RendererTraceRecord(PS2_TRACE_BUILD_INFO,
        stored ? (uint16_t)0u : (uint16_t)PS2_TRACE_FLAG_DROPPED,
        (uint64_t)offset | ((uint64_t)bytes << 32u),
        ps2RendererTraceHash(commit, bytes),
        0u, 0u);
}

static void ps2_trace_tmem_component(
    uint16_t subtype, const void *data, uint32_t bytes, uint64_t metadata)
{
    if (!ps2RendererTraceIsCapturing() || !data || bytes == 0u) {
        return;
    }
    uint32_t offset = 0u;
    const bool stored = ps2RendererTraceAppendBlob(
        data, bytes, 16u, &offset);
    ps2RendererTraceRecord(PS2_TRACE_TMEM_SNAPSHOT,
        (uint16_t)(subtype |
            (stored ? (uint16_t)0u : (uint16_t)PS2_TRACE_FLAG_DROPPED)),
        (uint64_t)offset | ((uint64_t)bytes << 32u),
        ps2RendererTraceHash(data, bytes),
        metadata, 0u);
}

static void ps2_trace_tmem_snapshot(void)
{
    if (!ps2RendererTraceIsCapturing()) {
        return;
    }

    const struct GfxRdpTmem *tmem = gfxRdpTmemLiveState();
    if (!tmem) {
        return;
    }
    struct GfxRdpTmemLiveStats stats;
    memset(&stats, 0, sizeof(stats));
    gfxRdpTmemLiveGetStats(&stats);

    ps2_trace_tmem_component(
        0u, tmem->bytes, sizeof(tmem->bytes), tmem->generation);
    ps2_trace_tmem_component(
        1u, tmem->byte_valid, sizeof(tmem->byte_valid), tmem->generation);
    ps2_trace_tmem_component(
        2u, tmem->word_generation, sizeof(tmem->word_generation),
        tmem->generation);
    ps2_trace_tmem_component(
        3u, tmem->word_valid, sizeof(tmem->word_valid), tmem->generation);
    ps2_trace_tmem_component(
        4u, &stats, sizeof(stats), tmem->generation);
}

static void ps2_trace_texture_blob(
    Ps2GsTextureHandle handle, uint16_t subtype,
    const void *data, size_t size, uint64_t metadata, uint64_t hash)
{
    if (!ps2RendererTraceIsCapturing() || !data || size == 0u ||
        size > UINT32_MAX) {
        return;
    }

    uint32_t offset = 0u;
    const uint32_t bytes = (uint32_t)size;
    const bool stored = ps2RendererTraceAppendBlob(
        data, bytes, 16u, &offset);
    ps2RendererTraceRecord(PS2_TRACE_TEXTURE_DETAIL,
        subtype | (stored ? 0u : (uint16_t)PS2_TRACE_FLAG_DROPPED),
        handle,
        (uint64_t)offset | ((uint64_t)bytes << 32u),
        metadata, hash);
}

static void ps2_upload_texture(const uint8_t *rgba32_buf, uint32_t width, uint32_t height, bool gen_mipmaps)
{
    if (s_active_texture_tile < 0 || s_active_texture_tile > 1) {
        return;
    }

    if (gen_mipmaps && !s_warned_mipmap) {
        sysLogPrintf(LOG_WARNING, "GfxPS2 mipmap generation is not implemented in the bring-up backend");
        s_warned_mipmap = true;
    }

    /*
     * Keep Fast3D's RGBA8 texture representation untouched. Texture alpha is
     * normalized at the GS texture-function boundary by fragment-alpha scale,
     * avoiding a full texture copy/repack on the upload critical path.
     */
    const Ps2GsTextureHandle handle =
        s_selected_texture[s_active_texture_tile];
    const size_t upload_bytes = (size_t)width * height * 4u;
    const uint64_t upload_hash =
        ps2RendererTraceIsCapturing() && rgba32_buf
        ? ps2_trace_hash(rgba32_buf, upload_bytes) : 0u;
    if (ps2RendererTraceIsCapturing()) {
        ps2RendererTraceRecord(PS2_TRACE_TEXTURE_UPLOAD,
            gen_mipmaps ? 1u : 0u, handle,
            ((uint64_t)width << 32u) | height, upload_bytes,
            upload_hash);
        ps2_trace_texture_blob(
            handle, 0x0200u, rgba32_buf, upload_bytes,
            (uint64_t)width | ((uint64_t)height << 32u),
            upload_hash);
    }
    bool mirror_s;
    bool mirror_t;
    ps2_effective_upload_mirror(width, height, &mirror_s, &mirror_t);
    if (ps2GsCoreUploadTextureRgba32(
            handle, rgba32_buf, width, height,
            mirror_s, mirror_t) &&
        handle < PS2_GFX_TEXTURE_STATE_SLOTS) {
        ps2_record_texture_mirror(
            handle, width, height, mirror_s, mirror_t);
        s_texture_sampler[handle].monochrome_rgb =
            gfxPs2Rgba32IsMonochrome(
                rgba32_buf, width * height);
        ps2_record_texture_provenance(
            handle, PS2_GFX_N64_FMT_RGBA, PS2_GFX_N64_SIZ_32B,
            0u, 0u, upload_hash, 0u, 0u);
    }
}

extern "C" void gfxPs2SetTextureUploadMirror(uint8_t cms, uint8_t cmt)
{
    s_upload_mirror_s = (cms & 1u) != 0u;
    s_upload_mirror_t = (cmt & 1u) != 0u;
}

extern "C" void gfxPs2TraceTmemTextureView(uint32_t texture_id,
    const struct GfxRdpTmemLiveTextureView *view,
    uint8_t format, uint8_t size, uint32_t palette_format)
{
    if (!ps2RendererTraceIsCapturing() || !view || !view->texels ||
        view->size_bytes == 0u ||
        texture_id == 0u || texture_id >= PS2_GFX_TEXTURE_STATE_SLOTS) {
        return;
    }

    const Ps2GsTextureHandle handle = (Ps2GsTextureHandle)texture_id;
    if (view->content_identity != 0u &&
        s_trace_tmem_snapshot_identity[handle] == view->content_identity) {
        return;
    }

    const uint64_t source_hash =
        ps2_trace_hash(view->texels, view->size_bytes);
    const uint64_t palette_hash =
        view->palette && view->palette_count != 0u
        ? ps2_trace_hash(
            view->palette,
            (size_t)view->palette_count * sizeof(uint16_t))
        : 0u;

    /*
     * A cache hit means no upload occurs during this capture, so refresh only
     * forensic provenance. Keep upload_serial unchanged: no GS residency was
     * created or replaced here.
     */
    struct Ps2TextureSamplerState *sampler = &s_texture_sampler[handle];
    sampler->source_format = format;
    sampler->source_size = size;
    sampler->palette_format = palette_format;
    sampler->palette_count = view->palette_count;
    sampler->source_hash = source_hash;
    sampler->palette_hash = palette_hash;
    sampler->content_identity = view->content_identity;

    ps2RendererTraceRecord(PS2_TRACE_TEXTURE_DETAIL, 0x1200u,
        handle, view->content_identity, source_hash, palette_hash);
    ps2_trace_texture_blob(
        handle, 0x0200u, view->texels, view->size_bytes,
        (uint64_t)view->line_size_bytes |
            ((uint64_t)(view->size_bytes / view->line_size_bytes) << 32u),
        source_hash);
    if (view->palette && view->palette_count != 0u) {
        ps2_trace_texture_blob(
            handle, 0x0300u, view->palette,
            (size_t)view->palette_count * sizeof(uint16_t),
            (uint64_t)view->palette_count |
                ((uint64_t)palette_format << 32u),
            palette_hash);
    }

    s_trace_tmem_snapshot_identity[handle] = view->content_identity;
}

extern "C" void gfxPs2TraceGfxCommands(
    const void *commands, uint32_t depth, uint32_t entry_count)
{
    if (!ps2RendererTraceIsCapturing() || !commands ||
        entry_count == 0u || entry_count > 4u) {
        return;
    }

    const uint32_t *words = (const uint32_t *)commands;
    for (uint32_t entry = 0u; entry < entry_count; ++entry) {
        const uint32_t w0 = words[entry * 2u];
        const uint32_t w1 = words[entry * 2u + 1u];
        const uint16_t flags = entry == 0u ? 0u : 0x0100u;
        ps2RendererTraceRecord(PS2_TRACE_GFX_COMMAND, flags,
            (uint64_t)(uint32_t)(uintptr_t)
                ((const uint8_t *)commands + entry * 8u),
            (uint64_t)depth | ((uint64_t)entry << 32u),
            w0, w1);
    }
}

extern "C" void gfxPs2TraceGfxSource(uint16_t kind, const void *data,
    uint32_t size, uint64_t metadata)
{
    if (!ps2RendererTraceIsCapturing() || !data || size == 0u) {
        return;
    }

    uint32_t offset = 0u;
    const bool stored = ps2RendererTraceAppendBlob(
        data, size, 16u, &offset);
    const uint64_t hash = ps2RendererTraceHash(data, size);
    ps2RendererTraceRecord(PS2_TRACE_GFX_SOURCE,
        (uint16_t)(kind |
            (stored ? (uint16_t)0u : (uint16_t)PS2_TRACE_FLAG_DROPPED)),
        (uint64_t)offset | ((uint64_t)size << 32u),
        hash, metadata,
        (uint64_t)(uint32_t)(uintptr_t)data);
}

extern "C" bool gfxPs2UploadTmemTexture(
    const struct GfxRdpTmemLiveTextureView *view,
    uint8_t format, uint8_t size, uint32_t palette_format,
    bool gen_mipmaps)
{
    if (!view || !view->texels ||
        view->line_size_bytes == 0u ||
        view->size_bytes == 0u ||
        view->size_bytes % view->line_size_bytes != 0u ||
        s_active_texture_tile < 0 || s_active_texture_tile > 1) {
        return false;
    }

    if (gen_mipmaps && !s_warned_mipmap) {
        sysLogPrintf(LOG_WARNING,
            "GfxPS2 mipmap generation is not implemented in the bring-up backend");
        s_warned_mipmap = true;
    }

    const uint32_t height = view->size_bytes / view->line_size_bytes;
    const Ps2GsTextureHandle handle =
        s_selected_texture[s_active_texture_tile];
    const bool trace_capture = ps2RendererTraceIsCapturing();
    const uint64_t source_hash = trace_capture
        ? ps2_trace_hash(view->texels, view->size_bytes) : 0u;
    const uint64_t palette_hash =
        trace_capture && view->palette && view->palette_count != 0u
        ? ps2_trace_hash(
            view->palette, (size_t)view->palette_count * sizeof(uint16_t))
        : 0u;
    if (trace_capture) {
        const uint16_t trace_flags =
            (uint16_t)((format & 0xfu) | ((size & 0xfu) << 4u));
        ps2RendererTraceRecord(PS2_TRACE_TEXTURE_UPLOAD, trace_flags,
            handle,
            ((uint64_t)view->line_size_bytes << 32u) | height,
            ((uint64_t)view->size_bytes << 32u) | view->palette_count,
            source_hash);
        if (view->palette && view->palette_count != 0u) {
            ps2RendererTraceRecord(PS2_TRACE_TEXTURE_DETAIL, 0x0100u,
                handle, view->palette_count, palette_format, palette_hash);
        }
        ps2_trace_texture_blob(
            handle, 0x0200u, view->texels, view->size_bytes,
            (uint64_t)view->line_size_bytes |
                ((uint64_t)height << 32u),
            source_hash);
        if (view->palette && view->palette_count != 0u) {
            const size_t palette_bytes =
                (size_t)view->palette_count * sizeof(uint16_t);
            ps2_trace_texture_blob(
                handle, 0x0300u, view->palette, palette_bytes,
                (uint64_t)view->palette_count |
                    ((uint64_t)palette_format << 32u),
                palette_hash);
        }
    }

    if (format == PS2_GFX_N64_FMT_RGBA &&
        size == PS2_GFX_N64_SIZ_32B &&
        (view->line_size_bytes & 3u) == 0u) {
        const uint32_t width = view->line_size_bytes / 4u;
        bool mirror_s;
        bool mirror_t;
        ps2_effective_upload_mirror(width, height, &mirror_s, &mirror_t);
        if (!ps2GsCoreUploadTextureRgba32(
                handle, view->texels, width, height,
                mirror_s, mirror_t)) {
            return false;
        }
        ps2_record_texture_mirror(
            handle, width, height, mirror_s, mirror_t);
        if (handle < PS2_GFX_TEXTURE_STATE_SLOTS) {
            s_texture_sampler[handle].monochrome_rgb =
                gfxPs2Rgba32IsMonochrome(
                    view->texels, width * height);
        }
        ps2_record_texture_provenance(
            handle, format, size, palette_format, view->palette_count,
            source_hash, palette_hash, view->content_identity);
        if (!s_logged_native_rgba32) {
            sysLogPrintf(LOG_NOTE,
                "GfxPS2 native texture path: exact split-TMEM N64 RGBA32 -> GS PSMCT32");
            s_logged_native_rgba32 = true;
        }
        return true;
    }

    if (format == PS2_GFX_N64_FMT_RGBA &&
        size == PS2_GFX_N64_SIZ_16B &&
        (view->line_size_bytes & 1u) == 0u) {
        const uint32_t width = view->line_size_bytes / 2u;
        bool mirror_s;
        bool mirror_t;
        ps2_effective_upload_mirror(width, height, &mirror_s, &mirror_t);
        if (!ps2GsCoreUploadTextureN64Rgba16(
                handle, view->texels, width, height,
                mirror_s, mirror_t)) {
            return false;
        }
        ps2_record_texture_mirror(
            handle, width, height, mirror_s, mirror_t);
        if (handle < PS2_GFX_TEXTURE_STATE_SLOTS) {
            s_texture_sampler[handle].monochrome_rgb =
                gfxPs2N64Rgba16IsMonochrome(
                    view->texels, width * height);
        }
        ps2_record_texture_provenance(
            handle, format, size, palette_format, view->palette_count,
            source_hash, palette_hash, view->content_identity);
        if (!s_logged_native_rgba16) {
            sysLogPrintf(LOG_NOTE,
                "GfxPS2 native texture path: exact N64 RGBA16 -> GS PSMCT16");
            s_logged_native_rgba16 = true;
        }
        return true;
    }

    if (format == PS2_GFX_N64_FMT_IA &&
        size == PS2_GFX_N64_SIZ_16B &&
        (view->line_size_bytes & 1u) == 0u) {
        const uint32_t width = view->line_size_bytes / 2u;
        bool mirror_s;
        bool mirror_t;
        ps2_effective_upload_mirror(width, height, &mirror_s, &mirror_t);
        if (!ps2GsCoreUploadTextureN64Ia16(
                handle, view->texels, width, height,
                mirror_s, mirror_t)) {
            return false;
        }
        ps2_record_texture_mirror(
            handle, width, height, mirror_s, mirror_t);
        if (handle < PS2_GFX_TEXTURE_STATE_SLOTS) {
            s_texture_sampler[handle].monochrome_rgb = true;
        }
        ps2_record_texture_provenance(
            handle, format, size, palette_format, view->palette_count,
            source_hash, palette_hash, view->content_identity);
        if (!s_logged_native_ia16) {
            sysLogPrintf(LOG_NOTE,
                "GfxPS2 native texture path: exact N64 IA16 -> GS PSMCT32");
            s_logged_native_ia16 = true;
        }
        return true;
    }

#if !defined(PERFECT_DARK_PS2_NATIVE_INDEXED_TEXTURES)
    /*
     * Retail-hardware correctness control. The portable Fast3D importer
     * expands these formats to RGBA32 after this function returns false.
     * Keeping this gate at the native residency boundary preserves the same
     * authoritative TMEM bytes, dimensions and cache identity while removing
     * PSMT4/PSMT8 IMAGE/TBW/CLUT layout from the experiment.
     */
    if (((format == PS2_GFX_N64_FMT_IA ||
          format == PS2_GFX_N64_FMT_I) &&
         (size == PS2_GFX_N64_SIZ_4B ||
          size == PS2_GFX_N64_SIZ_8B)) ||
        (format == PS2_GFX_N64_FMT_CI &&
         (size == PS2_GFX_N64_SIZ_4B ||
          size == PS2_GFX_N64_SIZ_8B))) {
        return false;
    }
#endif

    if ((format == PS2_GFX_N64_FMT_IA ||
         format == PS2_GFX_N64_FMT_I) &&
        (size == PS2_GFX_N64_SIZ_4B ||
         size == PS2_GFX_N64_SIZ_8B)) {
        const bool four_bit = size == PS2_GFX_N64_SIZ_4B;
        const enum Ps2GsN64IntensityEncoding encoding =
            format == PS2_GFX_N64_FMT_IA
                ? (four_bit ? PS2_GS_N64_IA4 : PS2_GS_N64_IA8)
                : (four_bit ? PS2_GS_N64_I4 : PS2_GS_N64_I8);
        const uint32_t width = four_bit
            ? view->line_size_bytes * 2u : view->line_size_bytes;
        bool mirror_s;
        bool mirror_t;
        ps2_effective_upload_mirror(width, height, &mirror_s, &mirror_t);
        if (!ps2GsCoreUploadTextureN64Intensity(
                handle, view->texels, width, height, encoding,
                mirror_s, mirror_t)) {
            return false;
        }
        ps2_record_texture_mirror(
            handle, width, height, mirror_s, mirror_t);
        if (handle < PS2_GFX_TEXTURE_STATE_SLOTS) {
            s_texture_sampler[handle].monochrome_rgb = true;
        }
        ps2_record_texture_provenance(
            handle, format, size, palette_format, view->palette_count,
            source_hash, palette_hash, view->content_identity);
        if (!s_logged_native_intensity[(uint32_t)encoding]) {
            sysLogPrintf(LOG_NOTE,
                "GfxPS2 native texture path: exact N64 %s%u -> GS PSMT%u/shared CT32 CSM1",
                format == PS2_GFX_N64_FMT_IA ? "IA" : "I",
                four_bit ? 4u : 8u, four_bit ? 4u : 8u);
            s_logged_native_intensity[(uint32_t)encoding] = true;
        }
        return true;
    }

    const bool ia16_palette =
        palette_format == PS2_GFX_N64_TT_IA16;
    if (format != PS2_GFX_N64_FMT_CI ||
        (palette_format != PS2_GFX_N64_TT_RGBA16 && !ia16_palette) ||
        !view->palette) {
        return false;
    }

    const bool ci4 = size == PS2_GFX_N64_SIZ_4B &&
        view->palette_count == 16u;
    const bool ci8 = size == PS2_GFX_N64_SIZ_8B &&
        view->palette_count == 256u;
    if (!ci4 && !ci8) {
        return false;
    }

    const uint32_t width = ci4
        ? view->line_size_bytes * 2u : view->line_size_bytes;
    bool mirror_s;
    bool mirror_t;
    ps2_effective_upload_mirror(width, height, &mirror_s, &mirror_t);
    if (!ps2GsCoreUploadTextureN64Ci(handle, view->texels,
            width, height, ci4 ? 4u : 8u,
            view->palette, view->palette_count,
            ia16_palette ? PS2_GS_N64_PALETTE_IA16
                         : PS2_GS_N64_PALETTE_RGBA16,
            mirror_s, mirror_t)) {
        return false;
    }
    ps2_record_texture_mirror(
        handle, width, height, mirror_s, mirror_t);
    if (handle < PS2_GFX_TEXTURE_STATE_SLOTS) {
        bool monochrome = ia16_palette;
        if (!monochrome) {
            monochrome = true;
            for (uint32_t i = 0u; i < view->palette_count; ++i) {
                const uint16_t texel = view->palette[i];
                const uint16_t r = (texel >> 11u) & 0x1fu;
                const uint16_t g = (texel >> 6u) & 0x1fu;
                const uint16_t b = (texel >> 1u) & 0x1fu;
                if (r != g || r != b) {
                    monochrome = false;
                    break;
                }
            }
        }
        s_texture_sampler[handle].monochrome_rgb = monochrome;
    }

    bool *logged = ia16_palette
        ? &s_logged_native_ci_ia16[ci4 ? 0u : 1u]
        : (ci4 ? &s_logged_native_ci4 : &s_logged_native_ci8);
    ps2_record_texture_provenance(
        handle, format, size, palette_format, view->palette_count,
        source_hash, palette_hash, view->content_identity);
    if (!*logged) {
        sysLogPrintf(LOG_NOTE,
            "GfxPS2 native texture path: exact N64 CI%u/%s TLUT -> GS PSMT%u/%s CSM1",
            ci4 ? 4u : 8u, ia16_palette ? "IA16" : "RGBA16",
            ci4 ? 4u : 8u, ia16_palette ? "CT32" : "CT16");
        *logged = true;
    }
    return true;
}

static void ps2_set_sampler_parameters(int sampler, bool linear_filter, uint32_t cms, uint32_t cmt, bool mipmaps)
{
    if (sampler < 0 || sampler > 1) {
        return;
    }

    s_sampler_cms[sampler] = cms;
    s_sampler_cmt[sampler] = cmt;
    s_sampler_linear[sampler] = linear_filter;
    const uint32_t texture_id = (uint32_t)s_selected_texture[sampler];
    if (texture_id < PS2_GFX_TEXTURE_STATE_SLOTS) {
        s_texture_sampler[texture_id].cms = cms;
        s_texture_sampler[texture_id].cmt = cmt;
    }
    ps2GsCoreSetTextureFilter(s_selected_texture[sampler], linear_filter);
    ps2GsCoreSetTextureClamp(cms, cmt);
    ps2RendererTraceRecord(PS2_TRACE_SAMPLER,
        (linear_filter ? 1u : 0u) | (mipmaps ? 2u : 0u),
        sampler, s_selected_texture[sampler], cms, cmt);

    if (mipmaps && !s_warned_mipmap) {
        sysLogPrintf(LOG_WARNING, "GfxPS2 mipmap sampling requested but not implemented");
        s_warned_mipmap = true;
    }
}

static void ps2_set_depth_mode(bool depth_test, bool depth_update, bool depth_compare,
                               bool depth_source_prim, uint16_t zmode)
{
#if defined(PERFECT_DARK_PS2_DISABLE_DEPTH)
    (void)depth_test;
    (void)depth_update;
    (void)depth_compare;
    (void)depth_source_prim;
    (void)zmode;
    s_depth_test = false;
    s_depth_update = false;
    s_depth_compare = false;
    s_depth_compare_equal = false;
    s_depth_decal = false;
    ps2GsCoreSetDepthMode(false, false, false, false);
#else
    const bool compare_equal =
        gfxPs2DepthModeAllowsEqual(depth_source_prim, zmode);
    const bool decal = gfxPs2DepthModeUsesDecalBias(
        depth_test, depth_compare, zmode);
    s_depth_test = depth_test;
    s_depth_update = depth_update;
    s_depth_compare = depth_compare;
    s_depth_compare_equal = compare_equal;
    s_depth_decal = decal;
    ps2GsCoreSetDepthMode(
        depth_test, depth_update, depth_compare, compare_equal);
#endif
    ps2RendererTraceRecord(PS2_TRACE_DEPTH,
        (depth_test ? 1u : 0u) | (depth_update ? 2u : 0u) |
            (depth_compare ? 4u : 0u) | (depth_source_prim ? 8u : 0u),
        zmode, s_depth_compare_equal ? 1u : 0u,
        s_depth_decal ? 1u : 0u, 0u);
}

static void ps2_set_depth_range(float znear, float zfar)
{
    s_depth_near = znear;
    s_depth_far = zfar;
}

static void ps2_set_viewport(int x, int y, int width, int height)
{
    s_viewport.x = x;
    s_viewport.y = gfxPs2TopLeftY(
        ps2GsCoreGetHeight(), y, height);
    s_viewport.width = width;
    s_viewport.height = height;
    ps2RendererTraceRecord(PS2_TRACE_VIEWPORT, 0u,
        (uint32_t)x, (uint32_t)s_viewport.y,
        (uint32_t)width, (uint32_t)height);
}

static void ps2_set_scissor(int x, int y, int width, int height)
{
    const int top_y = gfxPs2TopLeftY(
        ps2GsCoreGetHeight(), y, height);
    s_scissor.x = x;
    s_scissor.y = top_y;
    s_scissor.width = width;
    s_scissor.height = height;
    ps2GsCoreSetScissor(x, top_y, width, height);
    ps2RendererTraceRecord(PS2_TRACE_SCISSOR, 0u,
        (uint32_t)x, (uint32_t)top_y,
        (uint32_t)width, (uint32_t)height);
}

static void ps2_set_use_alpha(bool use_alpha, bool modulate)
{
    s_alpha_blend = use_alpha;
    s_modulate = modulate;
    ps2GsCoreSetAlphaBlend(use_alpha);
    ps2RendererTraceRecord(PS2_TRACE_ALPHA,
        (use_alpha ? 1u : 0u) | (modulate ? 2u : 0u),
        use_alpha ? 1u : 0u, modulate ? 1u : 0u, 0u, 0u);
}

static float ps2_clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static uint8_t ps2_u8_component(float v)
{
    int out = (int)(ps2_clampf(v, 0.0f, 1.0f) * 255.0f + 0.5f);
    if (out < 0) out = 0;
    if (out > 255) out = 255;
    return (uint8_t)out;
}

static uint8_t ps2_modulate_component(float v)
{
    /* GS texture MODULATE uses 0x80 as a 1.0 fragment multiplier. */
    int out = (int)(ps2_clampf(v, 0.0f, 1.0f) * 128.0f + 0.5f);
    if (out < 0) out = 0;
    if (out > 128) out = 128;
    return (uint8_t)out;
}

static uint8_t ps2_texture_alpha_fragment_component(float v)
{
    /*
     * Fast3D texture alpha arrives in 0..255. GS MODULATE computes At*Af>>7,
     * so Af=0x40 represents the compensation needed to map texel 0xff to the
     * GS-native opaque alpha neighbourhood without an upload-side repack.
     */
    int out = (int)(ps2_clampf(v, 0.0f, 1.0f) * 64.0f + 0.5f);
    if (out < 0) out = 0;
    if (out > 64) out = 64;
    return (uint8_t)out;
}

#if defined(PERFECT_DARK_PS2_MATERIAL_BASELINE)
static bool ps2_material_baseline_texture_alpha(
    const struct ShaderProgram *shader)
{
    return shader && gfxPs2DirectMaterialUsesTextureAlpha(
        shader->plan.supported,
        shader->plan.texture_alpha,
        shader->features.opt_alpha,
        shader->features.used_textures[0]);
}

static uint8_t ps2_material_baseline_fragment_alpha(
    const struct ShaderProgram *shader, const float input[3][4])
{
    if (!shader || !shader->features.opt_alpha) {
        return 0x80u;
    }

    switch (shader->plan.alpha_recipe) {
        case PS2_ALPHA_ZERO:
            return 0x00u;
        case PS2_ALPHA_OPAQUE:
        case PS2_ALPHA_ONE:
            return 0x80u;
        case PS2_ALPHA_INPUT1:
            return ps2_modulate_component(input[0][3]);
        case PS2_ALPHA_TEX0:
            return 0x40u;
        case PS2_ALPHA_TEX0_MUL_INPUT1:
            return ps2_texture_alpha_fragment_component(input[0][3]);
        case PS2_ALPHA_TEX1_MUL_INPUT1:
            return ps2_modulate_component(input[0][3]);
        case PS2_ALPHA_INPUT1_MUL_INPUT2:
            return ps2_modulate_component(input[0][3] * input[1][3]);
        case PS2_ALPHA_TEX0_MUL_INPUT1_MUL_INPUT2:
        case PS2_ALPHA_TEX0_MUL_TEX1_MUL_INPUT1:
            return ps2_texture_alpha_fragment_component(
                input[0][3] * input[1][3]);
        case PS2_ALPHA_INPUT1_PLUS_INPUT2_EDGE:
            return ps2_modulate_component(
                gfxPs2CoverageUnion(input[0][3], input[1][3]));
        case PS2_ALPHA_INPUT1_INV_INPUT1_MUL_INPUT2:
            return ps2_modulate_component(
                input[0][3] * (1.0f - input[0][3]) * input[1][3]);
        case PS2_ALPHA_TEX0_MUL_INPUT1_MINUS_INPUT2_PLUS_INPUT3_EDGE:
        case PS2_ALPHA_INPUT2_INPUT1_LERP_TEX0:
        case PS2_ALPHA_INPUT2_INPUT1_COVERAGE_LERP_TEX0:
            return 0x40u;
        case PS2_ALPHA_UNSUPPORTED:
        default:
            return ps2_material_baseline_texture_alpha(shader)
                ? ps2_texture_alpha_fragment_component(input[0][3])
                : ps2_modulate_component(input[0][3]);
    }
}
#endif

static uint8_t ps2_fog_coefficient(float fast3d_factor)
{
    const float source_weight = 1.0f - ps2_clampf(fast3d_factor, 0.0f, 1.0f);
    return ps2_u8_component(source_weight);
}

static uint32_t ps2_float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static struct Ps2GsPackedReg ps2_pack_rgbaq(
    uint8_t r, uint8_t g, uint8_t b, uint8_t a, float q)
{
    struct Ps2GsPackedReg out;
    out.value = (uint64_t)r |
                ((uint64_t)g << 8) |
                ((uint64_t)b << 16) |
                ((uint64_t)a << 24) |
                ((uint64_t)ps2_float_bits(q) << 32);
    out.reg = PS2_GS_REG_RGBAQ;
    return out;
}

static struct Ps2GsPackedReg ps2_pack_st(float s, float t)
{
    struct Ps2GsPackedReg out;
    out.value = (uint64_t)ps2_float_bits(s) |
                ((uint64_t)ps2_float_bits(t) << 32);
    out.reg = PS2_GS_REG_ST;
    return out;
}

static int ps2_fixed_xy(float value, int offset)
{
    int fixed = (int)(value * 16.0f) + offset;
    if (fixed < 0) fixed = 0;
    if (fixed >= 4096 * 16) fixed = 4096 * 16 - 1;
    return fixed;
}

static struct Ps2GsPackedReg ps2_pack_xyz2(float x, float y, int z)
{
    const uint32_t fx = (uint32_t)ps2_fixed_xy(x, ps2GsCoreGetOffsetX());
    const uint32_t fy = (uint32_t)ps2_fixed_xy(y, ps2GsCoreGetOffsetY());

    struct Ps2GsPackedReg out;
    out.value = (uint64_t)(fx & 0xffffu) |
                ((uint64_t)(fy & 0xffffu) << 16) |
                ((uint64_t)(uint32_t)z << 32);
    out.reg = PS2_GS_REG_XYZ2;
    return out;
}

static struct Ps2GsPackedReg ps2_pack_xyzf2(float x, float y, int z, uint8_t fog)
{
    const uint32_t fx = (uint32_t)ps2_fixed_xy(x, ps2GsCoreGetOffsetX());
    const uint32_t fy = (uint32_t)ps2_fixed_xy(y, ps2GsCoreGetOffsetY());

    struct Ps2GsPackedReg out;
    out.value = (uint64_t)(fx & 0xffffu) |
                ((uint64_t)(fy & 0xffffu) << 16) |
                ((uint64_t)((uint32_t)z & 0x00ffffffu) << 32) |
                ((uint64_t)fog << 56);
    out.reg = PS2_GS_REG_XYZF2;
    return out;
}

static int ps2_map_clip_depth(float z, float w)
{
    float ndc = (w != 0.0f) ? (z / w) : 1.0f;
    ndc = ps2_clampf(ndc, 0.0f, 1.0f);

    float ranged = s_depth_near + ndc * (s_depth_far - s_depth_near);
    ranged = ps2_clampf(ranged, 0.0f, 1.0f);

    /* Current GS baseline uses GEQUAL: near is large, far is small. */
    const int depth = 1 + (int)((1.0f - ranged) * 65534.0f);
    return gfxPs2ApplyDecalDepthBias(depth, s_depth_decal);
}

static size_t ps2_vbo_stride(const struct ShaderProgram *prg)
{
    if (!prg) {
        return 0;
    }

    size_t stride = 4;
    for (int t = 0; t < 2; ++t) {
        if (!prg->features.used_textures[t]) {
            continue;
        }
        stride += 2;
        if (prg->features.clamp[t][0]) ++stride;
        if (prg->features.clamp[t][1]) ++stride;
    }
    if (prg->features.opt_fog) stride += 4;
    if (prg->features.opt_grayscale) stride += 4;
    stride += (size_t)prg->features.num_inputs * (prg->features.opt_alpha ? 4u : 3u);
    return stride;
}

static bool ps2_color_recipe_is_opaque_trilerp(enum Ps2ColorRecipe recipe)
{
    return recipe == PS2_COLOR_TEX01_LERP_INPUT1 ||
           recipe == PS2_COLOR_TEX01_LERP_INPUT1_MUL_INPUT2;
}

static bool ps2_color_recipe_is_opaque_input1_tex0_lerp(
    enum Ps2ColorRecipe recipe)
{
    return recipe == PS2_COLOR_INPUT1_TEX0_LERP_INPUT2 ||
           recipe == PS2_COLOR_INPUT1_TEX0_LERP_INPUT2_MUL_INPUT3;
}

static bool ps2_is_trilerp_independent_alpha(
    const struct Ps2CombinerPlan *plan)
{
    return plan->pass_graph ==
            PS2_PASS_GRAPH_TRILERP_INDEPENDENT_ALPHA &&
        plan->color_recipe ==
            PS2_COLOR_TEX01_LERP_INPUT1_MUL_INPUT2 &&
        (plan->alpha_recipe == PS2_ALPHA_INPUT1 ||
         plan->alpha_recipe == PS2_ALPHA_INPUT1_MUL_INPUT2 ||
         plan->alpha_recipe ==
            PS2_ALPHA_TEX0_MUL_INPUT1_MUL_INPUT2 ||
         plan->alpha_recipe == PS2_ALPHA_TEX1_MUL_INPUT1 ||
         plan->alpha_recipe ==
            PS2_ALPHA_INPUT1_PLUS_INPUT2_EDGE ||
         plan->alpha_recipe ==
            PS2_ALPHA_INPUT1_INV_INPUT1_MUL_INPUT2 ||
         plan->alpha_recipe ==
            PS2_ALPHA_TEX0_MUL_INPUT1_MINUS_INPUT2_PLUS_INPUT3_EDGE);
}

static bool ps2_is_independent_tex0_alpha(
    const struct Ps2CombinerPlan *plan)
{
    return plan &&
        plan->pass_graph == PS2_PASS_GRAPH_INDEPENDENT_TEX0_ALPHA &&
        plan->color_recipe == PS2_COLOR_INPUT1 &&
        (plan->alpha_recipe == PS2_ALPHA_TEX0 ||
         plan->alpha_recipe == PS2_ALPHA_TEX0_MUL_INPUT1);
}

static bool ps2_independent_alpha_is_custom24(
    const struct Ps2CombinerPlan *plan)
{
    return plan->alpha_recipe ==
        PS2_ALPHA_INPUT1_INV_INPUT1_MUL_INPUT2;
}

static bool ps2_independent_alpha_is_custom22_23(
    const struct Ps2CombinerPlan *plan)
{
    return plan->alpha_recipe ==
        PS2_ALPHA_TEX0_MUL_INPUT1_MINUS_INPUT2_PLUS_INPUT3_EDGE;
}

static bool ps2_independent_alpha_uses_tex1(
    const struct Ps2CombinerPlan *plan)
{
    return plan->alpha_recipe == PS2_ALPHA_TEX1_MUL_INPUT1;
}

static void ps2_trilerp_set_base_state(void)
{
    ps2GsCoreSetDepthMode(s_depth_test, s_depth_update, s_depth_compare,
        s_depth_compare_equal);
    ps2GsCoreSetAlphaBlend(false);
    ps2GsCoreSetAlphaWrite(true);
    ps2GsCoreSetTextureAlpha(false);
    ps2_apply_texture_clamp(0);
}

static void ps2_trilerp_set_lerp_state(void)
{
    /* Equal-depth fragments must pass, but the second pass must not rewrite Z. */
    ps2GsCoreSetDepthMode(s_depth_test, false, s_depth_compare, true);
    ps2GsCoreSetAlphaBlend(true);
    ps2GsCoreSetAlphaWrite(false);
    ps2GsCoreSetTextureAlpha(false);
    ps2_apply_texture_clamp(1);
}

static void ps2_trilerp_restore_state(void)
{
    ps2GsCoreSetAlphaWrite(true);
    ps2GsCoreSetAlphaBlend(s_alpha_blend);
    ps2GsCoreSetDepthMode(s_depth_test, s_depth_update, s_depth_compare,
        s_depth_compare_equal);
    ps2_apply_texture_clamp(0);
}

static void ps2_input1_tex0_lerp_set_base_state(void)
{
    ps2GsCoreSetDepthMode(s_depth_test, s_depth_update, s_depth_compare,
        s_depth_compare_equal);
    ps2GsCoreSetAlphaBlend(false);
    ps2GsCoreSetAlphaWrite(true);
    ps2GsCoreSetTextureAlpha(false);
}

static void ps2_input1_tex0_lerp_set_texture_state(void)
{
    ps2GsCoreSetDepthMode(s_depth_test, false, s_depth_compare, true);
    ps2GsCoreSetAlphaBlend(true);
    ps2GsCoreSetAlphaWrite(false);
    ps2GsCoreSetTextureAlpha(false);
    ps2_apply_texture_clamp(0);
}

static void ps2_draw_opaque_trilerp(uint32_t vertex_count)
{
    /*
     * With a writing Z buffer, pass 0 establishes ownership for the complete
     * batch and pass 1 can safely use equal-depth rejection. Without that
     * invariant, keep the two passes adjacent per triangle to preserve N64
     * primitive ordering for overlapping geometry.
     */
    const bool batch_safe = s_depth_test && s_depth_compare && s_depth_update;
    if (batch_safe) {
        ps2_trilerp_set_base_state();
        ps2GsCoreDrawTexturedTriangles(
            s_selected_texture[0], s_stq_vertices[0], vertex_count);
        ps2_trilerp_set_lerp_state();
        ps2GsCoreDrawTexturedTriangles(
            s_selected_texture[1], s_stq_vertices[1], vertex_count);
    } else {
        for (uint32_t vertex = 0; vertex < vertex_count; vertex += 3u) {
            ps2_trilerp_set_base_state();
            ps2GsCoreDrawTexturedTriangles(
                s_selected_texture[0], &s_stq_vertices[0][vertex], 3u);
            ps2_trilerp_set_lerp_state();
            ps2GsCoreDrawTexturedTriangles(
                s_selected_texture[1], &s_stq_vertices[1][vertex], 3u);
        }
    }

    ps2_trilerp_restore_state();
}

static void ps2_draw_opaque_input1_tex0_lerp(uint32_t vertex_count)
{
    const bool batch_safe = s_depth_test && s_depth_compare && s_depth_update;
    if (batch_safe) {
        ps2_input1_tex0_lerp_set_base_state();
        ps2GsCoreDrawColorTriangles(s_color_vertices, vertex_count);
        ps2_input1_tex0_lerp_set_texture_state();
        ps2GsCoreDrawTexturedTriangles(
            s_selected_texture[0], s_stq_vertices[0], vertex_count);
    } else {
        for (uint32_t vertex = 0; vertex < vertex_count; vertex += 3u) {
            ps2_input1_tex0_lerp_set_base_state();
            ps2GsCoreDrawColorTriangles(&s_color_vertices[vertex], 3u);
            ps2_input1_tex0_lerp_set_texture_state();
            ps2GsCoreDrawTexturedTriangles(
                s_selected_texture[0], &s_stq_vertices[0][vertex], 3u);
        }
    }

    ps2_trilerp_restore_state();
}

static bool ps2_ensure_alpha_trilerp_color_workspace(void)
{
    if (s_alpha_trilerp_color_target == PS2_GS_RENDER_TARGET_DEFAULT) {
        s_alpha_trilerp_color_target = ps2GsCoreCreateRenderTarget(
            PS2_GFX_PASS_GRAPH_TILE_WIDTH,
            PS2_GFX_PASS_GRAPH_TILE_HEIGHT);
    }
    return s_alpha_trilerp_color_target != PS2_GS_RENDER_TARGET_DEFAULT;
}

static bool ps2_ensure_alpha_trilerp_workspace(void)
{
    if (ps2_ensure_alpha_trilerp_color_workspace() &&
        s_alpha_trilerp_scalar_target == PS2_GS_RENDER_TARGET_DEFAULT) {
        s_alpha_trilerp_scalar_target = ps2GsCoreCreateRenderTarget(
            PS2_GFX_PASS_GRAPH_TILE_WIDTH,
            PS2_GFX_PASS_GRAPH_TILE_HEIGHT);
    }
    if (s_alpha_trilerp_color_target != PS2_GS_RENDER_TARGET_DEFAULT &&
        s_alpha_trilerp_scalar_target != PS2_GS_RENDER_TARGET_DEFAULT) {
        return true;
    }

    if (!s_warned_alpha_trilerp_workspace) {
        sysLogPrintf(LOG_ERROR,
            "GfxPS2 alpha-trilerp workspace allocation failed (%dx%d x2 CT32)",
            PS2_GFX_PASS_GRAPH_TILE_WIDTH,
            PS2_GFX_PASS_GRAPH_TILE_HEIGHT);
        s_warned_alpha_trilerp_workspace = true;
    }
    return false;
}

static void ps2_make_independent_alpha_texture_triangle(
    const struct Ps2AlphaTrilerpVertex *source, int texture_index,
    int origin_x, int origin_y, bool lerp_color,
    struct Ps2GsTexturedVertex output[3])
{
    for (uint32_t i = 0u; i < 3u; ++i) {
        const struct Ps2AlphaTrilerpVertex *vertex = &source[i];
        output[i].rgbaq = ps2_pack_rgbaq(
            vertex->shade_r, vertex->shade_g, vertex->shade_b,
            lerp_color ? vertex->lod : vertex->independent_alpha,
            vertex->inv_w);
        output[i].st = ps2_pack_st(
            vertex->tex_u[texture_index] * vertex->inv_w,
            vertex->tex_v[texture_index] * vertex->inv_w);
        const float x = vertex->x - (float)origin_x;
        const float y = vertex->y - (float)origin_y;
        output[i].xyz2 = s_shader->features.opt_fog
            ? ps2_pack_xyzf2(x, y, vertex->z, vertex->fog)
            : ps2_pack_xyz2(x, y, vertex->z);
    }
}

static void ps2_make_custom24_scalar_triangle(
    const struct Ps2AlphaTrilerpVertex *source,
    int origin_x, int origin_y, struct Ps2GsColorVertex output[3])
{
    for (uint32_t i = 0u; i < 3u; ++i) {
        const struct Ps2AlphaTrilerpVertex *vertex = &source[i];
        const uint8_t base = vertex->independent_alpha;
        output[i].rgbaq = ps2_pack_rgbaq(
            base, base, base, vertex->shade_a, 0.0f);
        output[i].xyz2 = ps2_pack_xyz2(
            vertex->x - (float)origin_x,
            vertex->y - (float)origin_y,
            vertex->z);
    }
}

static void ps2_make_alpha_trilerp_texture_triangle(
    const struct Ps2AlphaTrilerpVertex *source, int texture_index,
    int origin_x, int origin_y, bool capture_alpha, bool lerp_color,
    struct Ps2GsTexturedVertex output[3])
{
    for (uint32_t i = 0u; i < 3u; ++i) {
        const struct Ps2AlphaTrilerpVertex *vertex = &source[i];
        const uint8_t red = capture_alpha ? 0x80u : vertex->shade_r;
        const uint8_t green = capture_alpha ? 0x80u : vertex->shade_g;
        const uint8_t blue = capture_alpha ? 0x80u : vertex->shade_b;
        const uint8_t alpha = capture_alpha ? vertex->shade_a :
            (lerp_color ? vertex->lod : 0x80u);
        output[i].rgbaq = ps2_pack_rgbaq(
            red, green, blue, alpha, vertex->inv_w);
        output[i].st = ps2_pack_st(
            vertex->tex_u[texture_index] * vertex->inv_w,
            vertex->tex_v[texture_index] * vertex->inv_w);
        output[i].xyz2 = ps2_pack_xyz2(
            vertex->x - (float)origin_x,
            vertex->y - (float)origin_y,
            vertex->z);
    }
}

static void ps2_make_alpha_trilerp_add_triangle(
    const struct Ps2AlphaTrilerpVertex *source,
    int origin_x, int origin_y, struct Ps2GsColorVertex output[3])
{
    for (uint32_t i = 0u; i < 3u; ++i) {
        const struct Ps2AlphaTrilerpVertex *vertex = &source[i];
        const uint8_t add = vertex->alpha_add;
        output[i].rgbaq = ps2_pack_rgbaq(
            add, add, add, 0x80u, 0.0f);
        output[i].xyz2 = ps2_pack_xyz2(
            vertex->x - (float)origin_x,
            vertex->y - (float)origin_y,
            vertex->z);
    }
}

static void ps2_make_alpha_trilerp_workspace_triangle(
    const struct Ps2AlphaTrilerpVertex *source,
    int origin_x, int origin_y, uint8_t alpha, bool use_vertex_lod,
    bool screen_position, bool apply_fog,
    struct Ps2GsTexturedVertex output[3])
{
    for (uint32_t i = 0u; i < 3u; ++i) {
        const struct Ps2AlphaTrilerpVertex *vertex = &source[i];
        const float local_x = vertex->x - (float)origin_x;
        const float local_y = vertex->y - (float)origin_y;
        const struct Ps2GfxPassGraphSample sample =
            ps2GfxMapPassGraphSample(
                vertex->x, vertex->y, origin_x, origin_y);
        output[i].rgbaq = ps2_pack_rgbaq(
            0x80u, 0x80u, 0x80u,
            use_vertex_lod ? vertex->lod : alpha, 1.0f);
        output[i].st = ps2_pack_st(sample.s, sample.t);
        const float x = screen_position ? vertex->x : local_x;
        const float y = screen_position ? vertex->y : local_y;
        output[i].xyz2 = apply_fog && s_shader->features.opt_fog
            ? ps2_pack_xyzf2(x, y, vertex->z, vertex->fog)
            : ps2_pack_xyz2(x, y, vertex->z);
    }
}

static void ps2_restore_alpha_trilerp_state(void)
{
    ps2GsCoreBindDefaultRenderTarget();
    ps2GsCoreSetScissor(
        s_scissor.x, s_scissor.y, s_scissor.width, s_scissor.height);
    ps2GsCoreSetDepthMode(s_depth_test, s_depth_update, s_depth_compare,
        s_depth_compare_equal);
    ps2GsCoreSetColorWrite(true);
    ps2GsCoreSetAlphaWrite(true);
    ps2GsCoreSetAlphaBlend(s_alpha_blend);
    ps2GsCoreSetFramebufferAlphaForce(false);
    ps2GsCoreSetAlphaTest(
        s_shader && s_shader->features.opt_alpha_threshold,
        s_shader && s_shader->features.opt_alpha_threshold ?
            PS2_GFX_ALPHA_THRESHOLD : 0u);
    ps2GsCoreSetFog(
        s_shader && s_shader->features.opt_fog,
        s_draw_fog_r, s_draw_fog_g, s_draw_fog_b);
    ps2GsCoreSetTextureAlpha(
        s_shader && s_shader->plan.texture_alpha);
    ps2_apply_texture_clamp(0);
}

static void ps2_make_independent_tex0_alpha_texture_triangle(
    const struct Ps2IndependentTex0AlphaVertex *source,
    int origin_x, int origin_y, struct Ps2GsTexturedVertex output[3])
{
    for (uint32_t i = 0u; i < 3u; ++i) {
        const struct Ps2IndependentTex0AlphaVertex *vertex = &source[i];
        output[i].rgbaq = ps2_pack_rgbaq(
            0x80u, 0x80u, 0x80u, vertex->input[3], vertex->inv_w);
        output[i].st = ps2_pack_st(
            vertex->tex_u * vertex->inv_w,
            vertex->tex_v * vertex->inv_w);
        output[i].xyz2 = ps2_pack_xyz2(
            vertex->x - (float)origin_x,
            vertex->y - (float)origin_y,
            vertex->z);
    }
}

static void ps2_make_independent_tex0_alpha_color_triangle(
    const struct Ps2IndependentTex0AlphaVertex *source,
    int origin_x, int origin_y, struct Ps2GsColorVertex output[3])
{
    for (uint32_t i = 0u; i < 3u; ++i) {
        const struct Ps2IndependentTex0AlphaVertex *vertex = &source[i];
        output[i].rgbaq = ps2_pack_rgbaq(
            vertex->input[0], vertex->input[1], vertex->input[2],
            0x80u, 0.0f);
        output[i].xyz2 = ps2_pack_xyz2(
            vertex->x - (float)origin_x,
            vertex->y - (float)origin_y,
            vertex->z);
    }
}

static void ps2_make_independent_tex0_alpha_composite_triangle(
    const struct Ps2IndependentTex0AlphaVertex *source,
    int origin_x, int origin_y, struct Ps2GsTexturedVertex output[3])
{
    for (uint32_t i = 0u; i < 3u; ++i) {
        const struct Ps2IndependentTex0AlphaVertex *vertex = &source[i];
        const struct Ps2GfxPassGraphSample sample =
            ps2GfxMapPassGraphSample(
                vertex->x, vertex->y, origin_x, origin_y);
        output[i].rgbaq = ps2_pack_rgbaq(
            0x80u, 0x80u, 0x80u, 0x80u, 1.0f);
        output[i].st = ps2_pack_st(sample.s, sample.t);
        output[i].xyz2 = s_shader->features.opt_fog
            ? ps2_pack_xyzf2(
                vertex->x, vertex->y, vertex->z, vertex->fog)
            : ps2_pack_xyz2(vertex->x, vertex->y, vertex->z);
    }
}

static bool ps2_draw_independent_tex0_alpha_tile(
    const struct Ps2IndependentTex0AlphaVertex *triangle,
    const struct Ps2GfxPassGraphRect *tile)
{
    struct Ps2GsTexturedVertex alpha_vertices[3];
    struct Ps2GsColorVertex color_vertices[3];
    struct Ps2GsTexturedVertex composite_vertices[3];
    ps2_make_independent_tex0_alpha_texture_triangle(
        triangle, tile->x, tile->y, alpha_vertices);
    ps2_make_independent_tex0_alpha_color_triangle(
        triangle, tile->x, tile->y, color_vertices);
    ps2_make_independent_tex0_alpha_composite_triangle(
        triangle, tile->x, tile->y, composite_vertices);

    if (!ps2GsCoreBindRenderTarget(s_alpha_trilerp_color_target)) {
        return false;
    }
    ps2GsCoreSetAlphaTest(false, 0u);
    ps2GsCoreSetFog(false, 0u, 0u, 0u);
    ps2GsCoreSetDepthMode(false, false, false, false);
    ps2GsCoreSetAlphaBlend(false);
    ps2GsCoreSetFramebufferAlphaForce(false);
    ps2GsCoreSetColorWrite(true);
    ps2GsCoreSetAlphaWrite(true);
    ps2GsCoreClear(true, false);

    /* Capture TEXEL0 alpha without allowing its RGB lanes to reach the target. */
    ps2GsCoreSetColorWrite(false);
    ps2GsCoreSetTextureAlpha(true);
    ps2_apply_texture_clamp(0);
    ps2GsCoreDrawTexturedTriangles(
        s_selected_texture[0], alpha_vertices, 3u);

    /* Fill the independent INPUT1 RGB equation while preserving target alpha. */
    ps2GsCoreSetColorWrite(true);
    ps2GsCoreSetAlphaWrite(false);
    ps2GsCoreSetTextureAlpha(false);
    ps2GsCoreDrawColorTriangles(color_vertices, 3u);

    ps2GsCoreBindDefaultRenderTarget();
    ps2GsCoreSetScissor(tile->x, tile->y, tile->width, tile->height);
    ps2GsCoreSetDepthMode(s_depth_test, s_depth_update, s_depth_compare,
        s_depth_compare_equal);
    ps2GsCoreSetColorWrite(true);
    ps2GsCoreSetAlphaWrite(true);
    const bool texture_edge = s_shader->features.opt_texture_edge;
    ps2GsCoreSetAlphaTest(
        texture_edge || s_shader->features.opt_alpha_threshold,
        texture_edge ? PS2_GFX_TEXTURE_EDGE_THRESHOLD :
            (s_shader->features.opt_alpha_threshold ?
                PS2_GFX_ALPHA_THRESHOLD : 0u));
    ps2GsCoreSetFramebufferAlphaForce(texture_edge);
    ps2GsCoreSetAlphaBlend(texture_edge ? false : s_alpha_blend);
    ps2GsCoreSetTextureAlpha(true);
    ps2GsCoreSetFog(s_shader->features.opt_fog,
        s_draw_fog_r, s_draw_fog_g, s_draw_fog_b);
    return ps2GsCoreDrawRenderTargetTriangles(
        s_alpha_trilerp_color_target, composite_vertices, 3u, false);
}

static void ps2_make_independent_tex0_alpha_direct_texture_triangle(
    const struct Ps2IndependentTex0AlphaVertex *source,
    struct Ps2GsTexturedVertex output[3])
{
    for (uint32_t i = 0u; i < 3u; ++i) {
        const struct Ps2IndependentTex0AlphaVertex *vertex = &source[i];
        output[i].rgbaq = ps2_pack_rgbaq(
            0x80u, 0x80u, 0x80u, vertex->input[3], vertex->inv_w);
        output[i].st = ps2_pack_st(
            vertex->tex_u * vertex->inv_w,
            vertex->tex_v * vertex->inv_w);
        output[i].xyz2 = ps2_pack_xyz2(
            vertex->x, vertex->y, vertex->z);
    }
}

static void ps2_make_independent_tex0_alpha_direct_color_triangle(
    const struct Ps2IndependentTex0AlphaVertex *source,
    struct Ps2GsColorVertex output[3])
{
    for (uint32_t i = 0u; i < 3u; ++i) {
        const struct Ps2IndependentTex0AlphaVertex *vertex = &source[i];
        output[i].rgbaq = ps2_pack_rgbaq(
            vertex->input[0], vertex->input[1], vertex->input[2],
            0x80u, 0.0f);
        output[i].xyz2 = s_shader->features.opt_fog
            ? ps2_pack_xyzf2(
                vertex->x, vertex->y, vertex->z, vertex->fog)
            : ps2_pack_xyz2(vertex->x, vertex->y, vertex->z);
    }
}

static bool ps2_draw_independent_tex0_alpha_direct(uint32_t vertex_count)
{
    if (s_shader->features.opt_texture_edge ||
        s_shader->features.opt_alpha_threshold) {
        return false;
    }

    for (uint32_t vertex = 0u; vertex < vertex_count; vertex += 3u) {
        struct Ps2GsTexturedVertex alpha_vertices[3];
        struct Ps2GsColorVertex color_vertices[3];
        ps2_make_independent_tex0_alpha_direct_texture_triangle(
            &s_independent_tex0_alpha_vertices[vertex], alpha_vertices);
        ps2_make_independent_tex0_alpha_direct_color_triangle(
            &s_independent_tex0_alpha_vertices[vertex], color_vertices);

        /*
         * First materialize the source alpha in destination alpha only.
         * It observes the final draw's depth test but deliberately cannot
         * claim Z ownership before RGB is committed.
         */
        ps2GsCoreSetDepthMode(
            s_depth_test, false, s_depth_compare, s_depth_compare_equal);
        ps2GsCoreSetAlphaTest(false, 0u);
        ps2GsCoreSetFog(false, 0u, 0u, 0u);
        ps2GsCoreSetFramebufferAlphaForce(false);
        ps2GsCoreSetColorWrite(false);
        ps2GsCoreSetAlphaWrite(true);
        ps2GsCoreSetAlphaBlend(false);
        ps2GsCoreSetTextureAlpha(true);
        ps2_apply_texture_clamp(0);
        ps2GsCoreDrawTexturedTriangles(
            s_selected_texture[0], alpha_vertices, 3u);

        /*
         * GS ALPHA can use Ad as C. Since Ad now equals this primitive's
         * source alpha, (Cs-Cd)*Ad+Cd is the ordinary source-over equation
         * without an intermediate render target.
         */
        ps2GsCoreSetColorWrite(true);
        ps2GsCoreSetAlphaWrite(false);
        ps2GsCoreSetTextureAlpha(false);
        ps2GsCoreSetDepthMode(
            s_depth_test, s_depth_update, s_depth_compare,
            s_depth_compare_equal);
        ps2GsCoreSetFog(s_shader->features.opt_fog,
            s_draw_fog_r, s_draw_fog_g, s_draw_fog_b);
        if (s_alpha_blend) {
            /*
             * SetAlphaBlend(true) intentionally restores SOURCE_OVER in the
             * core, so enable blending first and only then install the
             * destination-alpha equation for this RGB pass.
             */
            ps2GsCoreSetAlphaBlend(true);
            ps2GsCoreSetAlphaBlendEquation(
                PS2_GS_ALPHA_BLEND_DESTINATION_ALPHA_LERP);
        } else {
            ps2GsCoreSetAlphaBlend(false);
        }
        ps2GsCoreDrawColorTriangles(color_vertices, 3u);
    }

    /* Restore the ordinary material equation before later draws inherit it. */
    ps2GsCoreSetAlphaBlendEquation(PS2_GS_ALPHA_BLEND_SOURCE_OVER);
    ps2_restore_alpha_trilerp_state();
    ps2RendererTraceRecord(PS2_TRACE_INDEPENDENT_ALPHA_DRAW,
        (uint16_t)PS2_TRACE_FLAG_SUPPORTED,
        vertex_count, vertex_count / 3u, 1u, 0u);
    return true;
}

static bool ps2_draw_independent_tex0_alpha_mask(uint32_t vertex_count)
{
    if (!ps2GsCoreTextureHasAlphaMask(s_selected_texture[0])) {
        return false;
    }

    for (uint32_t i = 0u; i < vertex_count; ++i) {
        const struct Ps2IndependentTex0AlphaVertex *vertex =
            &s_independent_tex0_alpha_vertices[i];
        s_stq_vertices[0][i].rgbaq = ps2_pack_rgbaq(
            vertex->input[0], vertex->input[1], vertex->input[2],
            vertex->input[3], vertex->inv_w);
        s_stq_vertices[0][i].st = ps2_pack_st(
            vertex->tex_u * vertex->inv_w,
            vertex->tex_v * vertex->inv_w);
        s_stq_vertices[0][i].xyz2 = s_shader->features.opt_fog
            ? ps2_pack_xyzf2(
                vertex->x, vertex->y, vertex->z, vertex->fog)
            : ps2_pack_xyz2(vertex->x, vertex->y, vertex->z);
    }

    const bool texture_edge = s_shader->features.opt_texture_edge;
    ps2GsCoreSetDepthMode(s_depth_test, s_depth_update, s_depth_compare,
        s_depth_compare_equal);
    ps2GsCoreSetColorWrite(true);
    ps2GsCoreSetAlphaWrite(true);
    ps2GsCoreSetAlphaTest(
        texture_edge || s_shader->features.opt_alpha_threshold,
        texture_edge ? PS2_GFX_TEXTURE_EDGE_THRESHOLD :
            (s_shader->features.opt_alpha_threshold ?
                PS2_GFX_ALPHA_THRESHOLD : 0u));
    ps2GsCoreSetFramebufferAlphaForce(texture_edge);
    ps2GsCoreSetAlphaBlend(texture_edge ? false : s_alpha_blend);
    ps2GsCoreSetTextureAlpha(true);
    ps2GsCoreSetFog(s_shader->features.opt_fog,
        s_draw_fog_r, s_draw_fog_g, s_draw_fog_b);
    ps2_apply_texture_clamp(0);

    const bool success = ps2GsCoreDrawTextureAlphaMaskTriangles(
        s_selected_texture[0], s_stq_vertices[0], vertex_count);
    if (success) {
        ps2RendererTraceRecord(PS2_TRACE_INDEPENDENT_ALPHA_DRAW,
            (uint16_t)PS2_TRACE_FLAG_SUPPORTED,
            vertex_count, vertex_count / 3u, 2u, 0u);
    }
    return success;
}

static bool ps2_draw_independent_tex0_alpha(uint32_t vertex_count)
{
    if (s_modulate) {
        if (!s_warned_independent_tex0_alpha_modulate) {
            sysLogPrintf(LOG_WARNING,
                "GfxPS2 independent TEXEL0 alpha rejects destination-color blend mode");
            s_warned_independent_tex0_alpha_modulate = true;
        }
        return false;
    }
#if defined(PERFECT_DARK_PS2_INDEPENDENT_ALPHA_MASK)
    if (ps2GsCoreTextureHasAlphaMask(s_selected_texture[0])) {
        return ps2_draw_independent_tex0_alpha_mask(vertex_count);
    }
#endif
#if defined(PERFECT_DARK_PS2_INDEPENDENT_ALPHA_DIRECT)
    if (!s_shader->features.opt_texture_edge &&
        !s_shader->features.opt_alpha_threshold) {
        return ps2_draw_independent_tex0_alpha_direct(vertex_count);
    }
#endif
    if (!ps2_ensure_alpha_trilerp_color_workspace()) {
        if (!s_warned_independent_tex0_alpha_workspace) {
            sysLogPrintf(LOG_ERROR,
                "GfxPS2 independent TEXEL0 alpha workspace allocation failed (%dx%d CT32)",
                PS2_GFX_PASS_GRAPH_TILE_WIDTH,
                PS2_GFX_PASS_GRAPH_TILE_HEIGHT);
            s_warned_independent_tex0_alpha_workspace = true;
        }
        return false;
    }

    int clip_x0 = s_scissor.x > 0 ? s_scissor.x : 0;
    int clip_y0 = s_scissor.y > 0 ? s_scissor.y : 0;
    int clip_x1 = s_scissor.x + s_scissor.width;
    int clip_y1 = s_scissor.y + s_scissor.height;
    const int screen_width = ps2GsCoreGetWidth();
    const int screen_height = ps2GsCoreGetHeight();
    if (clip_x1 > screen_width) clip_x1 = screen_width;
    if (clip_y1 > screen_height) clip_y1 = screen_height;
    if (clip_x0 >= clip_x1 || clip_y0 >= clip_y1) {
        return true;
    }
    const struct Ps2GfxPassGraphRect clip = {
        clip_x0, clip_y0, clip_x1 - clip_x0, clip_y1 - clip_y0,
    };

    bool success = true;
    for (uint32_t vertex = 0u; vertex < vertex_count && success;
         vertex += 3u) {
        struct Ps2GfxPassGraphTriangle geometry = {};
        for (uint32_t i = 0u; i < 3u; ++i) {
            geometry.x[i] =
                s_independent_tex0_alpha_vertices[vertex + i].x;
            geometry.y[i] =
                s_independent_tex0_alpha_vertices[vertex + i].y;
        }
        struct Ps2GfxPassGraphTiles tiles = {};
        if (!ps2GfxDescribePassGraphTiles(&geometry, &clip, &tiles)) {
            continue;
        }

        const uint32_t tile_count = tiles.columns * tiles.rows;
        for (uint32_t tile_index = 0u;
             tile_index < tile_count && success; ++tile_index) {
            struct Ps2GfxPassGraphRect tile = {};
            success = ps2GfxGetPassGraphTile(
                &tiles, tile_index, &tile) &&
                ps2_draw_independent_tex0_alpha_tile(
                    &s_independent_tex0_alpha_vertices[vertex], &tile);
        }
    }

    ps2_restore_alpha_trilerp_state();
    if (!success && !s_warned_independent_tex0_alpha_workspace) {
        sysLogPrintf(LOG_ERROR,
            "GfxPS2 independent TEXEL0 alpha submission failed");
        s_warned_independent_tex0_alpha_workspace = true;
    }
    return success;
}

static void ps2_make_tex0_factor_capture_triangle(
    const struct Ps2Tex0FactorLerpVertex *source,
    int texture_index, int origin_x, int origin_y,
    struct Ps2GsTexturedVertex output[3])
{
    for (uint32_t i = 0u; i < 3u; ++i) {
        const struct Ps2Tex0FactorLerpVertex *vertex = &source[i];
        /* Store every 0..255 texel lane as a GS-native 0..127 factor. */
        output[i].rgbaq = ps2_pack_rgbaq(
            0x40u, 0x40u, 0x40u, 0x40u, vertex->inv_w);
        output[i].st = ps2_pack_st(
            vertex->tex_u[texture_index] * vertex->inv_w,
            vertex->tex_v[texture_index] * vertex->inv_w);
        output[i].xyz2 = ps2_pack_xyz2(
            vertex->x - (float)origin_x,
            vertex->y - (float)origin_y, vertex->z);
    }
}

static void ps2_make_tex0_factor_texture_alpha_triangle(
    const struct Ps2Tex0FactorLerpVertex *source,
    int origin_x, int origin_y, struct Ps2GsTexturedVertex output[3])
{
    for (uint32_t i = 0u; i < 3u; ++i) {
        const struct Ps2Tex0FactorLerpVertex *vertex = &source[i];
        output[i].rgbaq = ps2_pack_rgbaq(
            0x80u, 0x80u, 0x80u,
            vertex->tex0_alpha_input, vertex->inv_w);
        output[i].st = ps2_pack_st(
            vertex->tex_u[0] * vertex->inv_w,
            vertex->tex_v[0] * vertex->inv_w);
        output[i].xyz2 = ps2_pack_xyz2(
            vertex->x - (float)origin_x,
            vertex->y - (float)origin_y, vertex->z);
    }
}

static void ps2_make_tex0_factor_solid_triangle(
    const struct Ps2Tex0FactorLerpVertex *source,
    int origin_x, int origin_y, bool input1, bool alpha_scalar,
    struct Ps2GsColorVertex output[3])
{
    for (uint32_t i = 0u; i < 3u; ++i) {
        const struct Ps2Tex0FactorLerpVertex *vertex = &source[i];
        const uint8_t *value = input1 ? vertex->input1 : vertex->input2;
        const uint8_t r = alpha_scalar ? value[3] : value[0];
        const uint8_t g = alpha_scalar ? value[3] : value[1];
        const uint8_t b = alpha_scalar ? value[3] : value[2];
        output[i].rgbaq = ps2_pack_rgbaq(r, g, b, 0x80u, 0.0f);
        output[i].xyz2 = ps2_pack_xyz2(
            vertex->x - (float)origin_x,
            vertex->y - (float)origin_y, vertex->z);
    }
}

static void ps2_make_tex0_factor_output_alpha_triangle(
    const struct Ps2Tex0FactorLerpVertex *source,
    int origin_x, int origin_y, bool use_input1,
    struct Ps2GsColorVertex output[3])
{
    for (uint32_t i = 0u; i < 3u; ++i) {
        const struct Ps2Tex0FactorLerpVertex *vertex = &source[i];
        const uint8_t alpha = use_input1
            ? vertex->input1[3] : 0x80u;
        output[i].rgbaq = ps2_pack_rgbaq(
            0u, 0u, 0u, alpha, 0.0f);
        output[i].xyz2 = ps2_pack_xyz2(
            vertex->x - (float)origin_x,
            vertex->y - (float)origin_y, vertex->z);
    }
}

static void ps2_make_tex0_factor_workspace_sample(
    const struct Ps2Tex0FactorLerpVertex *source,
    int origin_x, int origin_y, bool screen_position,
    uint8_t alpha, bool use_input1_alpha, bool apply_fog,
    struct Ps2GsTexturedVertex output[3])
{
    for (uint32_t i = 0u; i < 3u; ++i) {
        const struct Ps2Tex0FactorLerpVertex *vertex = &source[i];
        const struct Ps2GfxPassGraphSample sample =
            ps2GfxMapPassGraphSample(
                vertex->x, vertex->y, origin_x, origin_y);
        output[i].rgbaq = ps2_pack_rgbaq(
            0x80u, 0x80u, 0x80u,
            use_input1_alpha ? vertex->input1[3] : alpha, 1.0f);
        output[i].st = ps2_pack_st(sample.s, sample.t);
        const float x = screen_position ? vertex->x :
            vertex->x - (float)origin_x;
        const float y = screen_position ? vertex->y :
            vertex->y - (float)origin_y;
        output[i].xyz2 = apply_fog && s_shader->features.opt_fog
            ? ps2_pack_xyzf2(x, y, vertex->z, vertex->fog)
            : ps2_pack_xyz2(x, y, vertex->z);
    }
}

static bool ps2_draw_tex0_factor_lerp_tile(
    const struct Ps2Tex0FactorLerpVertex *triangle,
    const struct Ps2GfxPassGraphRect *tile, bool monochrome)
{
    struct Ps2GsTexturedVertex capture[3];
    struct Ps2GsTexturedVertex alpha_sample[3];
    struct Ps2GsTexturedVertex tex0_alpha[3];
    struct Ps2GsTexturedVertex composite[3];
    struct Ps2GsColorVertex base_rgb[3];
    struct Ps2GsColorVertex source_rgb[3];
    struct Ps2GsColorVertex base_alpha[3];
    struct Ps2GsColorVertex source_alpha[3];
    struct Ps2GsColorVertex output_alpha[3];
    const bool tex1_alpha_factor = s_shader->plan.pass_graph ==
        PS2_PASS_GRAPH_TEX1_ALPHA_FACTOR_LERP;
    const int factor_texture = tex1_alpha_factor ? 1 : 0;
    ps2_make_tex0_factor_capture_triangle(
        triangle, factor_texture, tile->x, tile->y, capture);
    ps2_make_tex0_factor_workspace_sample(
        triangle, tile->x, tile->y, false,
        0u, true, false, alpha_sample);
    ps2_make_tex0_factor_texture_alpha_triangle(
        triangle, tile->x, tile->y, tex0_alpha);
    ps2_make_tex0_factor_workspace_sample(
        triangle, tile->x, tile->y, true,
        0x80u, false, true, composite);
    ps2_make_tex0_factor_solid_triangle(
        triangle, tile->x, tile->y, false, false, base_rgb);
    ps2_make_tex0_factor_solid_triangle(
        triangle, tile->x, tile->y, true, false, source_rgb);
    ps2_make_tex0_factor_solid_triangle(
        triangle, tile->x, tile->y, false, true, base_alpha);
    ps2_make_tex0_factor_solid_triangle(
        triangle, tile->x, tile->y, true, true, source_alpha);
    ps2_make_tex0_factor_output_alpha_triangle(
        triangle, tile->x, tile->y,
        s_shader->plan.alpha_recipe == PS2_ALPHA_INPUT1,
        output_alpha);

    /* Capture T.rgba/2 once. It supplies GS ALPHA's native 0..128 factors. */
    ps2GsCoreSetColorWrite(true);
    ps2GsCoreSetAlphaWrite(true);
    ps2GsCoreSetAlphaTest(false, 0u);
    ps2GsCoreSetFog(false, 0u, 0u, 0u);
    ps2GsCoreSetDepthMode(false, false, false, false);
    ps2GsCoreSetAlphaBlend(false);
    ps2GsCoreSetFramebufferAlphaForce(false);
    ps2GsCoreSetTextureAlpha(true);
    if (!ps2GsCoreBindRenderTarget(s_alpha_trilerp_scalar_target)) {
        return false;
    }
    ps2GsCoreClear(true, false);
    ps2_apply_texture_clamp(factor_texture);
    ps2GsCoreDrawTexturedTriangles(
        s_selected_texture[factor_texture], capture, 3u);

    const bool alpha_lerp = s_shader->plan.alpha_recipe ==
            PS2_ALPHA_INPUT2_INPUT1_LERP_TEX0 ||
        s_shader->plan.alpha_recipe ==
            PS2_ALPHA_INPUT2_INPUT1_COVERAGE_LERP_TEX0;
    if (alpha_lerp) {
        /* Build lerp(INPUT2.a, INPUT1.a, TEXEL0.a) in output red. */
        if (!ps2GsCoreBindRenderTarget(s_alpha_trilerp_color_target)) {
            return false;
        }
        ps2GsCoreSetColorWrite(true);
        ps2GsCoreSetAlphaWrite(true);
        ps2GsCoreSetAlphaBlend(false);
        ps2GsCoreClear(true, false);
        ps2GsCoreSetColorChannelWriteMask(PS2_GS_COLOR_WRITE_RED);
        ps2GsCoreSetAlphaWrite(false);
        ps2GsCoreDrawColorTriangles(base_alpha, 3u);
        if (!ps2GsCoreBlitRenderTargetChannelRectToActiveAlpha(
                s_alpha_trilerp_scalar_target,
                PS2_GS_CT32_CHANNEL_ALPHA,
                (uint32_t)tile->width, (uint32_t)tile->height)) {
            return false;
        }
        ps2GsCoreSetAlphaBlend(true);
        ps2GsCoreSetAlphaBlendEquation(
            PS2_GS_ALPHA_BLEND_DESTINATION_ALPHA_LERP);
        ps2GsCoreDrawColorTriangles(source_alpha, 3u);

        /* Keep the scalar in the raw target alpha while RGB is rebuilt. */
        if (!ps2GsCoreBindRenderTarget(s_alpha_trilerp_scalar_target) ||
            !ps2GsCoreBlitRenderTargetChannelRectToActiveAlpha(
                s_alpha_trilerp_color_target,
                PS2_GS_CT32_CHANNEL_RED,
                (uint32_t)tile->width, (uint32_t)tile->height)) {
            return false;
        }
    }

    if (!ps2GsCoreBindRenderTarget(s_alpha_trilerp_color_target)) {
        return false;
    }
    ps2GsCoreSetColorWrite(true);
    ps2GsCoreSetAlphaWrite(true);
    ps2GsCoreSetAlphaBlend(false);
    ps2GsCoreClear(true, false);
    ps2GsCoreSetAlphaWrite(false);
    ps2GsCoreDrawColorTriangles(base_rgb, 3u);

    static const enum Ps2GsCt32Channel channels[3] = {
        PS2_GS_CT32_CHANNEL_RED,
        PS2_GS_CT32_CHANNEL_GREEN,
        PS2_GS_CT32_CHANNEL_BLUE,
    };
    static const uint8_t write_masks[3] = {
        PS2_GS_COLOR_WRITE_RED,
        PS2_GS_COLOR_WRITE_GREEN,
        PS2_GS_COLOR_WRITE_BLUE,
    };
    const uint32_t channel_count = tex1_alpha_factor
        ? 1u : gfxPs2MaterialRgbChannelPasses(monochrome);
    for (uint32_t channel = 0u; channel < channel_count; ++channel) {
        if (!ps2GsCoreBlitRenderTargetChannelRectToActiveAlpha(
                s_alpha_trilerp_scalar_target,
                tex1_alpha_factor ? PS2_GS_CT32_CHANNEL_ALPHA
                                  : channels[channel],
                (uint32_t)tile->width, (uint32_t)tile->height)) {
            return false;
        }
        ps2GsCoreSetColorChannelWriteMask(
            (monochrome || tex1_alpha_factor)
                ? (uint8_t)PS2_GS_COLOR_WRITE_RGB :
                write_masks[channel]);
        ps2GsCoreSetAlphaWrite(false);
        ps2GsCoreSetAlphaBlend(true);
        ps2GsCoreSetAlphaBlendEquation(
            PS2_GS_ALPHA_BLEND_DESTINATION_ALPHA_LERP);
        ps2GsCoreDrawColorTriangles(source_rgb, 3u);
    }

    ps2GsCoreSetColorWrite(false);
    ps2GsCoreSetAlphaWrite(true);
    ps2GsCoreSetAlphaBlend(false);
    ps2GsCoreSetTextureAlpha(true);
    if (alpha_lerp) {
        if (!ps2GsCoreBlitRenderTargetChannelRectToActiveAlpha(
                s_alpha_trilerp_scalar_target,
                PS2_GS_CT32_CHANNEL_ALPHA,
                (uint32_t)tile->width, (uint32_t)tile->height)) {
            return false;
        }
    } else if (tex1_alpha_factor &&
        s_shader->plan.alpha_recipe == PS2_ALPHA_TEX0_MUL_INPUT1) {
        ps2_apply_texture_clamp(0);
        ps2GsCoreDrawTexturedTriangles(
            s_selected_texture[0], tex0_alpha, 3u);
    } else if (s_shader->plan.alpha_recipe == PS2_ALPHA_INPUT1 ||
        s_shader->plan.alpha_recipe == PS2_ALPHA_OPAQUE ||
        s_shader->plan.alpha_recipe == PS2_ALPHA_ONE) {
        ps2GsCoreDrawColorTriangles(output_alpha, 3u);
    } else if (!ps2GsCoreDrawRenderTargetAlphaTriangles(
            s_alpha_trilerp_scalar_target,
            alpha_sample, 3u, false)) {
        return false;
    }

    ps2GsCoreBindDefaultRenderTarget();
    ps2GsCoreSetScissor(tile->x, tile->y, tile->width, tile->height);
    ps2GsCoreSetDepthMode(s_depth_test, s_depth_update, s_depth_compare,
        s_depth_compare_equal);
    ps2GsCoreSetColorWrite(true);
    ps2GsCoreSetAlphaWrite(true);
    ps2GsCoreSetAlphaTest(
        s_shader->features.opt_alpha_threshold,
        s_shader->features.opt_alpha_threshold ?
            PS2_GFX_ALPHA_THRESHOLD : 0u);
    ps2GsCoreSetFog(s_shader->features.opt_fog,
        s_draw_fog_r, s_draw_fog_g, s_draw_fog_b);
    ps2GsCoreSetTextureAlpha(true);
    ps2GsCoreSetAlphaBlend(s_alpha_blend);
    return ps2GsCoreDrawRenderTargetTriangles(
        s_alpha_trilerp_color_target, composite, 3u, false);
}

static bool ps2_draw_tex0_factor_lerp(uint32_t vertex_count)
{
    if (s_modulate) {
        if (!s_warned_tex0_factor_modulate) {
            sysLogPrintf(LOG_WARNING,
                "GfxPS2 texture-factor lerp rejects destination-color blend mode");
            s_warned_tex0_factor_modulate = true;
        }
        return false;
    }
    if (!ps2_ensure_alpha_trilerp_workspace()) {
        return false;
    }

    const bool tex1_alpha_factor = s_shader->plan.pass_graph ==
        PS2_PASS_GRAPH_TEX1_ALPHA_FACTOR_LERP;
    const Ps2GsTextureHandle handle =
        s_selected_texture[tex1_alpha_factor ? 1 : 0];
    const bool monochrome = tex1_alpha_factor ||
        (handle < PS2_GFX_TEXTURE_STATE_SLOTS &&
         s_texture_sampler[handle].monochrome_rgb);
    if (monochrome && !s_logged_tex0_factor_scalar) {
        sysLogPrintf(LOG_NOTE,
            "GfxPS2 texture-factor material uses one-channel RGB graph");
        s_logged_tex0_factor_scalar = true;
    } else if (!monochrome && !s_logged_tex0_factor_vector) {
        sysLogPrintf(LOG_NOTE,
            "GfxPS2 texture-factor material uses exact three-channel RGB graph");
        s_logged_tex0_factor_vector = true;
    }

    int clip_x0 = s_scissor.x > 0 ? s_scissor.x : 0;
    int clip_y0 = s_scissor.y > 0 ? s_scissor.y : 0;
    int clip_x1 = s_scissor.x + s_scissor.width;
    int clip_y1 = s_scissor.y + s_scissor.height;
    const int screen_width = ps2GsCoreGetWidth();
    const int screen_height = ps2GsCoreGetHeight();
    if (clip_x1 > screen_width) clip_x1 = screen_width;
    if (clip_y1 > screen_height) clip_y1 = screen_height;
    if (clip_x0 >= clip_x1 || clip_y0 >= clip_y1) {
        return true;
    }
    const struct Ps2GfxPassGraphRect clip = {
        clip_x0, clip_y0, clip_x1 - clip_x0, clip_y1 - clip_y0,
    };

    bool success = true;
    for (uint32_t vertex = 0u;
         vertex < vertex_count && success; vertex += 3u) {
        struct Ps2GfxPassGraphTriangle geometry = {};
        for (uint32_t i = 0u; i < 3u; ++i) {
            geometry.x[i] = s_tex0_factor_vertices[vertex + i].x;
            geometry.y[i] = s_tex0_factor_vertices[vertex + i].y;
        }
        struct Ps2GfxPassGraphTiles tiles = {};
        if (!ps2GfxDescribePassGraphTiles(&geometry, &clip, &tiles)) {
            continue;
        }
        const uint32_t tile_count = tiles.columns * tiles.rows;
        for (uint32_t tile_index = 0u;
             tile_index < tile_count && success; ++tile_index) {
            struct Ps2GfxPassGraphRect tile = {};
            success = ps2GfxGetPassGraphTile(
                &tiles, tile_index, &tile) &&
                ps2_draw_tex0_factor_lerp_tile(
                    &s_tex0_factor_vertices[vertex],
                    &tile, monochrome);
        }
    }

    ps2_restore_alpha_trilerp_state();
    if (!success && !s_warned_tex0_factor_workspace) {
        sysLogPrintf(LOG_ERROR,
            "GfxPS2 texture-factor material graph submission failed");
        s_warned_tex0_factor_workspace = true;
    }
    return success;
}

static void ps2_make_interference_texture_triangle(
    const struct Ps2InterferenceVertex *source, int texture_index,
    int origin_x, int origin_y, bool half_scale, bool shade,
    struct Ps2GsTexturedVertex output[3])
{
    for (uint32_t i = 0u; i < 3u; ++i) {
        const struct Ps2InterferenceVertex *vertex = &source[i];
        const uint8_t r = half_scale ? 0x40u :
            (shade ? vertex->shade[0] : 0x80u);
        const uint8_t g = half_scale ? 0x40u :
            (shade ? vertex->shade[1] : 0x80u);
        const uint8_t b = half_scale ? 0x40u :
            (shade ? vertex->shade[2] : 0x80u);
        const uint8_t a = half_scale ? 0x40u :
            (shade ? vertex->shade[3] : 0x80u);
        output[i].rgbaq = ps2_pack_rgbaq(r, g, b, a, vertex->inv_w);
        output[i].st = ps2_pack_st(
            vertex->tex_u[texture_index] * vertex->inv_w,
            vertex->tex_v[texture_index] * vertex->inv_w);
        output[i].xyz2 = ps2_pack_xyz2(
            vertex->x - (float)origin_x,
            vertex->y - (float)origin_y, vertex->z);
    }
}

static void ps2_make_interference_solid_triangle(
    const struct Ps2InterferenceVertex *source,
    int origin_x, int origin_y, struct Ps2GsColorVertex output[3])
{
    for (uint32_t i = 0u; i < 3u; ++i) {
        const struct Ps2InterferenceVertex *vertex = &source[i];
        output[i].rgbaq = ps2_pack_rgbaq(
            0x80u, 0x80u, 0x80u, 0x80u, 0.0f);
        output[i].xyz2 = ps2_pack_xyz2(
            vertex->x - (float)origin_x,
            vertex->y - (float)origin_y, vertex->z);
    }
}

static void ps2_make_interference_workspace_triangle(
    const struct Ps2InterferenceVertex *source,
    int origin_x, int origin_y, bool screen_position, bool apply_fog,
    struct Ps2GsTexturedVertex output[3])
{
    for (uint32_t i = 0u; i < 3u; ++i) {
        const struct Ps2InterferenceVertex *vertex = &source[i];
        const struct Ps2GfxPassGraphSample sample =
            ps2GfxMapPassGraphSample(
                vertex->x, vertex->y, origin_x, origin_y);
        output[i].rgbaq = ps2_pack_rgbaq(
            0x80u, 0x80u, 0x80u, 0x80u, 1.0f);
        output[i].st = ps2_pack_st(sample.s, sample.t);
        const float x = screen_position ? vertex->x :
            vertex->x - (float)origin_x;
        const float y = screen_position ? vertex->y :
            vertex->y - (float)origin_y;
        output[i].xyz2 = apply_fog && s_shader->features.opt_fog
            ? ps2_pack_xyzf2(x, y, vertex->z, vertex->fog)
            : ps2_pack_xyz2(x, y, vertex->z);
    }
}

static bool ps2_draw_interference_tile(
    const struct Ps2InterferenceVertex *triangle,
    const struct Ps2GfxPassGraphRect *tile, bool monochrome)
{
    struct Ps2GsTexturedVertex tex0_factor[3];
    struct Ps2GsTexturedVertex tex0_alpha[3];
    struct Ps2GsTexturedVertex tex1_shade[3];
    struct Ps2GsTexturedVertex alpha_view[3];
    struct Ps2GsTexturedVertex composite[3];
    struct Ps2GsColorVertex multiplier[3];
    ps2_make_interference_texture_triangle(
        triangle, 0, tile->x, tile->y, true, false, tex0_factor);
    ps2_make_interference_texture_triangle(
        triangle, 0, tile->x, tile->y, false, false, tex0_alpha);
    ps2_make_interference_texture_triangle(
        triangle, 1, tile->x, tile->y, false, true, tex1_shade);
    ps2_make_interference_workspace_triangle(
        triangle, tile->x, tile->y, false, false, alpha_view);
    ps2_make_interference_workspace_triangle(
        triangle, tile->x, tile->y, true, true, composite);
    ps2_make_interference_solid_triangle(
        triangle, tile->x, tile->y, multiplier);

    ps2GsCoreSetColorWrite(true);
    ps2GsCoreSetAlphaWrite(true);
    ps2GsCoreSetAlphaTest(false, 0u);
    ps2GsCoreSetFog(false, 0u, 0u, 0u);
    ps2GsCoreSetDepthMode(false, false, false, false);
    ps2GsCoreSetAlphaBlend(false);
    ps2GsCoreSetFramebufferAlphaForce(false);
    ps2GsCoreSetTextureAlpha(true);

    /* Keep TEXEL0 RGB as half-range GS factors in the scalar target. */
    if (!ps2GsCoreBindRenderTarget(s_alpha_trilerp_scalar_target)) {
        return false;
    }
    ps2GsCoreClear(true, false);
    ps2_apply_texture_clamp(0);
    ps2GsCoreDrawTexturedTriangles(
        s_selected_texture[0], tex0_factor, 3u);

    /* TEXEL1 * SHADE establishes both the color base and partial alpha. */
    if (!ps2GsCoreBindRenderTarget(s_alpha_trilerp_color_target)) {
        return false;
    }
    ps2GsCoreClear(true, false);
    ps2_apply_texture_clamp(1);
    ps2GsCoreDrawTexturedTriangles(
        s_selected_texture[1], tex1_shade, 3u);

    /* Preserve TEXEL1.a * SHADE.a while color target alpha is scratch. */
    if (!ps2GsCoreBindRenderTarget(s_alpha_trilerp_scalar_target) ||
        !ps2GsCoreBlitRenderTargetChannelRectToActiveAlpha(
            s_alpha_trilerp_color_target,
            PS2_GS_CT32_CHANNEL_ALPHA,
            (uint32_t)tile->width, (uint32_t)tile->height) ||
        !ps2GsCoreBindRenderTarget(s_alpha_trilerp_color_target)) {
        return false;
    }

    static const enum Ps2GsCt32Channel channels[3] = {
        PS2_GS_CT32_CHANNEL_RED,
        PS2_GS_CT32_CHANNEL_GREEN,
        PS2_GS_CT32_CHANNEL_BLUE,
    };
    static const uint8_t write_masks[3] = {
        PS2_GS_COLOR_WRITE_RED,
        PS2_GS_COLOR_WRITE_GREEN,
        PS2_GS_COLOR_WRITE_BLUE,
    };
    const uint32_t channel_count =
        gfxPs2MaterialRgbChannelPasses(monochrome);
    for (uint32_t channel = 0u; channel < channel_count; ++channel) {
        if (!ps2GsCoreBlitRenderTargetChannelRectToActiveAlpha(
                s_alpha_trilerp_scalar_target, channels[channel],
                (uint32_t)tile->width, (uint32_t)tile->height)) {
            return false;
        }
        ps2GsCoreSetColorChannelWriteMask(
            monochrome ? (uint8_t)PS2_GS_COLOR_WRITE_RGB :
                write_masks[channel]);
        ps2GsCoreSetAlphaWrite(false);
        ps2GsCoreSetAlphaBlend(true);
        ps2GsCoreSetAlphaBlendEquation(
            PS2_GS_ALPHA_BLEND_DESTINATION_RGB_TIMES_DESTINATION_ALPHA);
        ps2GsCoreDrawColorTriangles(multiplier, 3u);
    }

    /* Restore partial alpha before sampling it into the scalar target. */
    ps2GsCoreSetColorWrite(false);
    ps2GsCoreSetAlphaWrite(true);
    ps2GsCoreSetAlphaBlend(false);
    if (!ps2GsCoreBlitRenderTargetChannelRectToActiveAlpha(
            s_alpha_trilerp_scalar_target,
            PS2_GS_CT32_CHANNEL_ALPHA,
            (uint32_t)tile->width, (uint32_t)tile->height) ||
        !ps2GsCoreBindRenderTarget(s_alpha_trilerp_scalar_target)) {
        return false;
    }

    ps2GsCoreSetColorWrite(true);
    ps2GsCoreSetAlphaWrite(true);
    ps2GsCoreClear(true, false);
    if (!ps2GsCoreDrawRenderTargetAlphaTriangles(
            s_alpha_trilerp_color_target,
            alpha_view, 3u, false)) {
        return false;
    }
    ps2GsCoreSetColorChannelWriteMask(PS2_GS_COLOR_WRITE_RED);
    ps2GsCoreSetAlphaWrite(false);
    ps2GsCoreSetAlphaBlend(true);
    ps2GsCoreSetAlphaBlendEquation(
        PS2_GS_ALPHA_BLEND_DESTINATION_RGB_TIMES_SOURCE_ALPHA);
    ps2GsCoreSetTextureAlpha(true);
    ps2_apply_texture_clamp(0);
    ps2GsCoreDrawTexturedTriangles(
        s_selected_texture[0], tex0_alpha, 3u);

    if (!ps2GsCoreBindRenderTarget(s_alpha_trilerp_color_target)) {
        return false;
    }
    ps2GsCoreSetColorWrite(false);
    ps2GsCoreSetAlphaWrite(true);
    ps2GsCoreSetAlphaBlend(false);
    if (!ps2GsCoreBlitRenderTargetChannelRectToActiveAlpha(
            s_alpha_trilerp_scalar_target,
            PS2_GS_CT32_CHANNEL_RED,
            (uint32_t)tile->width, (uint32_t)tile->height)) {
        return false;
    }

    ps2GsCoreBindDefaultRenderTarget();
    ps2GsCoreSetScissor(tile->x, tile->y, tile->width, tile->height);
    ps2GsCoreSetDepthMode(s_depth_test, s_depth_update, s_depth_compare,
        s_depth_compare_equal);
    ps2GsCoreSetColorWrite(true);
    ps2GsCoreSetAlphaWrite(true);
    ps2GsCoreSetAlphaTest(
        s_shader->features.opt_alpha_threshold,
        s_shader->features.opt_alpha_threshold ?
            PS2_GFX_ALPHA_THRESHOLD : 0u);
    ps2GsCoreSetFog(s_shader->features.opt_fog,
        s_draw_fog_r, s_draw_fog_g, s_draw_fog_b);
    ps2GsCoreSetTextureAlpha(true);
    ps2GsCoreSetAlphaBlend(s_alpha_blend);
    return ps2GsCoreDrawRenderTargetTriangles(
        s_alpha_trilerp_color_target, composite, 3u, false);
}

static bool ps2_draw_interference(uint32_t vertex_count)
{
    if (s_modulate) {
        if (!s_warned_interference_modulate) {
            sysLogPrintf(LOG_WARNING,
                "GfxPS2 INTERFERENCE rejects destination-color blend mode");
            s_warned_interference_modulate = true;
        }
        return false;
    }
    if (!ps2_ensure_alpha_trilerp_workspace()) {
        return false;
    }

    const Ps2GsTextureHandle handle = s_selected_texture[0];
    const bool monochrome = handle < PS2_GFX_TEXTURE_STATE_SLOTS &&
        s_texture_sampler[handle].monochrome_rgb;
    if (monochrome && !s_logged_interference_scalar) {
        sysLogPrintf(LOG_NOTE,
            "GfxPS2 INTERFERENCE uses one-channel TEXEL0 RGB factor");
        s_logged_interference_scalar = true;
    } else if (!monochrome && !s_logged_interference_vector) {
        sysLogPrintf(LOG_NOTE,
            "GfxPS2 INTERFERENCE uses exact three-channel TEXEL0 RGB factor");
        s_logged_interference_vector = true;
    }

    int clip_x0 = s_scissor.x > 0 ? s_scissor.x : 0;
    int clip_y0 = s_scissor.y > 0 ? s_scissor.y : 0;
    int clip_x1 = s_scissor.x + s_scissor.width;
    int clip_y1 = s_scissor.y + s_scissor.height;
    const int screen_width = ps2GsCoreGetWidth();
    const int screen_height = ps2GsCoreGetHeight();
    if (clip_x1 > screen_width) clip_x1 = screen_width;
    if (clip_y1 > screen_height) clip_y1 = screen_height;
    if (clip_x0 >= clip_x1 || clip_y0 >= clip_y1) {
        return true;
    }
    const struct Ps2GfxPassGraphRect clip = {
        clip_x0, clip_y0, clip_x1 - clip_x0, clip_y1 - clip_y0,
    };

    bool success = true;
    for (uint32_t vertex = 0u;
         vertex < vertex_count && success; vertex += 3u) {
        struct Ps2GfxPassGraphTriangle geometry = {};
        for (uint32_t i = 0u; i < 3u; ++i) {
            geometry.x[i] = s_interference_vertices[vertex + i].x;
            geometry.y[i] = s_interference_vertices[vertex + i].y;
        }
        struct Ps2GfxPassGraphTiles tiles = {};
        if (!ps2GfxDescribePassGraphTiles(&geometry, &clip, &tiles)) {
            continue;
        }
        const uint32_t tile_count = tiles.columns * tiles.rows;
        for (uint32_t tile_index = 0u;
             tile_index < tile_count && success; ++tile_index) {
            struct Ps2GfxPassGraphRect tile = {};
            success = ps2GfxGetPassGraphTile(
                &tiles, tile_index, &tile) &&
                ps2_draw_interference_tile(
                    &s_interference_vertices[vertex],
                    &tile, monochrome);
        }
    }

    ps2_restore_alpha_trilerp_state();
    if (!success && !s_warned_interference_workspace) {
        sysLogPrintf(LOG_ERROR,
            "GfxPS2 INTERFERENCE material graph submission failed");
        s_warned_interference_workspace = true;
    }
    return success;
}

static bool ps2_draw_custom24_nonlinear_alpha_tile(
    const struct Ps2AlphaTrilerpVertex *triangle,
    const struct Ps2GfxPassGraphRect *tile)
{
    struct Ps2GsTexturedVertex texture0[3];
    struct Ps2GsTexturedVertex texture1[3];
    struct Ps2GsTexturedVertex composite[3];
    struct Ps2GsColorVertex scalar[3];
    ps2_make_independent_alpha_texture_triangle(
        triangle, 0, tile->x, tile->y, false, texture0);
    ps2_make_independent_alpha_texture_triangle(
        triangle, 1, tile->x, tile->y, true, texture1);
    ps2_make_alpha_trilerp_workspace_triangle(
        triangle, tile->x, tile->y, 0x80u, false, true, false,
        composite);
    ps2_make_custom24_scalar_triangle(
        triangle, tile->x, tile->y, scalar);

    /*
     * CUSTOM_24 alpha is ENV.a * SHADE.a * (1 - SHADE.a). The vertex RGB
     * carries the linear ENV.a * SHADE.a base and source alpha carries
     * SHADE.a. GS ALPHA (0-Cs)*As+Cs evaluates the remaining nonlinear factor
     * per fragment into the scalar target's red lane.
     */
    if (!ps2GsCoreBindRenderTarget(s_alpha_trilerp_scalar_target)) {
        return false;
    }
    ps2GsCoreSetAlphaWrite(true);
    ps2GsCoreSetAlphaTest(false, 0u);
    ps2GsCoreSetFog(false, 0u, 0u, 0u);
    ps2GsCoreSetDepthMode(false, false, false, false);
    ps2GsCoreSetAlphaBlend(false);
    ps2GsCoreSetFramebufferAlphaForce(false);
    ps2GsCoreSetTextureAlpha(false);
    ps2GsCoreClear(true, false);
    ps2GsCoreSetAlphaBlend(true);
    ps2GsCoreSetAlphaBlendEquation(
        PS2_GS_ALPHA_BLEND_SOURCE_RGB_TIMES_INV_SOURCE_ALPHA);
    ps2GsCoreDrawColorTriangles(scalar, 3u);

    if (!ps2GsCoreBindRenderTarget(s_alpha_trilerp_color_target)) {
        return false;
    }
    ps2GsCoreSetAlphaBlend(false);
    ps2GsCoreSetAlphaWrite(false);
    ps2GsCoreSetFog(s_shader->features.opt_fog,
        s_draw_fog_r, s_draw_fog_g, s_draw_fog_b);
    ps2GsCoreClear(true, false);
    ps2_apply_texture_clamp(0);
    ps2GsCoreDrawTexturedTriangles(
        s_selected_texture[0], texture0, 3u);
    ps2GsCoreSetAlphaBlend(true);
    ps2_apply_texture_clamp(1);
    ps2GsCoreDrawTexturedTriangles(
        s_selected_texture[1], texture1, 3u);

    ps2GsCoreSetAlphaWrite(true);
    ps2GsCoreSetAlphaBlend(false);
    if (!ps2GsCoreBlitRenderTargetChannelRectToActiveAlpha(
            s_alpha_trilerp_scalar_target, PS2_GS_CT32_CHANNEL_RED,
            (uint32_t)tile->width, (uint32_t)tile->height)) {
        return false;
    }

    ps2GsCoreBindDefaultRenderTarget();
    ps2GsCoreSetScissor(tile->x, tile->y, tile->width, tile->height);
    ps2GsCoreSetDepthMode(s_depth_test, s_depth_update, s_depth_compare,
        s_depth_compare_equal);
    ps2GsCoreSetAlphaTest(
        s_shader->features.opt_alpha_threshold,
        s_shader->features.opt_alpha_threshold ?
            PS2_GFX_ALPHA_THRESHOLD : 0u);
    ps2GsCoreSetFog(false, 0u, 0u, 0u);
    ps2GsCoreSetTextureAlpha(true);
    ps2GsCoreSetAlphaBlend(s_alpha_blend);
    return ps2GsCoreDrawRenderTargetTriangles(
        s_alpha_trilerp_color_target, composite, 3u, false);
}

static void ps2_make_signed_alpha_triangle(
    const struct Ps2AlphaTrilerpVertex *source,
    struct Ps2GfxSignedAlphaVertex output[3])
{
    for (uint32_t i = 0u; i < 3u; ++i) {
        output[i].x = source[i].x;
        output[i].y = source[i].y;
        output[i].z = (float)source[i].z;
        output[i].s = source[i].tex_u[0] * source[i].inv_w;
        output[i].t = source[i].tex_v[0] * source[i].inv_w;
        output[i].q = source[i].inv_w;
        output[i].delta = source[i].signed_alpha_delta;
    }
}

static void ps2_make_signed_alpha_texture_vertices(
    const struct Ps2GfxSignedAlphaTriangles *source,
    int origin_x, int origin_y,
    struct Ps2GsTexturedVertex output[
        PS2_GFX_SIGNED_ALPHA_MAX_TRIANGLE_VERTICES])
{
    for (uint32_t i = 0u; i < source->vertex_count; ++i) {
        const struct Ps2GfxSignedAlphaVertex *vertex =
            &source->vertices[i];
        const float magnitude = vertex->delta < 0.0f ?
            -vertex->delta : vertex->delta;
        output[i].rgbaq = ps2_pack_rgbaq(
            0x80u, 0x80u, 0x80u,
            ps2_texture_alpha_fragment_component(magnitude),
            vertex->q);
        output[i].st = ps2_pack_st(vertex->s, vertex->t);
        output[i].xyz2 = ps2_pack_xyz2(
            vertex->x - (float)origin_x,
            vertex->y - (float)origin_y,
            (int)(vertex->z + 0.5f));
    }
}

static void ps2_make_signed_alpha_composite_vertices(
    const struct Ps2GfxSignedAlphaTriangles *source,
    int origin_x, int origin_y,
    struct Ps2GsTexturedVertex output[
        PS2_GFX_SIGNED_ALPHA_MAX_TRIANGLE_VERTICES])
{
    for (uint32_t i = 0u; i < source->vertex_count; ++i) {
        const struct Ps2GfxSignedAlphaVertex *vertex =
            &source->vertices[i];
        const struct Ps2GfxPassGraphSample sample =
            ps2GfxMapPassGraphSample(
                vertex->x, vertex->y, origin_x, origin_y);
        output[i].rgbaq = ps2_pack_rgbaq(
            0x80u, 0x80u, 0x80u, 0x80u, 1.0f);
        output[i].st = ps2_pack_st(sample.s, sample.t);
        output[i].xyz2 = ps2_pack_xyz2(
            vertex->x, vertex->y, (int)(vertex->z + 0.5f));
    }
}

static bool ps2_composite_signed_alpha_region(
    const struct Ps2GfxSignedAlphaTriangles *geometry,
    const struct Ps2GfxAlphaEdgeTest *test,
    const struct Ps2GsTexturedVertex *vertices)
{
    if (geometry->vertex_count == 0u ||
        test->comparison == PS2_GFX_ALPHA_EDGE_REJECT) {
        return true;
    }

    switch (test->comparison) {
        case PS2_GFX_ALPHA_EDGE_ALWAYS:
            ps2GsCoreSetAlphaTest(false, 0u);
            break;
        case PS2_GFX_ALPHA_EDGE_GEQUAL:
            ps2GsCoreSetAlphaTestComparison(
                true, test->reference, PS2_GS_ALPHA_TEST_GEQUAL);
            break;
        case PS2_GFX_ALPHA_EDGE_LEQUAL:
            ps2GsCoreSetAlphaTestComparison(
                true, test->reference, PS2_GS_ALPHA_TEST_LEQUAL);
            break;
        case PS2_GFX_ALPHA_EDGE_REJECT:
        default:
            return true;
    }
    return ps2GsCoreDrawRenderTargetTriangles(
        s_alpha_trilerp_color_target, vertices,
        geometry->vertex_count, false);
}

static bool ps2_draw_custom22_23_signed_alpha_tile(
    const struct Ps2AlphaTrilerpVertex *triangle,
    const struct Ps2GfxPassGraphRect *tile)
{
    struct Ps2GsTexturedVertex texture0[3];
    struct Ps2GsTexturedVertex texture1[3];
    ps2_make_independent_alpha_texture_triangle(
        triangle, 0, tile->x, tile->y, false, texture0);
    ps2_make_independent_alpha_texture_triangle(
        triangle, 1, tile->x, tile->y, true, texture1);

    struct Ps2GfxSignedAlphaVertex signed_triangle[3];
    struct Ps2GfxSignedAlphaTriangles positive = {};
    struct Ps2GfxSignedAlphaTriangles negative = {};
    ps2_make_signed_alpha_triangle(triangle, signed_triangle);
    if (!ps2GfxClipSignedAlphaTriangle(
            signed_triangle, true, &positive) ||
        !ps2GfxClipSignedAlphaTriangle(
            signed_triangle, false, &negative)) {
        return false;
    }

    struct Ps2GsTexturedVertex positive_alpha[
        PS2_GFX_SIGNED_ALPHA_MAX_TRIANGLE_VERTICES];
    struct Ps2GsTexturedVertex negative_alpha[
        PS2_GFX_SIGNED_ALPHA_MAX_TRIANGLE_VERTICES];
    struct Ps2GsTexturedVertex positive_composite[
        PS2_GFX_SIGNED_ALPHA_MAX_TRIANGLE_VERTICES];
    struct Ps2GsTexturedVertex negative_composite[
        PS2_GFX_SIGNED_ALPHA_MAX_TRIANGLE_VERTICES];
    ps2_make_signed_alpha_texture_vertices(
        &positive, tile->x, tile->y, positive_alpha);
    ps2_make_signed_alpha_texture_vertices(
        &negative, tile->x, tile->y, negative_alpha);
    ps2_make_signed_alpha_composite_vertices(
        &positive, tile->x, tile->y, positive_composite);
    ps2_make_signed_alpha_composite_vertices(
        &negative, tile->x, tile->y, negative_composite);

    const uint8_t primitive_alpha = triangle[0].primitive_alpha;
    const struct Ps2GfxAlphaEdgeTest positive_test =
        ps2GfxPlanSignedAlphaEdgeTest(
            PS2_GFX_TEXTURE_EDGE_THRESHOLD,
            primitive_alpha, true);
    const struct Ps2GfxAlphaEdgeTest negative_test =
        ps2GfxPlanSignedAlphaEdgeTest(
            PS2_GFX_TEXTURE_EDGE_THRESHOLD,
            primitive_alpha, false);

    /* Reconstruct RGB once. Alpha writes remain masked until signed capture. */
    if (!ps2GsCoreBindRenderTarget(s_alpha_trilerp_color_target)) {
        return false;
    }
    ps2GsCoreSetAlphaTest(false, 0u);
    ps2GsCoreSetDepthMode(false, false, false, false);
    ps2GsCoreSetFramebufferAlphaForce(false);
    ps2GsCoreSetTextureAlpha(false);
    ps2GsCoreSetAlphaWrite(false);
    ps2GsCoreSetColorWrite(true);
    ps2GsCoreSetAlphaBlend(false);
    ps2GsCoreSetFog(s_shader->features.opt_fog,
        s_draw_fog_r, s_draw_fog_g, s_draw_fog_b);
    ps2GsCoreClear(true, false);
    ps2_apply_texture_clamp(0);
    ps2GsCoreDrawTexturedTriangles(
        s_selected_texture[0], texture0, 3u);
    ps2GsCoreSetAlphaBlend(true);
    ps2_apply_texture_clamp(1);
    ps2GsCoreDrawTexturedTriangles(
        s_selected_texture[1], texture1, 3u);

    /* Store |SHADE.a-ENV.a|*TEXEL0.a only where a test consumes it. */
    ps2GsCoreSetFog(false, 0u, 0u, 0u);
    ps2GsCoreSetAlphaBlend(false);
    ps2GsCoreSetColorWrite(false);
    ps2GsCoreSetAlphaWrite(true);
    ps2GsCoreSetTextureAlpha(true);
    ps2_apply_texture_clamp(0);
    if (positive_test.comparison == PS2_GFX_ALPHA_EDGE_GEQUAL &&
        positive.vertex_count != 0u) {
        ps2GsCoreDrawTexturedTriangles(
            s_selected_texture[0], positive_alpha,
            positive.vertex_count);
    }
    if (negative_test.comparison == PS2_GFX_ALPHA_EDGE_LEQUAL &&
        negative.vertex_count != 0u) {
        ps2GsCoreDrawTexturedTriangles(
            s_selected_texture[0], negative_alpha,
            negative.vertex_count);
    }
    ps2GsCoreSetColorWrite(true);

    ps2GsCoreBindDefaultRenderTarget();
    ps2GsCoreSetScissor(tile->x, tile->y, tile->width, tile->height);
    ps2GsCoreSetDepthMode(s_depth_test, s_depth_update, s_depth_compare,
        s_depth_compare_equal);
    ps2GsCoreSetAlphaWrite(true);
    ps2GsCoreSetFog(false, 0u, 0u, 0u);
    ps2GsCoreSetTextureAlpha(true);
    ps2GsCoreSetFramebufferAlphaForce(true);
    ps2GsCoreSetAlphaBlend(false);
    return ps2_composite_signed_alpha_region(
               &positive, &positive_test, positive_composite) &&
           ps2_composite_signed_alpha_region(
               &negative, &negative_test, negative_composite);
}

static bool ps2_draw_trilerp_independent_alpha_tile(
    const struct Ps2AlphaTrilerpVertex *triangle,
    const struct Ps2GfxPassGraphRect *tile)
{
    struct Ps2GsTexturedVertex texture0[3];
    struct Ps2GsTexturedVertex texture1[3];
    struct Ps2GsTexturedVertex texture1_alpha[3];
    struct Ps2GsTexturedVertex composite[3];
    const bool tex1_alpha = ps2_independent_alpha_uses_tex1(
        &s_shader->plan);
    ps2_make_independent_alpha_texture_triangle(
        triangle, 0, tile->x, tile->y, false, texture0);
    ps2_make_independent_alpha_texture_triangle(
        triangle, 1, tile->x, tile->y, true, texture1);
    if (tex1_alpha) {
        ps2_make_independent_alpha_texture_triangle(
            triangle, 1, tile->x, tile->y, false, texture1_alpha);
    }
    ps2_make_alpha_trilerp_workspace_triangle(
        triangle, tile->x, tile->y, 0x80u, false, true, false,
        composite);

    if (!ps2GsCoreBindRenderTarget(s_alpha_trilerp_color_target)) {
        return false;
    }
    ps2GsCoreSetAlphaWrite(!tex1_alpha);
    ps2GsCoreSetAlphaTest(false, 0u);
    ps2GsCoreSetFog(s_shader->features.opt_fog,
        s_draw_fog_r, s_draw_fog_g, s_draw_fog_b);
    ps2GsCoreSetDepthMode(false, false, false, false);
    ps2GsCoreSetAlphaBlend(false);
    ps2GsCoreSetFramebufferAlphaForce(false);
    ps2GsCoreSetTextureAlpha(
        s_shader->plan.texture_alpha && !tex1_alpha);
    ps2GsCoreClear(true, false);
    ps2_apply_texture_clamp(0);
    ps2GsCoreDrawTexturedTriangles(
        s_selected_texture[0], texture0, 3u);

    /* RGB interpolates toward TEXEL1 while the final alpha stays intact. */
    ps2GsCoreSetAlphaWrite(false);
    ps2GsCoreSetAlphaBlend(true);
    ps2GsCoreSetTextureAlpha(false);
    ps2_apply_texture_clamp(1);
    ps2GsCoreDrawTexturedTriangles(
        s_selected_texture[1], texture1, 3u);

    if (tex1_alpha) {
        /* Preserve reconstructed RGB and replace only its alpha lane. */
        ps2GsCoreSetAlphaBlend(false);
        ps2GsCoreSetColorWrite(false);
        ps2GsCoreSetAlphaWrite(true);
        ps2GsCoreSetTextureAlpha(true);
        ps2_apply_texture_clamp(1);
        ps2GsCoreDrawTexturedTriangles(
            s_selected_texture[1], texture1_alpha, 3u);
        ps2GsCoreSetColorWrite(true);
    }

    ps2GsCoreBindDefaultRenderTarget();
    ps2GsCoreSetScissor(tile->x, tile->y, tile->width, tile->height);
    ps2GsCoreSetDepthMode(s_depth_test, s_depth_update, s_depth_compare,
        s_depth_compare_equal);
    ps2GsCoreSetAlphaWrite(true);
    const bool texture_edge = s_shader->features.opt_texture_edge;
    ps2GsCoreSetAlphaTest(
        texture_edge || s_shader->features.opt_alpha_threshold,
        texture_edge ? s_draw_texture_edge_reference :
            (s_shader->features.opt_alpha_threshold ?
                PS2_GFX_ALPHA_THRESHOLD : 0u));
    ps2GsCoreSetFog(false, 0u, 0u, 0u);
    ps2GsCoreSetTextureAlpha(true);
    ps2GsCoreSetFramebufferAlphaForce(texture_edge);
    ps2GsCoreSetAlphaBlend(texture_edge ? false : s_alpha_blend);
    return ps2GsCoreDrawRenderTargetTriangles(
        s_alpha_trilerp_color_target, composite, 3u, false);
}

static bool ps2_alpha_trilerp_sampler_states_match(void);
static bool ps2_alpha_trilerp_triangle_samples_match(
    const struct Ps2AlphaTrilerpVertex *triangle);

static bool ps2_independent_alpha_same_sample_batch(uint32_t vertex_count)
{
    if (!s_shader ||
        s_shader->plan.alpha_recipe != PS2_ALPHA_INPUT1 ||
        s_shader->features.opt_texture_edge ||
        !ps2_alpha_trilerp_sampler_states_match()) {
        return false;
    }

    for (uint32_t vertex = 0u; vertex < vertex_count; vertex += 3u) {
        if (!ps2_alpha_trilerp_triangle_samples_match(
                &s_alpha_trilerp_vertices[vertex])) {
            return false;
        }
    }
    return true;
}

static void ps2_draw_independent_alpha_same_sample(uint32_t vertex_count)
{
    /*
     * If both combiner texture inputs resolve to the exact same sample,
     * lerp(TEX0, TEX1, LOD) collapses algebraically to TEX0.  INPUT1 alpha is
     * independent of that RGB lerp, so preserve it directly in vertex alpha
     * and avoid the per-tile render-target reconstruction entirely.
     */
    for (uint32_t i = 0u; i < vertex_count; ++i) {
        const struct Ps2AlphaTrilerpVertex *vertex =
            &s_alpha_trilerp_vertices[i];
        struct Ps2GsTexturedVertex *out = &s_stq_vertices[0][i];
        out->rgbaq = ps2_pack_rgbaq(
            vertex->shade_r, vertex->shade_g, vertex->shade_b,
            vertex->independent_alpha, vertex->inv_w);
        out->st = ps2_pack_st(
            vertex->tex_u[0] * vertex->inv_w,
            vertex->tex_v[0] * vertex->inv_w);
        out->xyz2 = s_shader->features.opt_fog
            ? ps2_pack_xyzf2(
                vertex->x, vertex->y, vertex->z, vertex->fog)
            : ps2_pack_xyz2(vertex->x, vertex->y, vertex->z);
    }

    ps2GsCoreSetDepthMode(
        s_depth_test, s_depth_update, s_depth_compare,
        s_depth_compare_equal);
    ps2GsCoreSetColorWrite(true);
    ps2GsCoreSetAlphaWrite(true);
    ps2GsCoreSetAlphaTest(
        s_shader->features.opt_alpha_threshold,
        s_shader->features.opt_alpha_threshold
            ? PS2_GFX_ALPHA_THRESHOLD : 0u);
    ps2GsCoreSetFramebufferAlphaForce(false);
    ps2GsCoreSetAlphaBlend(s_alpha_blend);
    ps2GsCoreSetTextureAlpha(false);
    ps2GsCoreSetFog(
        s_shader->features.opt_fog,
        s_draw_fog_r, s_draw_fog_g, s_draw_fog_b);
    ps2_apply_texture_clamp(0);
    ps2GsCoreDrawTexturedTriangles(
        s_selected_texture[0], s_stq_vertices[0], vertex_count);

    ps2RendererTraceRecord(
        PS2_TRACE_INDEPENDENT_ALPHA_DRAW,
        (uint16_t)PS2_TRACE_FLAG_SUPPORTED,
        vertex_count, vertex_count / 3u, 3u, 0u);
}

static bool ps2_draw_trilerp_independent_alpha(uint32_t vertex_count)
{
    if (s_modulate) {
        if (!s_warned_independent_alpha_modulate) {
            sysLogPrintf(LOG_WARNING,
                "GfxPS2 independent-alpha trilerp rejects destination-color blend mode");
            s_warned_independent_alpha_modulate = true;
        }
        return false;
    }
#if defined(PERFECT_DARK_PS2_ALPHA_SAME_SAMPLE_FASTPATH)
    if (ps2_independent_alpha_same_sample_batch(vertex_count)) {
        ps2_draw_independent_alpha_same_sample(vertex_count);
        return true;
    }
#endif
    const bool custom24 = ps2_independent_alpha_is_custom24(
        &s_shader->plan);
    const bool custom22_23 = ps2_independent_alpha_is_custom22_23(
        &s_shader->plan);
    const bool workspace_ready = custom24
        ? ps2_ensure_alpha_trilerp_workspace()
        : ps2_ensure_alpha_trilerp_color_workspace();
    if (!workspace_ready) {
        if (!s_warned_independent_alpha_workspace) {
            sysLogPrintf(LOG_ERROR,
                "GfxPS2 independent-alpha workspace allocation failed (%dx%d CT32)",
                PS2_GFX_PASS_GRAPH_TILE_WIDTH,
                PS2_GFX_PASS_GRAPH_TILE_HEIGHT);
            s_warned_independent_alpha_workspace = true;
        }
        return false;
    }

    int clip_x0 = s_scissor.x > 0 ? s_scissor.x : 0;
    int clip_y0 = s_scissor.y > 0 ? s_scissor.y : 0;
    int clip_x1 = s_scissor.x + s_scissor.width;
    int clip_y1 = s_scissor.y + s_scissor.height;
    const int screen_width = ps2GsCoreGetWidth();
    const int screen_height = ps2GsCoreGetHeight();
    if (clip_x1 > screen_width) clip_x1 = screen_width;
    if (clip_y1 > screen_height) clip_y1 = screen_height;
    if (clip_x0 >= clip_x1 || clip_y0 >= clip_y1) {
        return true;
    }
    const struct Ps2GfxPassGraphRect clip = {
        clip_x0, clip_y0, clip_x1 - clip_x0, clip_y1 - clip_y0,
    };

    bool success = true;
    for (uint32_t vertex = 0u; vertex < vertex_count && success; vertex += 3u) {
        struct Ps2GfxPassGraphTriangle geometry = {};
        for (uint32_t i = 0u; i < 3u; ++i) {
            geometry.x[i] = s_alpha_trilerp_vertices[vertex + i].x;
            geometry.y[i] = s_alpha_trilerp_vertices[vertex + i].y;
        }
        struct Ps2GfxPassGraphTiles tiles = {};
        if (!ps2GfxDescribePassGraphTiles(&geometry, &clip, &tiles)) {
            continue;
        }

        const uint32_t tile_count = tiles.columns * tiles.rows;
        for (uint32_t tile_index = 0u;
             tile_index < tile_count && success; ++tile_index) {
            struct Ps2GfxPassGraphRect tile = {};
            if (!ps2GfxGetPassGraphTile(
                    &tiles, tile_index, &tile)) {
                success = false;
            } else if (custom24) {
                success = ps2_draw_custom24_nonlinear_alpha_tile(
                    &s_alpha_trilerp_vertices[vertex], &tile);
            } else if (custom22_23) {
                success = ps2_draw_custom22_23_signed_alpha_tile(
                    &s_alpha_trilerp_vertices[vertex], &tile);
            } else {
                success = ps2_draw_trilerp_independent_alpha_tile(
                    &s_alpha_trilerp_vertices[vertex], &tile);
            }
        }
    }

    ps2_restore_alpha_trilerp_state();
    if (!success && !s_warned_independent_alpha_workspace) {
        sysLogPrintf(LOG_ERROR,
            "GfxPS2 independent-alpha trilerp submission failed");
        s_warned_independent_alpha_workspace = true;
    }
    return success;
}

static bool ps2_alpha_trilerp_sampler_states_match(void)
{
    return s_selected_texture[0] == s_selected_texture[1] &&
        s_sampler_cms[0] == s_sampler_cms[1] &&
        s_sampler_cmt[0] == s_sampler_cmt[1] &&
        s_draw_region_clamp[0].region_s ==
            s_draw_region_clamp[1].region_s &&
        s_draw_region_clamp[0].region_t ==
            s_draw_region_clamp[1].region_t &&
        s_draw_region_clamp[0].max_u ==
            s_draw_region_clamp[1].max_u &&
        s_draw_region_clamp[0].max_v ==
            s_draw_region_clamp[1].max_v;
}

static bool ps2_alpha_trilerp_triangle_samples_match(
    const struct Ps2AlphaTrilerpVertex *triangle)
{
    if (!triangle || !ps2_alpha_trilerp_sampler_states_match()) {
        return false;
    }
    for (uint32_t i = 0u; i < 3u; ++i) {
        if (triangle[i].tex_u[0] != triangle[i].tex_u[1] ||
            triangle[i].tex_v[0] != triangle[i].tex_v[1]) {
            return false;
        }
    }
    return true;
}

static void ps2_make_alpha_trilerp_same_sample_vertices(
    const struct Ps2AlphaTrilerpVertex *source,
    uint32_t vertex_count, struct Ps2GsTexturedVertex *output)
{
    for (uint32_t i = 0u; i < vertex_count; ++i) {
        const struct Ps2AlphaTrilerpVertex *vertex = &source[i];
        output[i].rgbaq = ps2_pack_rgbaq(
            vertex->shade_r, vertex->shade_g, vertex->shade_b,
            vertex->direct_alpha, vertex->inv_w);
        output[i].st = ps2_pack_st(
            vertex->tex_u[0] * vertex->inv_w,
            vertex->tex_v[0] * vertex->inv_w);
        output[i].xyz2 = s_shader->features.opt_fog
            ? ps2_pack_xyzf2(
                vertex->x, vertex->y, vertex->z, vertex->fog)
            : ps2_pack_xyz2(vertex->x, vertex->y, vertex->z);
    }
}

static void ps2_set_alpha_trilerp_same_sample_state(void)
{
    ps2GsCoreSetDepthMode(s_depth_test, s_depth_update, s_depth_compare,
        s_depth_compare_equal);
    ps2GsCoreSetAlphaTest(
        s_shader->features.opt_alpha_threshold,
        s_shader->features.opt_alpha_threshold ?
            PS2_GFX_ALPHA_THRESHOLD : 0u);
    ps2GsCoreSetAlphaWrite(true);
    ps2GsCoreSetTextureAlpha(true);
    ps2GsCoreSetAlphaBlend(s_alpha_blend);
    ps2GsCoreSetFog(s_shader->features.opt_fog,
        s_draw_fog_r, s_draw_fog_g, s_draw_fog_b);
    ps2_apply_texture_clamp(0);
}

static void ps2_draw_alpha_trilerp_same_sample_triangle(
    const struct Ps2AlphaTrilerpVertex *triangle)
{
    struct Ps2GsTexturedVertex vertices[3];
    ps2_make_alpha_trilerp_same_sample_vertices(triangle, 3u, vertices);
    ps2_set_alpha_trilerp_same_sample_state();
    ps2GsCoreDrawTexturedTriangles(
        s_selected_texture[0], vertices, 3u);
}

static bool ps2_alpha_trilerp_all_same_sample(uint32_t vertex_count)
{
    if (!ps2_alpha_trilerp_sampler_states_match()) {
        return false;
    }
    for (uint32_t vertex = 0u; vertex < vertex_count; vertex += 3u) {
        const struct Ps2AlphaTrilerpVertex *triangle =
            &s_alpha_trilerp_vertices[vertex];
        if (triangle[0].alpha_add != 0u ||
            triangle[1].alpha_add != 0u ||
            triangle[2].alpha_add != 0u ||
            !ps2_alpha_trilerp_triangle_samples_match(triangle)) {
            return false;
        }
    }
    return true;
}

static void ps2_draw_alpha_trilerp_same_sample_batch(uint32_t vertex_count)
{
    ps2_make_alpha_trilerp_same_sample_vertices(
        s_alpha_trilerp_vertices, vertex_count, s_stq_vertices[0]);
    ps2_set_alpha_trilerp_same_sample_state();
    ps2GsCoreDrawTexturedTriangles(
        s_selected_texture[0], s_stq_vertices[0], vertex_count);
}

static void ps2_draw_alpha_trilerp_direct_opaque_triangle(uint32_t vertex)
{
    ps2_trilerp_set_base_state();
    ps2GsCoreDrawTexturedTriangles(
        s_selected_texture[0], &s_stq_vertices[0][vertex], 3u);
    ps2_trilerp_set_lerp_state();
    ps2GsCoreDrawTexturedTriangles(
        s_selected_texture[1], &s_stq_vertices[1][vertex], 3u);
    ps2_trilerp_restore_state();
}

static void ps2_make_alpha_trilerp_vertex_alpha_composite(
    const struct Ps2AlphaTrilerpVertex *source,
    int origin_x, int origin_y, struct Ps2GsTexturedVertex output[3])
{
    for (uint32_t i = 0u; i < 3u; ++i) {
        const struct Ps2AlphaTrilerpVertex *vertex = &source[i];
        const struct Ps2GfxPassGraphSample sample =
            ps2GfxMapPassGraphSample(
                vertex->x, vertex->y, origin_x, origin_y);
        output[i].rgbaq = ps2_pack_rgbaq(
            0x80u, 0x80u, 0x80u, vertex->final_alpha, 1.0f);
        output[i].st = ps2_pack_st(sample.s, sample.t);
        output[i].xyz2 = s_shader->features.opt_fog
            ? ps2_pack_xyzf2(
                vertex->x, vertex->y, vertex->z, vertex->fog)
            : ps2_pack_xyz2(vertex->x, vertex->y, vertex->z);
    }
}

static bool ps2_draw_alpha_trilerp_vertex_alpha_tile(
    const struct Ps2AlphaTrilerpVertex *triangle,
    const struct Ps2GfxPassGraphRect *tile)
{
    struct Ps2GsTexturedVertex texture0_color[3];
    struct Ps2GsTexturedVertex texture1_color[3];
    struct Ps2GsTexturedVertex composite[3];
    ps2_make_alpha_trilerp_texture_triangle(
        triangle, 0, tile->x, tile->y, false, false, texture0_color);
    ps2_make_alpha_trilerp_texture_triangle(
        triangle, 1, tile->x, tile->y, false, true, texture1_color);
    ps2_make_alpha_trilerp_vertex_alpha_composite(
        triangle, tile->x, tile->y, composite);

    ps2GsCoreSetAlphaWrite(true);
    ps2GsCoreSetAlphaTest(false, 0u);
    ps2GsCoreSetFog(false, 0u, 0u, 0u);
    ps2GsCoreSetDepthMode(false, false, false, false);
    ps2GsCoreSetAlphaBlend(false);
    ps2GsCoreSetTextureAlpha(false);

    if (!ps2GsCoreBindRenderTarget(s_alpha_trilerp_color_target)) {
        return false;
    }
    ps2GsCoreClear(true, false);
    ps2_apply_texture_clamp(0);
    ps2GsCoreDrawTexturedTriangles(
        s_selected_texture[0], texture0_color, 3u);
    ps2GsCoreSetAlphaWrite(false);
    ps2GsCoreSetAlphaBlend(true);
    ps2_apply_texture_clamp(1);
    ps2GsCoreDrawTexturedTriangles(
        s_selected_texture[1], texture1_color, 3u);
    ps2GsCoreSetAlphaWrite(true);
    ps2GsCoreSetAlphaBlend(false);

    ps2GsCoreBindDefaultRenderTarget();
    ps2GsCoreSetScissor(tile->x, tile->y, tile->width, tile->height);
    ps2GsCoreSetDepthMode(s_depth_test, s_depth_update, s_depth_compare,
        s_depth_compare_equal);
    ps2GsCoreSetAlphaTest(
        s_shader->features.opt_alpha_threshold,
        s_shader->features.opt_alpha_threshold ?
            PS2_GFX_ALPHA_THRESHOLD : 0u);
    ps2GsCoreSetTextureAlpha(false);
    ps2GsCoreSetAlphaBlend(s_alpha_blend);
    ps2GsCoreSetFog(s_shader->features.opt_fog,
        s_draw_fog_r, s_draw_fog_g, s_draw_fog_b);
    return ps2GsCoreDrawRenderTargetTriangles(
        s_alpha_trilerp_color_target, composite, 3u, false);
}

static bool ps2_draw_alpha_trilerp_tile(
    const struct Ps2AlphaTrilerpVertex *triangle,
    const struct Ps2GfxPassGraphRect *tile)
{
    struct Ps2GsTexturedVertex texture0_alpha[3];
    struct Ps2GsTexturedVertex texture1_alpha[3];
    struct Ps2GsTexturedVertex texture0_color[3];
    struct Ps2GsTexturedVertex texture1_color[3];
    struct Ps2GsTexturedVertex alpha_base[3];
    struct Ps2GsTexturedVertex alpha_lerp[3];
    struct Ps2GsColorVertex alpha_add[3];
    struct Ps2GsTexturedVertex composite[3];
    const bool add_input3 = s_shader->plan.alpha_recipe ==
        PS2_ALPHA_TEX01_LERP_INPUT1_MUL_INPUT2_PLUS_INPUT3;
    const bool add_input3_nonzero = add_input3 &&
        (triangle[0].alpha_add != 0u ||
         triangle[1].alpha_add != 0u ||
         triangle[2].alpha_add != 0u);
    ps2_make_alpha_trilerp_texture_triangle(
        triangle, 0, tile->x, tile->y, true, false, texture0_alpha);
    ps2_make_alpha_trilerp_texture_triangle(
        triangle, 1, tile->x, tile->y, true, false, texture1_alpha);
    ps2_make_alpha_trilerp_texture_triangle(
        triangle, 0, tile->x, tile->y, false, false, texture0_color);
    ps2_make_alpha_trilerp_texture_triangle(
        triangle, 1, tile->x, tile->y, false, true, texture1_color);
    ps2_make_alpha_trilerp_workspace_triangle(
        triangle, tile->x, tile->y, 0x80u, false, false, false,
        alpha_base);
    ps2_make_alpha_trilerp_workspace_triangle(
        triangle, tile->x, tile->y, 0u, true, false, false,
        alpha_lerp);
    if (add_input3_nonzero) {
        ps2_make_alpha_trilerp_add_triangle(
            triangle, tile->x, tile->y, alpha_add);
    }
    ps2_make_alpha_trilerp_workspace_triangle(
        triangle, tile->x, tile->y,
        add_input3 ? 0x40u : 0x80u, false, true, true,
        composite);

    ps2GsCoreSetAlphaWrite(true);
    ps2GsCoreSetAlphaTest(false, 0u);
    ps2GsCoreSetFog(false, 0u, 0u, 0u);
    ps2GsCoreSetDepthMode(false, false, false, false);
    ps2GsCoreSetAlphaBlend(false);
    ps2GsCoreSetTextureAlpha(true);

    if (!ps2GsCoreBindRenderTarget(s_alpha_trilerp_color_target)) {
        return false;
    }
    ps2GsCoreClear(true, false);
    ps2_apply_texture_clamp(0);
    ps2GsCoreDrawTexturedTriangles(
        s_selected_texture[0], texture0_alpha, 3u);

    if (!ps2GsCoreBindRenderTarget(s_alpha_trilerp_scalar_target)) {
        return false;
    }
    ps2GsCoreClear(true, false);
    ps2GsCoreSetTextureAlpha(false);
    if (!ps2GsCoreDrawRenderTargetAlphaTriangles(
            s_alpha_trilerp_color_target, alpha_base, 3u, false)) {
        return false;
    }

    if (!ps2GsCoreBindRenderTarget(s_alpha_trilerp_color_target)) {
        return false;
    }
    ps2GsCoreSetTextureAlpha(true);
    ps2_apply_texture_clamp(1);
    ps2GsCoreDrawTexturedTriangles(
        s_selected_texture[1], texture1_alpha, 3u);

    if (!ps2GsCoreBindRenderTarget(s_alpha_trilerp_scalar_target)) {
        return false;
    }
    ps2GsCoreSetTextureAlpha(false);
    ps2GsCoreSetAlphaBlend(true);
    if (!ps2GsCoreDrawRenderTargetAlphaTriangles(
            s_alpha_trilerp_color_target, alpha_lerp, 3u, false)) {
        return false;
    }
    if (add_input3_nonzero) {
        ps2GsCoreSetColorChannelWriteMask(PS2_GS_COLOR_WRITE_RED);
        ps2GsCoreSetAlphaWrite(false);
        ps2GsCoreSetAlphaBlendEquation(
            PS2_GS_ALPHA_BLEND_SOURCE_PLUS_DESTINATION);
        ps2GsCoreDrawColorTriangles(alpha_add, 3u);
        ps2GsCoreSetColorWrite(true);
        ps2GsCoreSetAlphaWrite(true);
    }

    if (!ps2GsCoreBindRenderTarget(s_alpha_trilerp_color_target)) {
        return false;
    }
    ps2GsCoreSetAlphaBlend(false);
    ps2GsCoreSetTextureAlpha(false);
    ps2_apply_texture_clamp(0);
    ps2GsCoreDrawTexturedTriangles(
        s_selected_texture[0], texture0_color, 3u);
    ps2GsCoreSetAlphaWrite(false);
    ps2GsCoreSetAlphaBlend(true);
    ps2_apply_texture_clamp(1);
    ps2GsCoreDrawTexturedTriangles(
        s_selected_texture[1], texture1_color, 3u);
    ps2GsCoreSetAlphaWrite(true);
    ps2GsCoreSetAlphaBlend(false);
#if defined(PERFECT_DARK_PS2_ALPHA_SPARSE_SHUFFLE)
    const struct Ps2GfxPassGraphTriangle shuffle_geometry = {
        { triangle[0].x, triangle[1].x, triangle[2].x },
        { triangle[0].y, triangle[1].y, triangle[2].y },
    };
    struct Ps2GfxPassGraphRowSpans shuffle_spans = {};
    if (!ps2GfxDescribePassGraphRowSpans(
            &shuffle_geometry, tile, &shuffle_spans) ||
        !ps2GsCoreBlitRenderTargetChannelSpansToActiveAlpha(
            s_alpha_trilerp_scalar_target, PS2_GS_CT32_CHANNEL_RED,
            (uint32_t)tile->width, (uint32_t)tile->height,
            shuffle_spans.x0, shuffle_spans.x1,
            shuffle_spans.row_count)) {
        return false;
    }
#else
    if (!ps2GsCoreBlitRenderTargetChannelRectToActiveAlpha(
            s_alpha_trilerp_scalar_target, PS2_GS_CT32_CHANNEL_RED,
            (uint32_t)tile->width, (uint32_t)tile->height)) {
        return false;
    }
#endif

    ps2GsCoreBindDefaultRenderTarget();
    ps2GsCoreSetScissor(tile->x, tile->y, tile->width, tile->height);
    ps2GsCoreSetDepthMode(s_depth_test, s_depth_update, s_depth_compare,
        s_depth_compare_equal);
    ps2GsCoreSetAlphaTest(
        s_shader->features.opt_alpha_threshold,
        s_shader->features.opt_alpha_threshold ?
            PS2_GFX_ALPHA_THRESHOLD : 0u);
    ps2GsCoreSetTextureAlpha(true);
    ps2GsCoreSetAlphaBlend(s_alpha_blend);
    ps2GsCoreSetFog(s_shader->features.opt_fog,
        s_draw_fog_r, s_draw_fog_g, s_draw_fog_b);
    return ps2GsCoreDrawRenderTargetTriangles(
        s_alpha_trilerp_color_target, composite, 3u, false);
}

static bool ps2_draw_alpha_trilerp(uint32_t vertex_count)
{
    static uint32_t tiled_triangles, submitted_tiles;
    static uint64_t report_time;
    if (s_modulate) {
        if (!s_warned_alpha_trilerp_modulate) {
            sysLogPrintf(LOG_WARNING,
                "GfxPS2 alpha-trilerp rejects destination-color blend mode");
            s_warned_alpha_trilerp_modulate = true;
        }
        return false;
    }
    const bool opaque_texture_pair =
        ps2GsCoreTextureAlphaIsOpaque(s_selected_texture[0]) &&
        ps2GsCoreTextureAlphaIsOpaque(s_selected_texture[1]);
    bool color_workspace_ready = false;
    bool full_workspace_ready = false;

    int clip_x0 = s_scissor.x > 0 ? s_scissor.x : 0;
    int clip_y0 = s_scissor.y > 0 ? s_scissor.y : 0;
    int clip_x1 = s_scissor.x + s_scissor.width;
    int clip_y1 = s_scissor.y + s_scissor.height;
    const int screen_width = ps2GsCoreGetWidth();
    const int screen_height = ps2GsCoreGetHeight();
    if (clip_x1 > screen_width) clip_x1 = screen_width;
    if (clip_y1 > screen_height) clip_y1 = screen_height;
    if (clip_x0 >= clip_x1 || clip_y0 >= clip_y1) {
        return true;
    }
    const struct Ps2GfxPassGraphRect clip = {
        clip_x0, clip_y0, clip_x1 - clip_x0, clip_y1 - clip_y0,
    };

    const bool trace_draw = ps2RendererTraceIsCapturing();
    float trace_min_x = 0.0f;
    float trace_min_y = 0.0f;
    float trace_max_x = 0.0f;
    float trace_max_y = 0.0f;
    uint8_t trace_lod_min = 0xffu;
    uint8_t trace_lod_max = 0u;
    uint8_t trace_shade_a_min = 0xffu;
    uint8_t trace_shade_a_max = 0u;
    uint8_t trace_alpha_add_min = 0xffu;
    uint8_t trace_alpha_add_max = 0u;
    if (trace_draw && vertex_count != 0u) {
        trace_min_x = trace_max_x = s_alpha_trilerp_vertices[0].x;
        trace_min_y = trace_max_y = s_alpha_trilerp_vertices[0].y;
        for (uint32_t i = 0u; i < vertex_count; ++i) {
            const struct Ps2AlphaTrilerpVertex *vertex =
                &s_alpha_trilerp_vertices[i];
            if (vertex->x < trace_min_x) trace_min_x = vertex->x;
            if (vertex->x > trace_max_x) trace_max_x = vertex->x;
            if (vertex->y < trace_min_y) trace_min_y = vertex->y;
            if (vertex->y > trace_max_y) trace_max_y = vertex->y;
            if (vertex->lod < trace_lod_min) trace_lod_min = vertex->lod;
            if (vertex->lod > trace_lod_max) trace_lod_max = vertex->lod;
            if (vertex->shade_a < trace_shade_a_min) {
                trace_shade_a_min = vertex->shade_a;
            }
            if (vertex->shade_a > trace_shade_a_max) {
                trace_shade_a_max = vertex->shade_a;
            }
            if (vertex->alpha_add < trace_alpha_add_min) {
                trace_alpha_add_min = vertex->alpha_add;
            }
            if (vertex->alpha_add > trace_alpha_add_max) {
                trace_alpha_add_max = vertex->alpha_add;
            }
        }
    }

    bool success = true;
    uint32_t draw_tiles = 0u;
    uint32_t direct_opaque_triangles = 0u;
    uint32_t vertex_alpha_triangles = 0u;
    uint32_t same_sample_triangles = 0u;

#if defined(PERFECT_DARK_PS2_ALPHA_SAME_SAMPLE_FASTPATH)
    const bool batch_same_sample =
        ps2_alpha_trilerp_all_same_sample(vertex_count);
    if (batch_same_sample) {
        ps2_draw_alpha_trilerp_same_sample_batch(vertex_count);
        same_sample_triangles = vertex_count / 3u;
    }
#else
    const bool batch_same_sample = false;
#endif

    for (uint32_t vertex = 0u;
         !batch_same_sample && vertex < vertex_count && success;
         vertex += 3u) {
        const struct Ps2AlphaTrilerpVertex *triangle =
            &s_alpha_trilerp_vertices[vertex];
        const bool zero_add =
            triangle[0].alpha_add == 0u &&
            triangle[1].alpha_add == 0u &&
            triangle[2].alpha_add == 0u;
        const bool opaque_alpha =
            triangle[0].final_alpha == 0x80u &&
            triangle[1].final_alpha == 0x80u &&
            triangle[2].final_alpha == 0x80u;

#if defined(PERFECT_DARK_PS2_ALPHA_SAME_SAMPLE_FASTPATH)
        if (zero_add &&
            ps2_alpha_trilerp_triangle_samples_match(triangle)) {
            ps2_draw_alpha_trilerp_same_sample_triangle(triangle);
            ++same_sample_triangles;
            continue;
        }
#endif

        if (opaque_texture_pair && zero_add && opaque_alpha) {
            ps2_draw_alpha_trilerp_direct_opaque_triangle(vertex);
            ++direct_opaque_triangles;
            continue;
        }

        struct Ps2GfxPassGraphTriangle geometry = {};
        for (uint32_t i = 0u; i < 3u; ++i) {
            geometry.x[i] = triangle[i].x;
            geometry.y[i] = triangle[i].y;
        }
        struct Ps2GfxPassGraphTiles tiles = {};
        if (!ps2GfxDescribePassGraphTiles(&geometry, &clip, &tiles)) {
            continue;
        }

        const bool vertex_alpha_fast =
            opaque_texture_pair && zero_add;
        if (vertex_alpha_fast && !color_workspace_ready) {
            color_workspace_ready =
                ps2_ensure_alpha_trilerp_color_workspace();
            if (!color_workspace_ready) {
                success = false;
                break;
            }
        } else if (!vertex_alpha_fast && !full_workspace_ready) {
            full_workspace_ready = ps2_ensure_alpha_trilerp_workspace();
            color_workspace_ready =
                color_workspace_ready || full_workspace_ready;
            if (!full_workspace_ready) {
                success = false;
                break;
            }
        }

        const uint32_t tile_count = tiles.columns * tiles.rows;
        ++tiled_triangles;
        submitted_tiles += tile_count;
        draw_tiles += tile_count;
        if (vertex_alpha_fast) {
            ++vertex_alpha_triangles;
        }
        for (uint32_t tile_index = 0u;
             tile_index < tile_count && success; ++tile_index) {
            struct Ps2GfxPassGraphRect tile = {};
            if (!ps2GfxGetPassGraphTile(
                    &tiles, tile_index, &tile)) {
                success = false;
            } else if (vertex_alpha_fast) {
                success = ps2_draw_alpha_trilerp_vertex_alpha_tile(
                    triangle, &tile);
            } else {
                success = ps2_draw_alpha_trilerp_tile(
                    triangle, &tile);
            }
        }
    }

    ps2_restore_alpha_trilerp_state();
    if (trace_draw) {
        const int32_t min_x_16 = (int32_t)(trace_min_x * 16.0f);
        const int32_t min_y_16 = (int32_t)(trace_min_y * 16.0f);
        const int32_t max_x_16 = (int32_t)(trace_max_x * 16.0f);
        const int32_t max_y_16 = (int32_t)(trace_max_y * 16.0f);
        const uint64_t bbox_min =
            (uint32_t)min_x_16 | ((uint64_t)(uint32_t)min_y_16 << 32u);
        const uint64_t bbox_max =
            (uint32_t)max_x_16 | ((uint64_t)(uint32_t)max_y_16 << 32u);
        const bool additive_alpha = s_shader->plan.alpha_recipe ==
            PS2_ALPHA_TEX01_LERP_INPUT1_MUL_INPUT2_PLUS_INPUT3;
        const uint64_t alpha_ranges =
            (uint64_t)trace_lod_min |
            ((uint64_t)trace_lod_max << 8u) |
            ((uint64_t)trace_shade_a_min << 16u) |
            ((uint64_t)trace_shade_a_max << 24u) |
            ((uint64_t)trace_alpha_add_min << 32u) |
            ((uint64_t)trace_alpha_add_max << 40u) |
            ((uint64_t)(success ? 1u : 0u) << 48u) |
            ((uint64_t)(additive_alpha ? 1u : 0u) << 49u) |
            ((uint64_t)(direct_opaque_triangles & 0x7fu) << 50u) |
            ((uint64_t)(vertex_alpha_triangles & 0x7fu) << 57u);
        const uint16_t trace_flags =
            (uint16_t)PS2_TRACE_FLAG_TEXTURED |
            (uint16_t)PS2_TRACE_FLAG_SUPPORTED |
            (success ? 0u : (uint16_t)PS2_TRACE_FLAG_DROPPED);
        ps2RendererTraceRecord(PS2_TRACE_PASS_GRAPH_DRAW, trace_flags,
            vertex_count, draw_tiles, bbox_min, bbox_max);
        ps2RendererTraceRecord(PS2_TRACE_PASS_GRAPH_DRAW,
            (uint16_t)(trace_flags | 0x0100u),
            s_shader->shader_id0, s_shader->shader_id1,
            alpha_ranges, same_sample_triangles);
    }

    const uint64_t now = sysGetMicroseconds();
    if (now - report_time >= 5000000ULL) {
        sysLogPrintf(LOG_NOTE,
            "GfxPS2 trilerp cumulative: tiled=%u tiles=%u",
            tiled_triangles, submitted_tiles);
        report_time = now;
    }
    if (!success && !s_warned_alpha_trilerp_workspace) {
        sysLogPrintf(LOG_ERROR,
            "GfxPS2 alpha-trilerp pass graph submission failed");
        s_warned_alpha_trilerp_workspace = true;
    }
    return success;
}

static uint64_t ps2_trace_pack_float_pair(float a, float b);
static uint64_t ps2_trace_pack_u32_pair(uint32_t a, uint32_t b);

static void ps2_draw_triangles_unclipped(float buf_vbo[],
    size_t buf_vbo_len, size_t buf_vbo_num_tris)
{
    if (!ps2GsCoreIsReady() || !s_shader || !buf_vbo || buf_vbo_num_tris == 0) {
        return;
    }

    if (!s_shader->plan.supported) {
        ps2RendererStatsRecordUnsupportedShader(
            (uint32_t)buf_vbo_num_tris);
        if (!s_shader->warned_rejected_draw) {
            sysLogPrintf(LOG_WARNING,
                "GfxPS2 dropping unsupported shader id=%016llx/%08x first_batch_tris=%u",
                (unsigned long long)s_shader->shader_id0,
                (unsigned int)s_shader->shader_id1,
                (unsigned int)buf_vbo_num_tris);
            s_shader->warned_rejected_draw = true;
            if (!s_checkpointed_unsupported_shader &&
                !s_pending_unsupported_shader_checkpoint) {
                s_pending_unsupported_shader_checkpoint = true;
            }
        }
#if !defined(PERFECT_DARK_PS2_GEOMETRY_BASELINE) && \
    !defined(PERFECT_DARK_PS2_MATERIAL_BASELINE)
        return;
#endif
    }

    const size_t stride = ps2_vbo_stride(s_shader);
    const size_t vertex_count = buf_vbo_num_tris * 3;
    if (stride == 0 || buf_vbo_len < vertex_count * stride) {
        return;
    }

    const bool alpha_trilerp = s_shader->plan.pass_graph ==
        PS2_PASS_GRAPH_ALPHA_TRILERP_MODULATE;
    const bool independent_alpha_trilerp =
        ps2_is_trilerp_independent_alpha(&s_shader->plan);
    const bool independent_tex0_alpha =
        ps2_is_independent_tex0_alpha(&s_shader->plan);
    const bool opaque_trilerp = s_shader->plan.pass_graph ==
        PS2_PASS_GRAPH_OPAQUE_TRILERP &&
        ps2_color_recipe_is_opaque_trilerp(s_shader->plan.color_recipe);
    const bool opaque_input1_tex0_lerp = s_shader->plan.pass_graph ==
        PS2_PASS_GRAPH_OPAQUE_INPUT1_TEX0_LERP &&
        ps2_color_recipe_is_opaque_input1_tex0_lerp(
            s_shader->plan.color_recipe);
    const bool tex0_factor_lerp = s_shader->plan.pass_graph ==
            PS2_PASS_GRAPH_TEX0_FACTOR_LERP &&
        s_shader->plan.color_recipe ==
            PS2_COLOR_INPUT2_INPUT1_LERP_TEX0;
    const bool tex1_alpha_factor_lerp = s_shader->plan.pass_graph ==
            PS2_PASS_GRAPH_TEX1_ALPHA_FACTOR_LERP &&
        s_shader->plan.color_recipe ==
            PS2_COLOR_INPUT2_INPUT1_LERP_TEX1_ALPHA;
    const bool texture_factor_lerp = tex0_factor_lerp ||
        tex1_alpha_factor_lerp;
    const bool alpha_trilerp_opaque_textures = alpha_trilerp &&
        ps2GsCoreTextureAlphaIsOpaque(s_selected_texture[0]) &&
        ps2GsCoreTextureAlphaIsOpaque(s_selected_texture[1]);
    const bool interference = s_shader->plan.pass_graph ==
        PS2_PASS_GRAPH_INTERFERENCE &&
        s_shader->plan.color_recipe ==
            PS2_COLOR_TEX0_MUL_TEX1_MUL_INPUT1;
#if defined(PERFECT_DARK_PS2_MATERIAL_BASELINE)
    (void)alpha_trilerp;
    (void)independent_alpha_trilerp;
    (void)independent_tex0_alpha;
    (void)opaque_trilerp;
    (void)opaque_input1_tex0_lerp;
    (void)texture_factor_lerp;
    (void)interference;
#endif
#if defined(PERFECT_DARK_PS2_MATERIAL_BASELINE)
    if (s_shader->features.used_textures[0] &&
        !ps2GsCoreTextureReady(s_selected_texture[0])) {
        return;
    }
#elif !defined(PERFECT_DARK_PS2_GEOMETRY_BASELINE)
    if (s_shader->plan.textured &&
        (!ps2GsCoreTextureReady(s_selected_texture[0]) ||
         ((opaque_trilerp || alpha_trilerp ||
           independent_alpha_trilerp || interference ||
           tex1_alpha_factor_lerp) &&
          !ps2GsCoreTextureReady(s_selected_texture[1])))) {
        return;
    }
#endif

#if defined(PERFECT_DARK_PS2_OPAQUE_DIAGNOSTIC)
    if (gfxPs2OpaqueDiagnosticSkips(
            s_alpha_blend,
            s_shader->features.opt_texture_edge,
            s_shader->features.opt_invisible)) {
        return;
    }
#endif

    bool fog_color_emitted = false;
#if defined(PERFECT_DARK_PS2_MATERIAL_BASELINE) && \
    !defined(PERFECT_DARK_PS2_OPAQUE_DIAGNOSTIC)
    ps2GsCoreSetAlphaTest(
        s_shader->features.opt_alpha_threshold,
        s_shader->features.opt_alpha_threshold
            ? PS2_GFX_ALPHA_THRESHOLD : 0u);
    ps2GsCoreSetAlphaBlend(s_alpha_blend);
    ps2GsCoreSetFramebufferAlphaForce(false);
    ps2GsCoreSetColorWrite(true);
    ps2GsCoreSetAlphaWrite(true);
#endif
#if defined(PERFECT_DARK_PS2_GEOMETRY_BASELINE) || \
    (defined(PERFECT_DARK_PS2_MATERIAL_BASELINE) && \
     defined(PERFECT_DARK_PS2_OPAQUE_DIAGNOSTIC))
    const bool texture_edge = false;
    const bool invisible = false;
#else
    const bool texture_edge = s_shader->features.opt_texture_edge;
    const bool invisible = s_shader->features.opt_invisible;
#endif
    if (invisible) {
        ps2GsCoreSetColorWrite(false);
        ps2GsCoreSetAlphaWrite(false);
    }
    if (texture_edge) {
        /*
         * Fast3D's portable contract discards alpha <= 0.19 and promotes every
         * accepted fragment to opaque. Disable source-alpha blending and use
         * FBA to store the native 0x80 opaque bit after the GS alpha test.
         */
        ps2GsCoreSetAlphaTest(true, PS2_GFX_TEXTURE_EDGE_THRESHOLD);
        ps2GsCoreSetFramebufferAlphaForce(true);
        ps2GsCoreSetAlphaBlend(false);
    }
    float texture_coordinate_scale_s[2] = { 1.0f, 1.0f };
    float texture_coordinate_scale_t[2] = { 1.0f, 1.0f };
    for (int t = 0; t < 2; ++t) {
        if (!s_shader->features.used_textures[t]) {
            continue;
        }
        const Ps2GsTextureHandle handle = s_selected_texture[t];
        if (handle >= PS2_GFX_TEXTURE_STATE_SLOTS) {
            continue;
        }
        const struct Ps2TextureSamplerState *sampler =
            &s_texture_sampler[handle];
        texture_coordinate_scale_s[t] = sampler->coordinate_scale_s;
        texture_coordinate_scale_t[t] = sampler->coordinate_scale_t;
    }
    size_t base_vertex = 0;
    while (base_vertex < vertex_count) {
        size_t batch_vertices = vertex_count - base_vertex;
        if (batch_vertices > PS2_GFX_TRANSLATE_VERTS) {
            batch_vertices = PS2_GFX_TRANSLATE_VERTS;
            batch_vertices -= batch_vertices % 3;
        }

        memset(s_draw_region_clamp, 0, sizeof(s_draw_region_clamp));
        s_draw_texture_edge_reference = PS2_GFX_TEXTURE_EDGE_THRESHOLD;
#if defined(PERFECT_DARK_PS2_VU1_COLOR_BATCH)
        float transform_scale[4];
        float transform_offset[4];
        /* ZMODE_DEC needs a saturating +2 reversed-Z bias.  The shared VU1
         * affine mapping cannot clamp that bias without changing every draw,
         * so decals deliberately use the exact EE/PATH3 mapper below. */
        bool vu1_transform_eligible = !s_depth_decal &&
            ps2GsVu1BuildViewportMapping(
            s_viewport.x, s_viewport.y,
            s_viewport.width, s_viewport.height,
            ps2GsCoreGetOffsetX(), ps2GsCoreGetOffsetY(),
            s_depth_near, s_depth_far,
            transform_scale, transform_offset);
#endif

        const bool trace_texture_coords = ps2RendererTraceIsCapturing();
        float trace_min_u[2] = { FLT_MAX, FLT_MAX };
        float trace_min_v[2] = { FLT_MAX, FLT_MAX };
        float trace_max_u[2] = { -FLT_MAX, -FLT_MAX };
        float trace_max_v[2] = { -FLT_MAX, -FLT_MAX };

        const uint64_t translation_start = sysGetMicroseconds();
        for (size_t i = 0; i < batch_vertices; ++i) {
            const float *src = &buf_vbo[(base_vertex + i) * stride];
            size_t pos = 0;
            const float clip_x = src[pos++];
            const float clip_y = src[pos++];
            const float clip_z = src[pos++];
            const float clip_w = src[pos++];
            const float inv_w = clip_w != 0.0f ? 1.0f / clip_w : 0.0f;
            const float ndc_x = clip_x * inv_w;
            const float ndc_y = clip_y * inv_w;

            float tex_u[2] = { 0.0f, 0.0f };
            float tex_v[2] = { 0.0f, 0.0f };
            for (int t = 0; t < 2; ++t) {
                if (!s_shader->features.used_textures[t]) {
                    continue;
                }
                const Ps2GsTextureHandle handle = s_selected_texture[t];
                tex_u[t] = src[pos++] * texture_coordinate_scale_s[t];
                tex_v[t] = src[pos++] * texture_coordinate_scale_t[t];
                if (trace_texture_coords) {
                    if (tex_u[t] < trace_min_u[t]) trace_min_u[t] = tex_u[t];
                    if (tex_u[t] > trace_max_u[t]) trace_max_u[t] = tex_u[t];
                    if (tex_v[t] < trace_min_v[t]) trace_min_v[t] = tex_v[t];
                    if (tex_v[t] > trace_max_v[t]) trace_max_v[t] = tex_v[t];
                }
                if (s_shader->features.clamp[t][0]) {
                    const float bound = src[pos++];
                    if (i == 0u) {
                        s_draw_region_clamp[t].region_s =
                            ps2_decode_texture_region_clamp(
                                handle, true, bound,
                                &s_draw_region_clamp[t].max_u);
                    }
                }
                if (s_shader->features.clamp[t][1]) {
                    const float bound = src[pos++];
                    if (i == 0u) {
                        s_draw_region_clamp[t].region_t =
                            ps2_decode_texture_region_clamp(
                                handle, false, bound,
                                &s_draw_region_clamp[t].max_v);
                    }
                }
            }

            float fog_r = 0.0f;
            float fog_g = 0.0f;
            float fog_b = 0.0f;
            float fog_factor = 0.0f;
            if (s_shader->features.opt_fog) {
                fog_r = src[pos++];
                fog_g = src[pos++];
                fog_b = src[pos++];
                fog_factor = src[pos++];

                if (!fog_color_emitted) {
                    s_draw_fog_r = ps2_u8_component(fog_r);
                    s_draw_fog_g = ps2_u8_component(fog_g);
                    s_draw_fog_b = ps2_u8_component(fog_b);
                    ps2GsCoreSetFog(true,
                        s_draw_fog_r, s_draw_fog_g, s_draw_fog_b);
                    fog_color_emitted = true;
                }
            }

            if (s_shader->features.opt_grayscale) pos += 4;

            float input[3][4] = {
                { 1.0f, 1.0f, 1.0f, 1.0f },
                { 1.0f, 1.0f, 1.0f, 1.0f },
                { 1.0f, 1.0f, 1.0f, 1.0f },
            };
            int parsed_inputs = s_shader->features.num_inputs;
            if (parsed_inputs > 3) {
                parsed_inputs = 3;
            }
            for (int input_index = 0; input_index < parsed_inputs; ++input_index) {
                input[input_index][0] = src[pos++];
                input[input_index][1] = src[pos++];
                input[input_index][2] = src[pos++];
                if (s_shader->features.opt_alpha) {
                    input[input_index][3] = src[pos++];
                }
            }

            const float sx = (float)s_viewport.x + (ndc_x * 0.5f + 0.5f) * (float)s_viewport.width;
            const float sy = (float)s_viewport.y + (0.5f - ndc_y * 0.5f) * (float)s_viewport.height;
            const int iz = ps2_map_clip_depth(clip_z, clip_w);

            uint8_t cr = 0;
            uint8_t cg = 0;
            uint8_t cb = 0;
            switch (s_shader->plan.color_recipe) {
                case PS2_COLOR_INPUT1:
                    cr = ps2_u8_component(input[0][0]);
                    cg = ps2_u8_component(input[0][1]);
                    cb = ps2_u8_component(input[0][2]);
                    break;
                case PS2_COLOR_TEX0:
                    cr = cg = cb = 0x80;
                    break;
                case PS2_COLOR_TEX0_MUL_INPUT1:
                    cr = ps2_modulate_component(input[0][0]);
                    cg = ps2_modulate_component(input[0][1]);
                    cb = ps2_modulate_component(input[0][2]);
                    break;
                default:
                    break;
            }

            uint8_t ca = 0x80;
            switch (s_shader->plan.alpha_recipe) {
                case PS2_ALPHA_ZERO:
                    ca = 0x00;
                    break;
                case PS2_ALPHA_INPUT1:
                    ca = ps2_modulate_component(input[0][3]);
                    break;
                case PS2_ALPHA_TEX0:
                    ca = 0x40;
                    break;
                case PS2_ALPHA_TEX0_MUL_INPUT1:
                    ca = ps2_texture_alpha_fragment_component(input[0][3]);
                    break;
                case PS2_ALPHA_OPAQUE:
                case PS2_ALPHA_ONE:
                default:
                    ca = 0x80;
                    break;
            }
#if defined(PERFECT_DARK_PS2_MATERIAL_BASELINE)
            (void)fog_factor;
            (void)cr;
            (void)cg;
            (void)cb;
#if defined(PERFECT_DARK_PS2_OPAQUE_DIAGNOSTIC)
            (void)ca;
#else
            ca = ps2_material_baseline_fragment_alpha(s_shader, input);
#endif
#endif

            struct Ps2GsPackedReg packed_position;
#if defined(PERFECT_DARK_PS2_GEOMETRY_BASELINE) || \
    defined(PERFECT_DARK_PS2_MATERIAL_BASELINE)
            packed_position = ps2_pack_xyz2(sx, sy, iz);
#if defined(PERFECT_DARK_PS2_GEOMETRY_BASELINE)
            const size_t palette_index =
                ((base_vertex + i) / 3u) %
                (sizeof(s_geometry_baseline_palette) /
                 sizeof(s_geometry_baseline_palette[0]));
            s_color_vertices[i].rgbaq = ps2_pack_rgbaq(
                s_geometry_baseline_palette[palette_index][0],
                s_geometry_baseline_palette[palette_index][1],
                s_geometry_baseline_palette[palette_index][2],
                0x80u, 0.0f);
            s_color_vertices[i].xyz2 = packed_position;
#else
            if (s_shader->features.used_textures[0]) {
                s_stq_vertices[0][i].rgbaq = ps2_pack_rgbaq(
                    0x80u, 0x80u, 0x80u,
#if defined(PERFECT_DARK_PS2_OPAQUE_DIAGNOSTIC)
                    0x80u,
#else
                    ca,
#endif
                    inv_w);
                s_stq_vertices[0][i].st = ps2_pack_st(
                    tex_u[0] * inv_w, tex_v[0] * inv_w);
                s_stq_vertices[0][i].xyz2 = packed_position;
            } else {
                const size_t palette_index =
                    ((base_vertex + i) / 3u) %
                    (sizeof(s_geometry_baseline_palette) /
                     sizeof(s_geometry_baseline_palette[0]));
                s_color_vertices[i].rgbaq = ps2_pack_rgbaq(
                    s_geometry_baseline_palette[palette_index][0],
                    s_geometry_baseline_palette[palette_index][1],
                    s_geometry_baseline_palette[palette_index][2],
#if defined(PERFECT_DARK_PS2_OPAQUE_DIAGNOSTIC)
                    0x80u,
#else
                    ca,
#endif
                    0.0f);
                s_color_vertices[i].xyz2 = packed_position;
            }
#endif
#else
            if (s_shader->features.opt_fog) {
                packed_position = ps2_pack_xyzf2(
                    sx, sy, iz, ps2_fog_coefficient(fog_factor));
            } else {
                packed_position = ps2_pack_xyz2(sx, sy, iz);
            }
#endif

#if !defined(PERFECT_DARK_PS2_MATERIAL_BASELINE)
            if (independent_tex0_alpha) {
                struct Ps2IndependentTex0AlphaVertex *vertex =
                    &s_independent_tex0_alpha_vertices[i];
                vertex->x = sx;
                vertex->y = sy;
                vertex->inv_w = inv_w;
                vertex->tex_u = tex_u[0];
                vertex->tex_v = tex_v[0];
                vertex->z = iz;
                vertex->input[0] = ps2_u8_component(input[0][0]);
                vertex->input[1] = ps2_u8_component(input[0][1]);
                vertex->input[2] = ps2_u8_component(input[0][2]);
                vertex->input[3] = s_shader->plan.alpha_recipe ==
                        PS2_ALPHA_TEX0_MUL_INPUT1
                    ? ps2_texture_alpha_fragment_component(input[0][3])
                    : 0x40u;
                vertex->fog = ps2_fog_coefficient(fog_factor);
            } else if (interference) {
                struct Ps2InterferenceVertex *vertex =
                    &s_interference_vertices[i];
                vertex->x = sx;
                vertex->y = sy;
                vertex->inv_w = inv_w;
                vertex->tex_u[0] = tex_u[0];
                vertex->tex_u[1] = tex_u[1];
                vertex->tex_v[0] = tex_v[0];
                vertex->tex_v[1] = tex_v[1];
                vertex->z = iz;
                for (uint32_t channel = 0u; channel < 4u; ++channel) {
                    vertex->shade[channel] =
                        ps2_modulate_component(input[0][channel]);
                }
                vertex->fog = ps2_fog_coefficient(fog_factor);
            } else if (texture_factor_lerp) {
                struct Ps2Tex0FactorLerpVertex *vertex =
                    &s_tex0_factor_vertices[i];
                vertex->x = sx;
                vertex->y = sy;
                vertex->inv_w = inv_w;
                vertex->tex_u[0] = tex_u[0];
                vertex->tex_u[1] = tex_u[1];
                vertex->tex_v[0] = tex_v[0];
                vertex->tex_v[1] = tex_v[1];
                vertex->z = iz;
                for (uint32_t channel = 0u; channel < 3u; ++channel) {
                    vertex->input1[channel] =
                        ps2_u8_component(input[0][channel]);
                    vertex->input2[channel] =
                        ps2_u8_component(input[1][channel]);
                }
                vertex->input1[3] =
                    s_shader->plan.alpha_recipe ==
                            PS2_ALPHA_INPUT2_INPUT1_COVERAGE_LERP_TEX0
                        ? ps2_modulate_component(
                            gfxPs2CoverageUnion(
                                input[0][3], input[1][3]))
                        : ps2_modulate_component(input[0][3]);
                vertex->input2[3] =
                    ps2_modulate_component(input[1][3]);
                vertex->tex0_alpha_input =
                    ps2_texture_alpha_fragment_component(input[0][3]);
                vertex->fog = ps2_fog_coefficient(fog_factor);
            } else if (alpha_trilerp || independent_alpha_trilerp) {
                struct Ps2AlphaTrilerpVertex *vertex =
                    &s_alpha_trilerp_vertices[i];
                vertex->x = sx;
                vertex->y = sy;
                vertex->inv_w = inv_w;
                vertex->tex_u[0] = tex_u[0];
                vertex->tex_u[1] = tex_u[1];
                vertex->tex_v[0] = tex_v[0];
                vertex->tex_v[1] = tex_v[1];
                vertex->z = iz;
                vertex->shade_r = ps2_modulate_component(input[1][0]);
                vertex->shade_g = ps2_modulate_component(input[1][1]);
                vertex->shade_b = ps2_modulate_component(input[1][2]);
                vertex->lod = ps2_modulate_component(input[0][0]);
                vertex->primitive_alpha = 0u;
                vertex->alpha_add = 0u;
                vertex->final_alpha = 0x80u;
                vertex->direct_alpha =
                    ps2_texture_alpha_fragment_component(input[1][3]);
                vertex->signed_alpha_delta = 0.0f;
                if (ps2_independent_alpha_is_custom22_23(
                        &s_shader->plan)) {
                    vertex->shade_a = 0u;
                    vertex->independent_alpha = 0u;
                    vertex->primitive_alpha =
                        ps2_modulate_component(input[2][3]);
                    vertex->signed_alpha_delta =
                        input[0][3] - input[1][3];
                } else if (ps2_independent_alpha_is_custom24(
                        &s_shader->plan)) {
                    vertex->shade_a =
                        ps2_modulate_component(input[0][3]);
                    vertex->independent_alpha =
                        ps2_modulate_component(
                            input[0][3] * input[1][3]);
                } else if (s_shader->plan.alpha_recipe ==
                        PS2_ALPHA_TEX01_LERP_INPUT1_MUL_INPUT2_PLUS_INPUT3) {
                    /*
                     * Keep the scalar workspace in N64's 0..255 alpha domain.
                     * The final 0x40 composite factor converts it back to the
                     * GS 0..128 blend-factor neighbourhood.
                     */
                    vertex->shade_a =
                        ps2_modulate_component(input[1][3]);
                    vertex->independent_alpha = 0u;
                    vertex->final_alpha =
                        ps2_modulate_component(input[1][3]);
                    vertex->alpha_add =
                        ps2_u8_component(input[2][3]);
                } else if (s_shader->plan.alpha_recipe ==
                        PS2_ALPHA_INPUT1) {
                    vertex->shade_a = 0x80u;
                    vertex->independent_alpha =
                        ps2_modulate_component(input[0][3]);
                } else if (s_shader->plan.alpha_recipe ==
                        PS2_ALPHA_TEX1_MUL_INPUT1) {
                    vertex->shade_a = 0x80u;
                    vertex->independent_alpha =
                        ps2_texture_alpha_fragment_component(
                            input[0][3]);
                } else if (s_shader->plan.alpha_recipe ==
                        PS2_ALPHA_INPUT1_PLUS_INPUT2_EDGE) {
                    vertex->shade_a =
                        ps2_texture_alpha_fragment_component(
                            input[1][3]);
                    vertex->independent_alpha =
                        ps2_modulate_component(input[0][3]);
                    if (i == 0u) {
                        const uint8_t environment =
                            ps2_modulate_component(input[1][3]);
                        s_draw_texture_edge_reference =
                            gfxPs2TextureEdgeAdjustedReference(
                                PS2_GFX_TEXTURE_EDGE_THRESHOLD,
                                environment);
                    }
                } else {
                    vertex->shade_a =
                        ps2_texture_alpha_fragment_component(
                            input[1][3]);
                    vertex->independent_alpha =
                        s_shader->plan.alpha_recipe ==
                            PS2_ALPHA_TEX0_MUL_INPUT1_MUL_INPUT2
                        ? ps2_texture_alpha_fragment_component(
                            input[0][3] * input[1][3])
                        : ps2_modulate_component(
                            input[0][3] * input[1][3]);
                    if (alpha_trilerp) {
                        vertex->final_alpha =
                            ps2_modulate_component(input[1][3]);
                    }
                }
                vertex->fog = ps2_fog_coefficient(fog_factor);

                if (alpha_trilerp_opaque_textures) {
                    s_stq_vertices[0][i].rgbaq = ps2_pack_rgbaq(
                        vertex->shade_r, vertex->shade_g, vertex->shade_b,
                        0x80u, inv_w);
                    s_stq_vertices[0][i].st = ps2_pack_st(
                        tex_u[0] * inv_w, tex_v[0] * inv_w);
                    s_stq_vertices[0][i].xyz2 = packed_position;

                    s_stq_vertices[1][i].rgbaq = ps2_pack_rgbaq(
                        vertex->shade_r, vertex->shade_g, vertex->shade_b,
                        vertex->lod, inv_w);
                    s_stq_vertices[1][i].st = ps2_pack_st(
                        tex_u[1] * inv_w, tex_v[1] * inv_w);
                    s_stq_vertices[1][i].xyz2 = packed_position;
                }
            } else if (opaque_trilerp) {
                uint8_t shade_r = 0x80;
                uint8_t shade_g = 0x80;
                uint8_t shade_b = 0x80;
                if (s_shader->plan.color_recipe ==
                        PS2_COLOR_TEX01_LERP_INPUT1_MUL_INPUT2) {
                    shade_r = ps2_modulate_component(input[1][0]);
                    shade_g = ps2_modulate_component(input[1][1]);
                    shade_b = ps2_modulate_component(input[1][2]);
                }

                const uint8_t lod = ps2_modulate_component(input[0][0]);
                s_stq_vertices[0][i].rgbaq =
                    ps2_pack_rgbaq(shade_r, shade_g, shade_b, 0x80, inv_w);
                s_stq_vertices[0][i].st =
                    ps2_pack_st(tex_u[0] * inv_w, tex_v[0] * inv_w);
                s_stq_vertices[0][i].xyz2 = packed_position;

                s_stq_vertices[1][i].rgbaq =
                    ps2_pack_rgbaq(shade_r, shade_g, shade_b, lod, inv_w);
                s_stq_vertices[1][i].st =
                    ps2_pack_st(tex_u[1] * inv_w, tex_v[1] * inv_w);
                s_stq_vertices[1][i].xyz2 = packed_position;
            } else if (opaque_input1_tex0_lerp) {
                const bool modulate = s_shader->plan.color_recipe ==
                    PS2_COLOR_INPUT1_TEX0_LERP_INPUT2_MUL_INPUT3;
                const float shade_r = modulate ? input[2][0] : 1.0f;
                const float shade_g = modulate ? input[2][1] : 1.0f;
                const float shade_b = modulate ? input[2][2] : 1.0f;
                const uint8_t lerp = ps2_modulate_component(input[1][0]);

                s_color_vertices[i].rgbaq = ps2_pack_rgbaq(
                    ps2_u8_component(input[0][0] * shade_r),
                    ps2_u8_component(input[0][1] * shade_g),
                    ps2_u8_component(input[0][2] * shade_b),
                    0x80, 0.0f);
                s_color_vertices[i].xyz2 = packed_position;

                s_stq_vertices[0][i].rgbaq = ps2_pack_rgbaq(
                    ps2_modulate_component(shade_r),
                    ps2_modulate_component(shade_g),
                    ps2_modulate_component(shade_b),
                    lerp, inv_w);
                s_stq_vertices[0][i].st =
                    ps2_pack_st(tex_u[0] * inv_w, tex_v[0] * inv_w);
                s_stq_vertices[0][i].xyz2 = packed_position;
            } else if (s_shader->plan.textured) {
                s_stq_vertices[0][i].rgbaq =
                    ps2_pack_rgbaq(cr, cg, cb, ca, inv_w);
                s_stq_vertices[0][i].st =
                    ps2_pack_st(tex_u[0] * inv_w, tex_v[0] * inv_w);
                s_stq_vertices[0][i].xyz2 = packed_position;
#if defined(PERFECT_DARK_PS2_VU1_COLOR_BATCH)
                struct Ps2GsVu1TransformVertex *vu_vertex =
                    &s_vu1_transform_vertices[i];
                vu_vertex->clip[0] = clip_x;
                vu_vertex->clip[1] = clip_y;
                vu_vertex->clip[2] = clip_z;
                vu_vertex->clip[3] = clip_w;
                vu_vertex->texcoord.s = tex_u[0];
                vu_vertex->texcoord.t = tex_v[0];
                vu_vertex->texcoord.q = 1.0f;
                vu_vertex->texcoord.xyz_control =
                    s_shader->features.opt_fog
                    ? (uint32_t)ps2_fog_coefficient(fog_factor) << 4
                    : 0u;
                vu_vertex->rgba[0] = cr;
                vu_vertex->rgba[1] = cg;
                vu_vertex->rgba[2] = cb;
                vu_vertex->rgba[3] = ca;
                /* DIV on VU treats subnormal W as zero; keep that input on EE. */
                if (!(clip_w >= FLT_MIN && clip_w <= FLT_MAX)) {
                    vu1_transform_eligible = false;
                }
#endif
            } else {
                s_color_vertices[i].rgbaq = ps2_pack_rgbaq(cr, cg, cb, ca, 0.0f);
                s_color_vertices[i].xyz2 = packed_position;
            }
#endif
        }
        ps2RendererStatsRecordTranslation(
            (uint32_t)batch_vertices,
            sysGetMicroseconds() - translation_start);

        if (trace_texture_coords) {
            for (uint32_t t = 0u; t < 2u; ++t) {
                if (!s_shader->features.used_textures[t] ||
                    trace_min_u[t] == FLT_MAX) {
                    continue;
                }
                const Ps2GsTextureHandle handle = s_selected_texture[t];
                const struct Ps2TextureSamplerState *sampler =
                    handle < PS2_GFX_TEXTURE_STATE_SLOTS
                    ? &s_texture_sampler[handle] : NULL;
                uint16_t trace_flags =
                    (uint16_t)PS2_TRACE_FLAG_TEXTURED |
                    (s_sampler_linear[t] ? 0x0100u : 0u) |
                    (s_draw_region_clamp[t].region_s ? 0x0200u : 0u) |
                    (s_draw_region_clamp[t].region_t ? 0x0400u : 0u) |
                    (uint16_t)(((uint32_t)s_filter_mode & 0x3u) << 12u);
                ps2RendererTraceRecord(PS2_TRACE_TEXTURE_COORD_RANGE,
                    trace_flags,
                    ps2_trace_pack_u32_pair(t, (uint32_t)handle),
                    ps2_trace_pack_float_pair(
                        trace_min_u[t], trace_max_u[t]),
                    ps2_trace_pack_float_pair(
                        trace_min_v[t], trace_max_v[t]),
                    sampler ? ps2_trace_pack_u32_pair(
                        sampler->logical_width, sampler->logical_height) : 0u);
            }
        }

#if defined(PERFECT_DARK_PS2_GEOMETRY_BASELINE)
        ps2GsCoreSetFog(false, 0u, 0u, 0u);
        ps2GsCoreSetAlphaTest(false, 0u);
        ps2GsCoreSetAlphaBlend(false);
        ps2GsCoreSetFramebufferAlphaForce(false);
        ps2GsCoreSetColorWrite(true);
        ps2GsCoreSetAlphaWrite(true);
        ps2GsCoreDrawColorTriangles(
            s_color_vertices, (uint32_t)batch_vertices);
#elif defined(PERFECT_DARK_PS2_MATERIAL_BASELINE)
        ps2GsCoreSetFog(false, 0u, 0u, 0u);
#if defined(PERFECT_DARK_PS2_OPAQUE_DIAGNOSTIC)
        ps2GsCoreSetAlphaTest(false, 0u);
        ps2GsCoreSetAlphaBlend(false);
        ps2GsCoreSetFramebufferAlphaForce(false);
        ps2GsCoreSetColorWrite(true);
        ps2GsCoreSetAlphaWrite(true);
#endif
        if (s_shader->features.used_textures[0]) {
            ps2GsCoreSetTextureAlpha(
#if defined(PERFECT_DARK_PS2_OPAQUE_DIAGNOSTIC)
                false
#else
                ps2_material_baseline_texture_alpha(s_shader)
#endif
            );
            ps2_apply_texture_clamp(0);
            ps2GsCoreDrawTexturedTriangles(
                s_selected_texture[0], s_stq_vertices[0],
                (uint32_t)batch_vertices);
        } else {
            ps2GsCoreSetTextureAlpha(false);
            ps2GsCoreDrawColorTriangles(
                s_color_vertices, (uint32_t)batch_vertices);
        }
#else
        if (independent_tex0_alpha) {
            (void)ps2_draw_independent_tex0_alpha(
                (uint32_t)batch_vertices);
        } else if (interference) {
            (void)ps2_draw_interference(
                (uint32_t)batch_vertices);
        } else if (texture_factor_lerp) {
            (void)ps2_draw_tex0_factor_lerp(
                (uint32_t)batch_vertices);
        } else if (alpha_trilerp) {
            (void)ps2_draw_alpha_trilerp((uint32_t)batch_vertices);
        } else if (independent_alpha_trilerp) {
            (void)ps2_draw_trilerp_independent_alpha(
                (uint32_t)batch_vertices);
        } else if (opaque_trilerp) {
            ps2_draw_opaque_trilerp((uint32_t)batch_vertices);
        } else if (opaque_input1_tex0_lerp) {
            ps2_draw_opaque_input1_tex0_lerp((uint32_t)batch_vertices);
        } else if (s_shader->plan.textured) {
            ps2_apply_texture_clamp(0);
#if defined(PERFECT_DARK_PS2_VU1_COLOR_BATCH)
            if (vu1_transform_eligible) {
                ps2GsCoreDrawTexturedTrianglesTransform(
                    s_selected_texture[0], s_stq_vertices[0],
                    s_vu1_transform_vertices,
                    (uint32_t)batch_vertices,
                    transform_scale, transform_offset);
            } else {
                ps2GsCoreDrawTexturedTriangles(
                    s_selected_texture[0], s_stq_vertices[0],
                    (uint32_t)batch_vertices);
            }
#else
            ps2GsCoreDrawTexturedTriangles(
                s_selected_texture[0], s_stq_vertices[0],
                (uint32_t)batch_vertices);
#endif
        } else {
            ps2GsCoreDrawColorTriangles(s_color_vertices, (uint32_t)batch_vertices);
        }
#endif

        base_vertex += batch_vertices;
    }

    if (texture_edge) {
        ps2GsCoreSetFramebufferAlphaForce(false);
        ps2GsCoreSetAlphaBlend(s_alpha_blend);
        ps2GsCoreSetAlphaTest(
            s_shader->features.opt_alpha_threshold,
            s_shader->features.opt_alpha_threshold ?
                PS2_GFX_ALPHA_THRESHOLD : 0u);
    }
    if (invisible) {
        ps2GsCoreSetColorWrite(true);
        ps2GsCoreSetAlphaWrite(true);
    }
}

static uint64_t ps2_trace_pack_float_pair(float a, float b)
{
    uint32_t lo = 0u;
    uint32_t hi = 0u;
    memcpy(&lo, &a, sizeof(lo));
    memcpy(&hi, &b, sizeof(hi));
    return (uint64_t)lo | ((uint64_t)hi << 32u);
}

static uint64_t ps2_trace_pack_u32_pair(uint32_t a, uint32_t b)
{
    return (uint64_t)a | ((uint64_t)b << 32u);
}

static uint64_t ps2_trace_pack_screen_pair(float x, float y)
{
    const int32_t fx = (int32_t)(x * 16.0f);
    const int32_t fy = (int32_t)(y * 16.0f);
    return (uint32_t)fx | ((uint64_t)(uint32_t)fy << 32u);
}

static void ps2_trace_clipped_triangle_bounds(
    size_t source_triangle, size_t clipped_vertices,
    const float *vertices, size_t stride)
{
    if (!ps2RendererTraceIsCapturing() || !vertices ||
        stride < 4u || clipped_vertices < 3u) {
        return;
    }

    for (size_t out = 0u; out + 2u < clipped_vertices; out += 3u) {
        float sx[3];
        float sy[3];
        bool near_zero_w = false;
        for (uint32_t i = 0u; i < 3u; ++i) {
            const float *v = &vertices[(out + i) * stride];
            const float w = v[3];
            near_zero_w = near_zero_w ||
                (w > -1.0e-6f && w < 1.0e-6f);
            const float inv_w = w != 0.0f ? 1.0f / w : 0.0f;
            const float ndc_x = v[0] * inv_w;
            const float ndc_y = v[1] * inv_w;
            sx[i] = (float)s_viewport.x +
                (ndc_x * 0.5f + 0.5f) * (float)s_viewport.width;
            sy[i] = (float)s_viewport.y +
                (0.5f - ndc_y * 0.5f) * (float)s_viewport.height;
        }

        float min_x = sx[0], max_x = sx[0];
        float min_y = sy[0], max_y = sy[0];
        for (uint32_t i = 1u; i < 3u; ++i) {
            if (sx[i] < min_x) min_x = sx[i];
            if (sx[i] > max_x) max_x = sx[i];
            if (sy[i] < min_y) min_y = sy[i];
            if (sy[i] > max_y) max_y = sy[i];
        }

        const float width = max_x - min_x;
        const float height = max_y - min_y;
        const float area2 =
            (sx[1] - sx[0]) * (sy[2] - sy[0]) -
            (sy[1] - sy[0]) * (sx[2] - sx[0]);
        const float bbox_area = width * height;
        uint16_t flags = s_shader->plan.textured
            ? (uint16_t)PS2_TRACE_FLAG_TEXTURED : 0u;
        if (width >= (float)s_viewport.width * 0.8f) flags |= 0x0100u;
        if (height >= (float)s_viewport.height * 0.8f) flags |= 0x0200u;
        if (bbox_area > 1.0f &&
            (area2 < 0.0f ? -area2 : area2) <= bbox_area * 0.05f) {
            flags |= 0x0400u;
        }
        if (near_zero_w) flags |= 0x0800u;

        const uint16_t diagnostic_flags =
            flags & (0x0100u | 0x0200u | 0x0400u | 0x0800u);
        if (diagnostic_flags != 0u) {
            ps2RendererTraceRecord(
                PS2_TRACE_CLIPPED_TRIANGLE_BOUNDS, flags,
                source_triangle, out / 3u,
                ps2_trace_pack_screen_pair(min_x, min_y),
                ps2_trace_pack_screen_pair(max_x, max_y));
        }
    }
}

static void ps2_trace_draw_state(uint32_t draw_id)
{
    if (!ps2RendererTraceIsCapturing() || !s_shader || draw_id == 0u) {
        return;
    }

    const uint64_t textures =
        (uint32_t)s_selected_texture[0] |
        ((uint64_t)(uint32_t)s_selected_texture[1] << 32u);
    const uint64_t modes =
        ((uint64_t)(uint32_t)s_filter_mode) |
        ((uint64_t)(uint32_t)s_mipmap_filter << 8u) |
        ((uint64_t)(uint32_t)s_anisotropy << 16u) |
        ((uint64_t)(uint32_t)s_active_texture_tile << 32u);
    uint64_t draw_flags = 0u;
    draw_flags |= s_depth_test ? UINT64_C(1) << 0u : 0u;
    draw_flags |= s_depth_update ? UINT64_C(1) << 1u : 0u;
    draw_flags |= s_depth_compare ? UINT64_C(1) << 2u : 0u;
    draw_flags |= s_depth_compare_equal ? UINT64_C(1) << 3u : 0u;
    draw_flags |= s_depth_decal ? UINT64_C(1) << 4u : 0u;
    draw_flags |= s_alpha_blend ? UINT64_C(1) << 5u : 0u;
    draw_flags |= s_modulate ? UINT64_C(1) << 6u : 0u;
    draw_flags |= s_shader->features.opt_alpha_threshold ?
        UINT64_C(1) << 7u : 0u;
    draw_flags |= s_shader->features.opt_texture_edge ?
        UINT64_C(1) << 8u : 0u;
    draw_flags |= s_shader->features.opt_fog ?
        UINT64_C(1) << 9u : 0u;
    draw_flags |= s_shader->features.opt_invisible ?
        UINT64_C(1) << 10u : 0u;
    draw_flags |= s_shader->features.opt_2cyc ?
        UINT64_C(1) << 11u : 0u;
    draw_flags |= (uint64_t)(uint8_t)s_draw_fog_r << 16u;
    draw_flags |= (uint64_t)(uint8_t)s_draw_fog_g << 24u;
    draw_flags |= (uint64_t)(uint8_t)s_draw_fog_b << 32u;

    ps2RendererTraceRecord(PS2_TRACE_DRAW_STATE, 0u,
        draw_id, textures, modes, draw_flags);

    ps2RendererTraceRecord(PS2_TRACE_DRAW_STATE, 1u,
        draw_id,
        ps2_trace_pack_u32_pair(
            (uint32_t)s_viewport.x, (uint32_t)s_viewport.y),
        ps2_trace_pack_u32_pair(
            (uint32_t)s_viewport.width, (uint32_t)s_viewport.height),
        ps2_trace_pack_float_pair(s_depth_near, s_depth_far));
    ps2RendererTraceRecord(PS2_TRACE_DRAW_STATE, 2u,
        draw_id,
        ps2_trace_pack_u32_pair(
            (uint32_t)s_scissor.x, (uint32_t)s_scissor.y),
        ps2_trace_pack_u32_pair(
            (uint32_t)s_scissor.width, (uint32_t)s_scissor.height),
        0u);

    for (uint32_t t = 0u; t < 2u; ++t) {
        const Ps2GsTextureHandle handle = s_selected_texture[t];
        const struct Ps2TextureSamplerState *sampler =
            handle < PS2_GFX_TEXTURE_STATE_SLOTS
            ? &s_texture_sampler[handle] : NULL;
        const struct Ps2TextureRegionClampState *region =
            &s_draw_region_clamp[t];
        const uint64_t sampler_flags =
            (uint64_t)(s_sampler_cms[t] & 0xffu) |
            ((uint64_t)(s_sampler_cmt[t] & 0xffu) << 8u) |
            ((uint64_t)(s_sampler_linear[t] ? 1u : 0u) << 16u) |
            ((uint64_t)(region->region_s ? 1u : 0u) << 17u) |
            ((uint64_t)(region->region_t ? 1u : 0u) << 18u) |
            ((uint64_t)region->max_u << 24u) |
            ((uint64_t)region->max_v << 40u) |
            ((uint64_t)(sampler && sampler->expanded_mirror_s ? 1u : 0u)
                << 56u) |
            ((uint64_t)(sampler && sampler->expanded_mirror_t ? 1u : 0u)
                << 57u) |
            ((uint64_t)(sampler && sampler->monochrome_rgb ? 1u : 0u)
                << 58u);
        ps2RendererTraceRecord(PS2_TRACE_DRAW_STATE,
            (uint16_t)(3u + t), draw_id,
            ps2_trace_pack_u32_pair(
                (uint32_t)handle,
                sampler ? sampler->logical_width : 0u),
            ps2_trace_pack_u32_pair(
                sampler ? sampler->logical_height : 0u,
                (uint32_t)t),
            sampler_flags);
        if (sampler) {
            ps2RendererTraceRecord(PS2_TRACE_DRAW_STATE,
                (uint16_t)(5u + t), draw_id,
                ps2_trace_pack_float_pair(
                    sampler->coordinate_scale_s,
                    sampler->coordinate_scale_t),
                0u, 0u);
            const uint64_t source_meta =
                (uint64_t)sampler->source_format |
                ((uint64_t)sampler->source_size << 8u) |
                ((uint64_t)(sampler->palette_format & 0xffffu) << 16u) |
                ((uint64_t)(sampler->palette_count & 0xffffu) << 32u);
            const uint64_t handle_serial =
                (uint64_t)(uint32_t)handle |
                ((uint64_t)sampler->upload_serial << 32u);
            ps2RendererTraceRecord(PS2_TRACE_DRAW_STATE,
                (uint16_t)(7u + t), draw_id,
                handle_serial, source_meta,
                sampler->source_hash);
            ps2RendererTraceRecord(PS2_TRACE_DRAW_STATE,
                (uint16_t)(9u + t), draw_id,
                handle_serial, sampler->palette_hash,
                0u);
            ps2RendererTraceRecord(PS2_TRACE_DRAW_STATE,
                (uint16_t)(11u + t), draw_id,
                handle_serial, sampler->content_identity,
                0u);
        }
    }
}

static void ps2_trace_draw_payload(uint32_t draw_id, uint32_t chunk,
    uint16_t kind, const float *vertices, uint32_t vertex_count,
    uint32_t stride)
{
    if (!ps2RendererTraceIsCapturing() || !vertices ||
        vertex_count == 0u || stride == 0u || draw_id == 0u) {
        return;
    }

    const uint64_t bytes64 =
        (uint64_t)vertex_count * (uint64_t)stride * sizeof(float);
    if (bytes64 == 0u || bytes64 > UINT32_MAX) {
        return;
    }
    const uint32_t bytes = (uint32_t)bytes64;
    uint32_t offset = 0u;
    const bool stored = ps2RendererTraceAppendBlob(
        vertices, bytes, 16u, &offset);
    const uint16_t flags = kind |
        (stored ? 0u : (uint16_t)PS2_TRACE_FLAG_DROPPED);
    ps2RendererTraceRecord(PS2_TRACE_DRAW_PAYLOAD, flags,
        (uint64_t)draw_id | ((uint64_t)chunk << 32u),
        (uint64_t)offset | ((uint64_t)bytes << 32u),
        (uint64_t)vertex_count | ((uint64_t)stride << 32u),
        ps2_trace_hash(vertices, bytes));
}

static void ps2_draw_triangles(float buf_vbo[], size_t buf_vbo_len,
    size_t buf_vbo_num_tris)
{
    if (!ps2GsCoreIsReady() || !s_shader || !buf_vbo ||
        buf_vbo_num_tris == 0u) {
        return;
    }

    const size_t stride = ps2_vbo_stride(s_shader);
    const size_t source_vertices = buf_vbo_num_tris * 3u;
    const uint32_t trace_draw_id = ps2RendererTraceIsCapturing()
        ? ++s_trace_draw_id : 0u;
    ps2RendererTraceRecord(PS2_TRACE_DRAW_INPUT,
        s_shader->plan.textured ?
            (uint16_t)PS2_TRACE_FLAG_TEXTURED : 0u,
        buf_vbo_num_tris, source_vertices, stride, buf_vbo_len);
    if (stride < 4u || stride > PS2_GS_CLIP_MAX_VERTEX_FLOATS ||
        buf_vbo_len < source_vertices * stride) {
        return;
    }

    /*
     * Clamp bounds are decoded during translation below.  Clearing the prior
     * batch here prevents draw_state from presenting stale region-clamp data;
     * texture_coord_range carries the authoritative decoded bounds.
     */
    memset(s_draw_region_clamp, 0, sizeof(s_draw_region_clamp));
    ps2_trace_draw_state(trace_draw_id);
    ps2_trace_draw_payload(trace_draw_id, 0u, 1u,
        buf_vbo, (uint32_t)source_vertices, (uint32_t)stride);

    uint16_t *trace_clip_map = NULL;
    uint32_t trace_clip_map_offset = 0u;
    uint32_t trace_clip_map_bytes = 0u;
    if (trace_draw_id != 0u && buf_vbo_num_tris <= UINT32_MAX / 2u) {
        trace_clip_map_bytes = (uint32_t)buf_vbo_num_tris *
            (uint32_t)sizeof(uint16_t);
        trace_clip_map = (uint16_t *)ps2RendererTraceReserveBlob(
            trace_clip_map_bytes, 2u, &trace_clip_map_offset);
        if (trace_clip_map) {
            memset(trace_clip_map, 0, trace_clip_map_bytes);
        }
    }

    size_t buffered_vertices = 0u;
    size_t clipped_vertices_total = 0u;
    uint32_t trace_clip_chunk = 0u;
    for (size_t triangle = 0u; triangle < buf_vbo_num_tris; ++triangle) {
        if (PS2_GFX_TRANSLATE_VERTS - buffered_vertices <
            PS2_GS_CLIP_MAX_OUTPUT_VERTICES) {
            ps2_trace_draw_payload(
                trace_draw_id, ++trace_clip_chunk, 2u,
                s_clipped_vbo, (uint32_t)buffered_vertices,
                (uint32_t)stride);
            ps2_draw_triangles_unclipped(s_clipped_vbo,
                buffered_vertices * stride, buffered_vertices / 3u);
            buffered_vertices = 0u;
        }

        size_t clipped_vertices = 0u;
        const bool clipped = ps2GsClipTriangle(
            &buf_vbo[triangle * 3u * stride], stride,
            &s_clipped_vbo[buffered_vertices * stride],
            PS2_GFX_TRANSLATE_VERTS - buffered_vertices,
            &clipped_vertices);
        if (trace_clip_map) {
            trace_clip_map[triangle] = clipped
                ? (uint16_t)clipped_vertices : 0u;
        }
        if (!clipped) {
            continue;
        }
        ps2_trace_clipped_triangle_bounds(
            triangle, clipped_vertices,
            &s_clipped_vbo[buffered_vertices * stride], stride);
        buffered_vertices += clipped_vertices;
        clipped_vertices_total += clipped_vertices;
    }

    if (buffered_vertices != 0u) {
        ps2_trace_draw_payload(
            trace_draw_id, ++trace_clip_chunk, 2u,
            s_clipped_vbo, (uint32_t)buffered_vertices,
            (uint32_t)stride);
        ps2_draw_triangles_unclipped(s_clipped_vbo,
            buffered_vertices * stride, buffered_vertices / 3u);
    }
    if (trace_draw_id != 0u) {
        const bool stored = trace_clip_map != NULL;
        const uint16_t flags = 3u |
            (stored ? 0u : (uint16_t)PS2_TRACE_FLAG_DROPPED);
        ps2RendererTraceRecord(PS2_TRACE_DRAW_PAYLOAD, flags,
            trace_draw_id,
            (uint64_t)trace_clip_map_offset |
                ((uint64_t)trace_clip_map_bytes << 32u),
            (uint64_t)(uint32_t)buf_vbo_num_tris |
                ((uint64_t)sizeof(uint16_t) << 32u),
            stored ? ps2_trace_hash(
                trace_clip_map, trace_clip_map_bytes) : 0u);
    }
    ps2RendererTraceRecord(PS2_TRACE_DRAW_CLIPPED,
        s_shader->plan.textured ?
            (uint16_t)PS2_TRACE_FLAG_TEXTURED : 0u,
        buf_vbo_num_tris, source_vertices, clipped_vertices_total, 0u);
}

static void ps2_reset_viewport(void)
{
    if (!ps2GsCoreIsReady()) {
        s_viewport.x = 0;
        s_viewport.y = 0;
        s_viewport.width = 0;
        s_viewport.height = 0;
        s_scissor = s_viewport;
        return;
    }

    s_viewport.x = 0;
    s_viewport.y = 0;
    s_viewport.width = ps2GsCoreGetWidth();
    s_viewport.height = ps2GsCoreGetHeight();
    s_scissor = s_viewport;
}

static void ps2_init(void)
{
    ps2RendererStatsReset();
    ps2_clear_shaders();
    s_selected_texture[0] = PS2_GS_TEXTURE_INVALID;
    s_selected_texture[1] = PS2_GS_TEXTURE_INVALID;
    s_active_texture_tile = 0;
    s_depth_near = 0.0f;
    s_depth_far = 1.0f;
    /* RenderingState is zero-initialised by Fast3D.  Its first depth-disabled
     * draw therefore does not call set_depth_mode(), so the backend's initial
     * state must already be depth-test/write disabled. */
    s_depth_test = false;
    s_depth_update = false;
    s_depth_compare = false;
    s_depth_compare_equal = false;
    s_depth_decal = false;
    s_alpha_blend = false;
    s_modulate = false;
    s_sampler_cms[0] = s_sampler_cms[1] = 0;
    s_sampler_cmt[0] = s_sampler_cmt[1] = 0;
    memset(s_texture_sampler, 0, sizeof(s_texture_sampler));
    s_filter_mode = FILTER_LINEAR;
    s_mipmap_filter = MIPMAP_DISABLED;
    s_anisotropy = 1;
    s_draw_fog_r = 0u;
    s_draw_fog_g = 0u;
    s_draw_fog_b = 0u;
    s_logged_native_rgba16 = false;
    s_logged_native_ia16 = false;
    s_logged_native_mirror = false;
    s_checkpointed_unsupported_shader = false;
    s_pending_unsupported_shader_checkpoint = false;
    s_upload_mirror_s = false;
    s_upload_mirror_t = false;
    ps2GsCoreSetAlphaTest(false, 0u);
    ps2GsCoreSetAlphaBlend(false);
    ps2GsCoreSetFramebufferAlphaForce(false);
    ps2GsCoreSetColorWrite(true);
    ps2GsCoreSetAlphaWrite(true);
    ps2GsCoreSetFog(false, 0u, 0u, 0u);
    ps2GsCoreSetTextureAlpha(false);
    ps2GsCoreSetDepthMode(false, false, false, false);
    ps2_reset_viewport();

    sysLogPrintf(LOG_NOTE,
        "GfxPS2 init: shaders=%d translate_batch=%d indexed=%s geometry=%s",
        PS2_GFX_MAX_SHADERS, PS2_GFX_TRANSLATE_VERTS,
#if defined(PERFECT_DARK_PS2_NATIVE_INDEXED_TEXTURES)
        "native",
#else
        "rgba32-compat",
#endif
#if defined(PERFECT_DARK_PS2_VU1_COLOR_BATCH)
        "vu1-path1");
#else
        "ee-path3");
#endif
}

static void ps2_on_resize(void)
{
    ps2_reset_viewport();
}

static void ps2_trace_build_config(void)
{
    if (!ps2RendererTraceIsCapturing()) {
        return;
    }

    uint64_t build_flags = 0u;
#if defined(PERFECT_DARK_PS2_NATIVE_INDEXED_TEXTURES)
    build_flags |= UINT64_C(1) << 0u;
#endif
#if defined(PERFECT_DARK_PS2_VU1_COLOR_BATCH)
    build_flags |= UINT64_C(1) << 1u;
#endif
#if defined(PERFECT_DARK_PS2_ALPHA_SPARSE_SHUFFLE)
    build_flags |= UINT64_C(1) << 2u;
#endif
#if defined(PERFECT_DARK_PS2_ALPHA_SAME_SAMPLE_FASTPATH)
    build_flags |= UINT64_C(1) << 3u;
#endif
#if defined(PERFECT_DARK_PS2_INDEPENDENT_ALPHA_MASK)
    build_flags |= UINT64_C(1) << 4u;
#endif
#if defined(PERFECT_DARK_PS2_INDEPENDENT_ALPHA_DIRECT)
    build_flags |= UINT64_C(1) << 5u;
#endif
#if defined(PERFECT_DARK_PS2_GEOMETRY_BASELINE)
    build_flags |= UINT64_C(1) << 6u;
#endif
#if defined(PERFECT_DARK_PS2_MATERIAL_BASELINE)
    build_flags |= UINT64_C(1) << 7u;
#endif

    ps2RendererTraceRecord(PS2_TRACE_BUILD_CONFIG, 0u,
        build_flags,
        (uint64_t)PS2_GFX_TRANSLATE_VERTS |
            ((uint64_t)PS2_GFX_TEXTURE_STATE_SLOTS << 32u),
        (uint64_t)PS2_GFX_ALPHA_THRESHOLD |
            ((uint64_t)PS2_GFX_TEXTURE_EDGE_THRESHOLD << 32u),
        (uint64_t)(uint32_t)s_filter_mode |
            ((uint64_t)(uint32_t)s_mipmap_filter << 16u) |
            ((uint64_t)(uint32_t)s_anisotropy << 32u));
}

static void ps2_start_frame(void)
{
    ps2RendererTraceBeginFrame();
    if (ps2RendererTraceIsCapturing()) {
        ps2RendererStatsPerfSkipCurrentFrame();
    }
    memset(s_trace_tmem_snapshot_identity, 0,
        sizeof(s_trace_tmem_snapshot_identity));
    if (ps2RendererTraceIsCapturing()) {
        s_trace_draw_id = 0u;
        /*
         * Reserve both screen buffers before any lower-priority payload can
         * consume forensic blob space. Readback itself remains post-frame.
         */
        ps2GsCorePrepareTraceScreenshot();
        ps2_trace_build_config();
        ps2_trace_build_info();
    }
    ps2_trace_shader(s_shader);
    ps2RendererStatsBeginFrame();
    s_perf_renderer_build_start_us = sysGetMicroseconds();
    s_perf_renderer_build_active = true;
    ps2GsCoreBeginFrame();
}

static void ps2_log_renderer_stats(
    const struct Ps2RendererStats &stats, bool checkpoint)
{
    sysLogPrintf(LOG_NOTE,
        "GfxPS2 stats: frames=%llu translate_batches=%llu "
        "vertices=%llu ee_us=%llu",
        (unsigned long long)stats.frames,
        (unsigned long long)stats.translation_batches,
        (unsigned long long)stats.translated_vertices,
        (unsigned long long)stats.translation_microseconds);
    sysLogPrintf(LOG_NOTE,
        "GfxPS2 paths: path1_color=%llu path1_textured=%llu "
        "path1_vertices=%llu path1_records=%llu "
        "path3_color=%llu path3_textured=%llu "
        "path3_vertices=%llu path3_records=%llu "
        "unsupported=%llu/%llu triangles "
        "vu1_rejects=%llu/%llu vertices",
        (unsigned long long)stats.path1_color_batches,
        (unsigned long long)stats.path1_textured_batches,
        (unsigned long long)stats.path1_vertices,
        (unsigned long long)stats.path1_records,
        (unsigned long long)stats.path3_color_batches,
        (unsigned long long)stats.path3_textured_batches,
        (unsigned long long)stats.path3_vertices,
        (unsigned long long)stats.path3_records,
        (unsigned long long)stats.unsupported_shader_batches,
        (unsigned long long)stats.unsupported_shader_triangles,
        (unsigned long long)stats.vu1_rejected_batches,
        (unsigned long long)stats.vu1_rejected_vertices);
    struct Ps2RendererPerfSummary perf;
    ps2RendererStatsGetPerfSummary(&perf);
    if (perf.frame.sample_count != 0u) {
        sysLogPrintf(LOG_NOTE,
            "GfxPS2 perf frame[%u]: p50=%llu p95=%llu p99=%llu "
            "max=%llu us deadline=%u us misses=%u",
            perf.frame.sample_count,
            (unsigned long long)perf.frame.p50_microseconds,
            (unsigned long long)perf.frame.p95_microseconds,
            (unsigned long long)perf.frame.p99_microseconds,
            (unsigned long long)perf.frame.max_microseconds,
            perf.deadline_microseconds,
            perf.deadline_misses);
        sysLogPrintf(LOG_NOTE,
            "GfxPS2 perf split: build[%u] p50=%llu p95=%llu p99=%llu "
            "max=%llu us; GS-wait[%u] p50=%llu p95=%llu p99=%llu max=%llu us",
            perf.renderer_build.sample_count,
            (unsigned long long)perf.renderer_build.p50_microseconds,
            (unsigned long long)perf.renderer_build.p95_microseconds,
            (unsigned long long)perf.renderer_build.p99_microseconds,
            (unsigned long long)perf.renderer_build.max_microseconds,
            perf.present_wait.sample_count,
            (unsigned long long)perf.present_wait.p50_microseconds,
            (unsigned long long)perf.present_wait.p95_microseconds,
            (unsigned long long)perf.present_wait.p99_microseconds,
            (unsigned long long)perf.present_wait.max_microseconds);
    }
    sysLogPrintf(LOG_NOTE,
        "GfxPS2 VU1: transform_batches=%llu transform_vertices=%llu "
        "waits=%llu busy=%llu elided=%llu time=%llu us max=%llu us "
        "wait_timeouts=%llu wait_errors=%llu",
        (unsigned long long)stats.vu1_transform_batches,
        (unsigned long long)stats.vu1_transform_vertices,
        (unsigned long long)stats.vu1_wait_calls,
        (unsigned long long)stats.vu1_wait_busy_calls,
        (unsigned long long)stats.vu1_wait_elided_calls,
        (unsigned long long)stats.vu1_wait_microseconds,
        (unsigned long long)stats.vu1_wait_max_microseconds,
        (unsigned long long)stats.vu1_wait_timeouts,
        (unsigned long long)stats.vu1_wait_errors);
    if (checkpoint) {
        ps2LogCheckpointForce();
    }
}

extern "C" void gfxPs2LogRendererStats(int checkpoint)
{
    struct Ps2RendererStats stats;
    ps2RendererStatsGet(&stats);
    ps2_log_renderer_stats(stats, checkpoint != 0);
}

static void ps2_end_frame(void)
{
    ps2GsCoreSubmit();
    if (s_perf_renderer_build_active) {
        const uint64_t build_end_us = sysGetMicroseconds();
        ps2RendererStatsRecordRendererBuild(
            build_end_us >= s_perf_renderer_build_start_us
                ? build_end_us - s_perf_renderer_build_start_us : 0u);
        s_perf_renderer_build_active = false;
    }
    ps2RendererTraceMarkFrameEnd();

    if (ps2RendererTraceIsCapturing()) {
        const uint64_t viewport_xy =
            (uint32_t)s_viewport.x |
            ((uint64_t)(uint32_t)s_viewport.y << 32u);
        const uint64_t viewport_wh =
            (uint32_t)s_viewport.width |
            ((uint64_t)(uint32_t)s_viewport.height << 32u);
        const uint64_t textures =
            (uint32_t)s_selected_texture[0] |
            ((uint64_t)(uint32_t)s_selected_texture[1] << 32u);
        const uint16_t state_flags =
            (s_depth_test ? 1u : 0u) |
            (s_depth_update ? 2u : 0u) |
            (s_depth_compare ? 4u : 0u) |
            (s_alpha_blend ? 8u : 0u) |
            (s_modulate ? 16u : 0u);
        ps2RendererTraceRecord(PS2_TRACE_FRONTEND_STATE, state_flags,
            viewport_xy, viewport_wh, textures,
            (uint64_t)(uint32_t)s_active_texture_tile);
        ps2_trace_tmem_snapshot();
        ps2GsCoreRecordTraceSnapshot();

        struct Ps2RendererStats trace_stats;
        ps2RendererStatsGet(&trace_stats);
        ps2RendererTraceRecord(PS2_TRACE_RENDERER_STATS, 0u,
            trace_stats.translation_batches,
            trace_stats.translated_vertices,
            trace_stats.path1_vertices,
            trace_stats.path3_vertices);
        ps2RendererTraceRecord(PS2_TRACE_RENDERER_STATS, 1u,
            trace_stats.unsupported_shader_batches,
            trace_stats.unsupported_shader_triangles,
            trace_stats.vu1_rejected_batches,
            trace_stats.vu1_wait_microseconds);

        /*
         * MarkFrameEnd above already froze the measured frame interval. The
         * synchronous GS local-to-host readback is forensic-only and therefore
         * cannot masquerade as renderer time.
         */
        ps2GsCoreCaptureTraceTextureResidencies();
        ps2GsCoreCaptureTraceScreenshot();
        ps2GsCoreCaptureTraceVram();
        ps2RendererTraceEndFrameAndWrite();
    }

    /* Filesystem close/reopen stays outside primitive submission hot paths. */
    if (s_pending_unsupported_shader_checkpoint) {
        ps2LogCheckpointForce();
        s_pending_unsupported_shader_checkpoint = false;
        s_checkpointed_unsupported_shader = true;
    }

    struct Ps2RendererStats stats;
    ps2RendererStatsGet(&stats);
    const bool early_snapshot = stats.frames == 1u ||
        stats.frames == 60u || stats.frames == 120u;
    if (early_snapshot || stats.frames % 300u == 0u) {
        ps2_log_renderer_stats(stats, early_snapshot);
    }
}

extern "C" void gfxPs2RequestRendererCapture(
    uint32_t stage, uint32_t warmup_frames)
{
    ps2RendererTraceRequest(stage, warmup_frames);
}

static void ps2_finish_render(void)
{
    /*
     * Intentionally no unconditional GS FINISH here. Whole-system scheduling
     * treats FINISH as a dependency/profiling tool, not a per-frame ritual.
     * Buffer presentation belongs to the PS2 window-manager backend.
     */
}

static int ps2_create_framebuffer(void)
{
    if (!s_warned_framebuffer) {
        sysLogPrintf(LOG_WARNING,
            "GfxPS2 offscreen framebuffer API is not implemented; default scanout only");
        s_warned_framebuffer = true;
    }
    return 0;
}

static void ps2_update_framebuffer_parameters(int fb_id, uint32_t width, uint32_t height,
                                               uint32_t msaa_level, bool opengl_invert_y,
                                               bool render_target, bool has_depth_buffer,
                                               bool can_extract_depth)
{
    (void)fb_id; (void)width; (void)height; (void)msaa_level;
    (void)opengl_invert_y; (void)render_target; (void)has_depth_buffer; (void)can_extract_depth;
}

static bool ps2_start_draw_to_framebuffer(int fb_id, float noise_scale)
{
    (void)noise_scale;
    return fb_id == 0;
}

static void ps2_copy_framebuffer(int fb_dst, int fb_src, int left, int top, bool flip_y, bool use_back)
{
    (void)fb_dst; (void)fb_src; (void)left; (void)top; (void)flip_y; (void)use_back;
}

static void ps2_clear_framebuffer(bool clear_color, bool clear_depth)
{
    ps2GsCoreClear(clear_color, clear_depth);
}

static void ps2_resolve_msaa_color_buffer(int fb_id_target, int fb_id_source)
{
    (void)fb_id_target; (void)fb_id_source;
}

static void *ps2_get_framebuffer_texture_id(int fb_id)
{
    (void)fb_id;
    return NULL;
}

static void ps2_select_texture_fb(int fb_id)
{
    (void)fb_id;
}

static void ps2_delete_texture(uint32_t tex_id)
{
    if (tex_id <= UINT16_MAX) {
        ps2GsCoreReleaseTexture((Ps2GsTextureHandle)tex_id);
        if (tex_id < PS2_GFX_TEXTURE_STATE_SLOTS) {
            s_texture_sampler[tex_id] = {};
        }
    }
}

static void ps2_set_texture_filter(enum FilteringMode mode)
{
    s_filter_mode = mode;
}

static enum FilteringMode ps2_get_texture_filter(void)
{
    return s_filter_mode;
}

static void ps2_set_mipmap_filter(enum MipmapFilteringMode mode)
{
    s_mipmap_filter = mode;
}

static void ps2_set_anisotropy_level(int level)
{
    s_anisotropy = level;
}

static int ps2_get_max_anisotropy_level(void)
{
    return 1;
}

extern "C" {
struct GfxRenderingAPI gfx_ps2_api = {
    ps2_get_name,
    ps2_get_max_texture_size,
    ps2_get_clip_parameters,
    ps2_unload_shader,
    ps2_load_shader,
    ps2_create_and_load_new_shader,
    ps2_lookup_shader,
    ps2_shader_get_info,
    ps2_clear_shaders,
    ps2_new_texture,
    ps2_select_texture,
    ps2_upload_texture,
    ps2_set_sampler_parameters,
    ps2_set_depth_mode,
    ps2_set_depth_range,
    ps2_set_viewport,
    ps2_set_scissor,
    ps2_set_use_alpha,
    ps2_draw_triangles,
    ps2_init,
    ps2_on_resize,
    ps2_start_frame,
    ps2_end_frame,
    ps2_finish_render,
    ps2_create_framebuffer,
    ps2_update_framebuffer_parameters,
    ps2_start_draw_to_framebuffer,
    ps2_copy_framebuffer,
    ps2_clear_framebuffer,
    ps2_resolve_msaa_color_buffer,
    ps2_get_framebuffer_texture_id,
    ps2_select_texture_fb,
    ps2_delete_texture,
    ps2_set_texture_filter,
    ps2_get_texture_filter,
    ps2_set_mipmap_filter,
    ps2_set_anisotropy_level,
    ps2_get_max_anisotropy_level,
};
}
