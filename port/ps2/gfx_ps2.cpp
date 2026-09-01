#include <stdbool.h>
#include <float.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "gfx_cc.h"
#include "gfx_rendering_api.h"
#include "gfx_ps2.h"
#include "gfx_ps2_combiner.h"
#include "gfx_ps2_pass_graph.h"
#include "gs_core.h"
#include "gs_vu1_batch.h"
#include "gs_vu1_transform.h"
#include "log_ps2.h"
#include "ps2_renderer_stats.h"
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
    bool expanded_mirror_s;
    bool expanded_mirror_t;
    bool monochrome_rgb;
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
static bool s_alpha_blend;
static bool s_modulate;
static uint32_t s_sampler_cms[2];
static uint32_t s_sampler_cmt[2];
static struct Ps2TextureSamplerState
    s_texture_sampler[PS2_GFX_TEXTURE_STATE_SLOTS];
static struct Ps2TextureRegionClampState s_draw_region_clamp[2];
static enum FilteringMode s_filter_mode = FILTER_LINEAR;
static enum MipmapFilteringMode s_mipmap_filter = MIPMAP_DISABLED;
static int s_anisotropy = 1;
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
static Ps2GsRenderTargetHandle s_alpha_trilerp_color_target;
static Ps2GsRenderTargetHandle s_alpha_trilerp_scalar_target;
static uint8_t s_draw_fog_r;
static uint8_t s_draw_fog_g;
static uint8_t s_draw_fog_b;
static uint8_t s_draw_texture_edge_reference =
    PS2_GFX_TEXTURE_EDGE_THRESHOLD;

static struct Ps2GsTexturedVertex s_stq_vertices[2][PS2_GFX_TRANSLATE_VERTS];
static struct Ps2GsColorVertex s_color_vertices[PS2_GFX_TRANSLATE_VERTS];
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

static void ps2_load_shader(struct ShaderProgram *new_prg)
{
    s_shader = new_prg;

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
    }
    if ((mirror_s || mirror_t) && !s_logged_native_mirror) {
        sysLogPrintf(LOG_NOTE,
            "GfxPS2 native mirror-wrap: reflected 2x GS residency, axes=%s%s",
            mirror_s ? "S" : "", mirror_t ? "T" : "");
        s_logged_native_mirror = true;
    }
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
    if (texture_id < PS2_GFX_TEXTURE_STATE_SLOTS) {
        s_sampler_cms[tile] = s_texture_sampler[texture_id].cms;
        s_sampler_cmt[tile] = s_texture_sampler[texture_id].cmt;
        ps2GsCoreSetTextureClamp(
            s_sampler_cms[tile], s_sampler_cmt[tile]);
    }
    ps2GsCoreSetTextureFilter(handle, linear_filter);
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
    }
}

extern "C" void gfxPs2SetTextureUploadMirror(uint8_t cms, uint8_t cmt)
{
    s_upload_mirror_s = (cms & 1u) != 0u;
    s_upload_mirror_t = (cmt & 1u) != 0u;
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
        if (!s_logged_native_ia16) {
            sysLogPrintf(LOG_NOTE,
                "GfxPS2 native texture path: exact N64 IA16 -> GS PSMCT32");
            s_logged_native_ia16 = true;
        }
        return true;
    }

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
    const uint32_t texture_id = (uint32_t)s_selected_texture[sampler];
    if (texture_id < PS2_GFX_TEXTURE_STATE_SLOTS) {
        s_texture_sampler[texture_id].cms = cms;
        s_texture_sampler[texture_id].cmt = cmt;
    }
    ps2GsCoreSetTextureFilter(s_selected_texture[sampler], linear_filter);
    ps2GsCoreSetTextureClamp(cms, cmt);

    if (mipmaps && !s_warned_mipmap) {
        sysLogPrintf(LOG_WARNING, "GfxPS2 mipmap sampling requested but not implemented");
        s_warned_mipmap = true;
    }
}

static void ps2_set_depth_mode(bool depth_test, bool depth_update, bool depth_compare,
                               bool depth_source_prim, uint16_t zmode)
{
    (void)depth_source_prim;
    (void)zmode;

    s_depth_test = depth_test;
    s_depth_update = depth_update;
    s_depth_compare = depth_compare;
    ps2GsCoreSetDepthMode(depth_test, depth_update, depth_compare);

}

static void ps2_set_depth_range(float znear, float zfar)
{
    s_depth_near = znear;
    s_depth_far = zfar;
}

static void ps2_set_viewport(int x, int y, int width, int height)
{
    s_viewport.x = x;
    s_viewport.y = y;
    s_viewport.width = width;
    s_viewport.height = height;
}

static void ps2_set_scissor(int x, int y, int width, int height)
{
    s_scissor.x = x;
    s_scissor.y = y;
    s_scissor.width = width;
    s_scissor.height = height;
    ps2GsCoreSetScissor(x, y, width, height);
}

static void ps2_set_use_alpha(bool use_alpha, bool modulate)
{
    s_alpha_blend = use_alpha;
    s_modulate = modulate;
    ps2GsCoreSetAlphaBlend(use_alpha);
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
    return 1 + (int)((1.0f - ranged) * 65534.0f);
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

static void ps2_trilerp_set_base_state(void)
{
    ps2GsCoreSetDepthMode(s_depth_test, s_depth_update, s_depth_compare);
    ps2GsCoreSetAlphaBlend(false);
    ps2GsCoreSetAlphaWrite(true);
    ps2GsCoreSetTextureAlpha(false);
    ps2_apply_texture_clamp(0);
}

static void ps2_trilerp_set_lerp_state(void)
{
    /* Equal-depth fragments must pass, but the second pass must not rewrite Z. */
    ps2GsCoreSetDepthMode(s_depth_test, false, s_depth_compare);
    ps2GsCoreSetAlphaBlend(true);
    ps2GsCoreSetAlphaWrite(false);
    ps2GsCoreSetTextureAlpha(false);
    ps2_apply_texture_clamp(1);
}

static void ps2_trilerp_restore_state(void)
{
    ps2GsCoreSetAlphaWrite(true);
    ps2GsCoreSetAlphaBlend(s_alpha_blend);
    ps2GsCoreSetDepthMode(s_depth_test, s_depth_update, s_depth_compare);
    ps2_apply_texture_clamp(0);
}

static void ps2_input1_tex0_lerp_set_base_state(void)
{
    ps2GsCoreSetDepthMode(s_depth_test, s_depth_update, s_depth_compare);
    ps2GsCoreSetAlphaBlend(false);
    ps2GsCoreSetAlphaWrite(true);
    ps2GsCoreSetTextureAlpha(false);
}

static void ps2_input1_tex0_lerp_set_texture_state(void)
{
    ps2GsCoreSetDepthMode(s_depth_test, false, s_depth_compare);
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
    ps2GsCoreSetDepthMode(s_depth_test, s_depth_update, s_depth_compare);
    ps2GsCoreSetColorWrite(true);
    ps2GsCoreSetAlphaWrite(true);
    ps2GsCoreSetAlphaBlend(s_alpha_blend);
    ps2GsCoreSetFramebufferAlphaForce(false);
    ps2GsCoreSetAlphaTest(
        s_shader && s_shader->features.opt_alpha_threshold,
        s_shader && s_shader->features.opt_alpha_threshold ?
            PS2_GFX_ALPHA_THRESHOLD : 0u);
    ps2GsCoreSetFog(false, 0u, 0u, 0u);
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
    ps2GsCoreSetDepthMode(false, false, false);
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
    ps2GsCoreSetDepthMode(s_depth_test, s_depth_update, s_depth_compare);
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
    ps2GsCoreSetDepthMode(false, false, false);
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
    ps2GsCoreSetDepthMode(s_depth_test, s_depth_update, s_depth_compare);
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
    ps2GsCoreSetDepthMode(false, false, false);
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
    ps2GsCoreSetDepthMode(s_depth_test, s_depth_update, s_depth_compare);
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
    ps2GsCoreSetDepthMode(false, false, false);
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
    if (!ps2GsCoreBlitRenderTargetRedToActiveAlpha(
            s_alpha_trilerp_scalar_target)) {
        return false;
    }

    ps2GsCoreBindDefaultRenderTarget();
    ps2GsCoreSetScissor(tile->x, tile->y, tile->width, tile->height);
    ps2GsCoreSetDepthMode(s_depth_test, s_depth_update, s_depth_compare);
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
    ps2GsCoreSetDepthMode(false, false, false);
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
    ps2GsCoreSetDepthMode(s_depth_test, s_depth_update, s_depth_compare);
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
    struct Ps2GsTexturedVertex composite[3];
    ps2_make_independent_alpha_texture_triangle(
        triangle, 0, tile->x, tile->y, false, texture0);
    ps2_make_independent_alpha_texture_triangle(
        triangle, 1, tile->x, tile->y, true, texture1);
    ps2_make_alpha_trilerp_workspace_triangle(
        triangle, tile->x, tile->y, 0x80u, false, true, false,
        composite);

    if (!ps2GsCoreBindRenderTarget(s_alpha_trilerp_color_target)) {
        return false;
    }
    ps2GsCoreSetAlphaWrite(true);
    ps2GsCoreSetAlphaTest(false, 0u);
    ps2GsCoreSetFog(s_shader->features.opt_fog,
        s_draw_fog_r, s_draw_fog_g, s_draw_fog_b);
    ps2GsCoreSetDepthMode(false, false, false);
    ps2GsCoreSetAlphaBlend(false);
    ps2GsCoreSetFramebufferAlphaForce(false);
    ps2GsCoreSetTextureAlpha(s_shader->plan.texture_alpha);
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

    ps2GsCoreBindDefaultRenderTarget();
    ps2GsCoreSetScissor(tile->x, tile->y, tile->width, tile->height);
    ps2GsCoreSetDepthMode(s_depth_test, s_depth_update, s_depth_compare);
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
    struct Ps2GsTexturedVertex composite[3];
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
    ps2_make_alpha_trilerp_workspace_triangle(
        triangle, tile->x, tile->y, 0x80u, false, true, true,
        composite);

    ps2GsCoreSetAlphaWrite(true);
    ps2GsCoreSetAlphaTest(false, 0u);
    ps2GsCoreSetFog(false, 0u, 0u, 0u);
    ps2GsCoreSetDepthMode(false, false, false);
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
    if (!ps2GsCoreBlitRenderTargetRedToActiveAlpha(
            s_alpha_trilerp_scalar_target)) {
        return false;
    }

    ps2GsCoreBindDefaultRenderTarget();
    ps2GsCoreSetScissor(tile->x, tile->y, tile->width, tile->height);
    ps2GsCoreSetDepthMode(s_depth_test, s_depth_update, s_depth_compare);
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
    if (s_modulate) {
        if (!s_warned_alpha_trilerp_modulate) {
            sysLogPrintf(LOG_WARNING,
                "GfxPS2 alpha-trilerp rejects destination-color blend mode");
            s_warned_alpha_trilerp_modulate = true;
        }
        return false;
    }
    if (!ps2_ensure_alpha_trilerp_workspace()) {
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
            success = ps2GfxGetPassGraphTile(
                &tiles, tile_index, &tile) &&
                ps2_draw_alpha_trilerp_tile(
                    &s_alpha_trilerp_vertices[vertex], &tile);
        }
    }

    ps2_restore_alpha_trilerp_state();
    if (!success && !s_warned_alpha_trilerp_workspace) {
        sysLogPrintf(LOG_ERROR,
            "GfxPS2 alpha-trilerp pass graph submission failed");
        s_warned_alpha_trilerp_workspace = true;
    }
    return success;
}

static void ps2_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris)
{
    if (!ps2GsCoreIsReady() || !s_shader || !buf_vbo || buf_vbo_num_tris == 0) {
        return;
    }

    if (!s_shader->plan.supported) {
        if (!s_shader->warned_rejected_draw) {
            sysLogPrintf(LOG_WARNING,
                "GfxPS2 dropping unsupported shader id=%016llx/%08x first_batch_tris=%u",
                (unsigned long long)s_shader->shader_id0,
                (unsigned int)s_shader->shader_id1,
                (unsigned int)buf_vbo_num_tris);
            s_shader->warned_rejected_draw = true;
        }
        return;
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
    const bool interference = s_shader->plan.pass_graph ==
        PS2_PASS_GRAPH_INTERFERENCE &&
        s_shader->plan.color_recipe ==
            PS2_COLOR_TEX0_MUL_TEX1_MUL_INPUT1;
    if (s_shader->plan.textured &&
        (!ps2GsCoreTextureReady(s_selected_texture[0]) ||
         ((opaque_trilerp || alpha_trilerp ||
           independent_alpha_trilerp || interference ||
           tex1_alpha_factor_lerp) &&
          !ps2GsCoreTextureReady(s_selected_texture[1])))) {
        return;
    }

    bool fog_color_emitted = false;
    const bool texture_edge = s_shader->features.opt_texture_edge;
    const bool invisible = s_shader->features.opt_invisible;
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
        bool vu1_transform_eligible = ps2GsVu1BuildViewportMapping(
            s_viewport.x, s_viewport.y,
            s_viewport.width, s_viewport.height,
            ps2GsCoreGetOffsetX(), ps2GsCoreGetOffsetY(),
            s_depth_near, s_depth_far,
            transform_scale, transform_offset);
#endif

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
                const bool mirror_s =
                    handle < PS2_GFX_TEXTURE_STATE_SLOTS &&
                    s_texture_sampler[handle].expanded_mirror_s;
                const bool mirror_t =
                    handle < PS2_GFX_TEXTURE_STATE_SLOTS &&
                    s_texture_sampler[handle].expanded_mirror_t;
                tex_u[t] = src[pos++] * (mirror_s ? 0.5f : 1.0f);
                tex_v[t] = src[pos++] * (mirror_t ? 0.5f : 1.0f);
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

            struct Ps2GsPackedReg packed_position;
            if (s_shader->features.opt_fog) {
                packed_position = ps2_pack_xyzf2(
                    sx, sy, iz, ps2_fog_coefficient(fog_factor));
            } else {
                packed_position = ps2_pack_xyz2(sx, sy, iz);
            }

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
                        PS2_ALPHA_INPUT1) {
                    vertex->shade_a = 0x80u;
                    vertex->independent_alpha =
                        ps2_modulate_component(input[0][3]);
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
                }
                vertex->fog = ps2_fog_coefficient(fog_factor);
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
        }
        ps2RendererStatsRecordTranslation(
            (uint32_t)batch_vertices,
            sysGetMicroseconds() - translation_start);

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
    s_depth_test = true;
    s_depth_update = true;
    s_depth_compare = true;
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
    s_upload_mirror_s = false;
    s_upload_mirror_t = false;
    ps2GsCoreSetAlphaTest(false, 0u);
    ps2GsCoreSetFramebufferAlphaForce(false);
    ps2GsCoreSetColorWrite(true);
    ps2GsCoreSetFog(false, 0u, 0u, 0u);
    ps2GsCoreSetTextureAlpha(false);
    ps2_reset_viewport();

    sysLogPrintf(LOG_NOTE,
        "GfxPS2 init: shaders=%d translate_batch=%d native fog/alpha-test exact one-pass recipes",
        PS2_GFX_MAX_SHADERS, PS2_GFX_TRANSLATE_VERTS);
}

static void ps2_on_resize(void)
{
    ps2_reset_viewport();
}

static void ps2_start_frame(void)
{
    ps2RendererStatsBeginFrame();
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
        "vu1_rejects=%llu/%llu vertices",
        (unsigned long long)stats.path1_color_batches,
        (unsigned long long)stats.path1_textured_batches,
        (unsigned long long)stats.path1_vertices,
        (unsigned long long)stats.path1_records,
        (unsigned long long)stats.path3_color_batches,
        (unsigned long long)stats.path3_textured_batches,
        (unsigned long long)stats.path3_vertices,
        (unsigned long long)stats.path3_records,
        (unsigned long long)stats.vu1_rejected_batches,
        (unsigned long long)stats.vu1_rejected_vertices);
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

extern "C" void gfxPs2LogRendererStats(bool checkpoint)
{
    struct Ps2RendererStats stats;
    ps2RendererStatsGet(&stats);
    ps2_log_renderer_stats(stats, checkpoint);
}

static void ps2_end_frame(void)
{
    ps2GsCoreSubmit();

    struct Ps2RendererStats stats;
    ps2RendererStatsGet(&stats);
    const bool early_snapshot = stats.frames == 1u ||
        stats.frames == 60u || stats.frames == 120u;
    if (early_snapshot || stats.frames % 300u == 0u) {
        ps2_log_renderer_stats(stats, early_snapshot);
    }
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
