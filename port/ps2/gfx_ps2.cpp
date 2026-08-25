#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "gfx_cc.h"
#include "gfx_rendering_api.h"
#include "gfx_ps2.h"
#include "gs_core.h"
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
 * One-cycle color and alpha equations are classified independently. gfx_pc.cpp
 * already resolves SHADE/PRIMITIVE/ENVIRONMENT/etc. into compact INPUTn VBO
 * channels, so INPUT1 is intentionally semantic-agnostic here. This lets a
 * single RGBAQ record carry RGB from one N64 source and alpha from another.
 *
 * Current exact fixed-function color recipes:
 *   - INPUT1
 *   - TEXEL0
 *   - TEXEL0 * INPUT1
 *
 * Current exact alpha recipes:
 *   - opaque/one/zero
 *   - INPUT1
 *   - TEXEL0
 *   - TEXEL0 * INPUT1
 *
 * GS texture MODULATE uses 0x80 as unity and multiplies with >>7. Fast3D's
 * RGBA8 texture upload keeps texel alpha in 0..255, while GS fragment alpha uses
 * the native 0..0x80 range. When texture alpha participates (TCC=RGBA), fragment
 * alpha is therefore scaled to 0..0x40 so the MODULATE result returns to the
 * GS-native 0..0x80 range without repacking every texture at upload time.
 *
 * Fog maps to the GS native FOGCOL + per-vertex XYZF2 path. Fast3D's factor is
 * the fog contribution, while GS F is the source-color contribution, so the
 * conversion is F = 255 * (1 - factor).
 *
 * Unsupported combiners are retained in the shader table so shader_get_info()
 * still reports the exact upstream VBO layout, but draw submission rejects the
 * unsupported recipe rather than silently rendering a wrong approximation.
 */

#define PS2_GFX_MAX_SHADERS 32
#define PS2_GFX_TRANSLATE_VERTS 96
/* Fast3D threshold is 8/256. GS alpha unity is 0x80, hence reference 4. */
#define PS2_GFX_ALPHA_THRESHOLD 4u

/* GS packed-register IDs consumed by the packet-ready core boundary. */
#define PS2_GS_REG_RGBAQ 0x01u
#define PS2_GS_REG_ST    0x02u
#define PS2_GS_REG_XYZF2 0x04u
#define PS2_GS_REG_XYZ2  0x05u

enum Ps2ColorRecipe {
    PS2_COLOR_UNSUPPORTED = 0,
    PS2_COLOR_INPUT1,
    PS2_COLOR_TEX0,
    PS2_COLOR_TEX0_MUL_INPUT1,
};

enum Ps2AlphaRecipe {
    PS2_ALPHA_UNSUPPORTED = 0,
    PS2_ALPHA_OPAQUE,
    PS2_ALPHA_ZERO,
    PS2_ALPHA_ONE,
    PS2_ALPHA_INPUT1,
    PS2_ALPHA_TEX0,
    PS2_ALPHA_TEX0_MUL_INPUT1,
};

struct ShaderProgram {
    bool used;
    bool supported;
    bool textured;
    bool texture_alpha;
    bool warned_rejected_draw;
    enum Ps2ColorRecipe color_recipe;
    enum Ps2AlphaRecipe alpha_recipe;
    uint64_t shader_id0;
    uint32_t shader_id1;
    struct CCFeatures features;
};

struct Ps2Viewport {
    int x;
    int y;
    int width;
    int height;
};

static struct ShaderProgram s_shaders[PS2_GFX_MAX_SHADERS];
static struct ShaderProgram *s_shader;
static Ps2GsTextureHandle s_selected_texture[2];
static int s_active_texture_tile;
static struct Ps2Viewport s_viewport;
static float s_depth_near = 0.0f;
static float s_depth_far = 1.0f;
static bool s_depth_test = true;
static bool s_depth_update = true;
static bool s_modulate;
static enum FilteringMode s_filter_mode = FILTER_LINEAR;
static enum MipmapFilteringMode s_mipmap_filter = MIPMAP_DISABLED;
static int s_anisotropy = 1;
static bool s_warned_framebuffer;
static bool s_warned_depth_prim;
static bool s_warned_mipmap;

static struct Ps2GsTexturedVertex s_stq_vertices[PS2_GFX_TRANSLATE_VERTS];
static struct Ps2GsColorVertex s_color_vertices[PS2_GFX_TRANSLATE_VERTS];

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

static bool ps2_shader_common_supported(const struct CCFeatures *f)
{
    /*
     * These options need explicit GS state or an additional rendering pass.
     * Reject them until that mapping exists instead of accepting a visually
     * plausible but semantically wrong approximation. Alpha threshold and fog
     * both have fixed-function GS mappings and are allowed here.
     */
    return !f->opt_texture_edge && !f->opt_noise && !f->opt_2cyc &&
           !f->opt_invisible && !f->opt_grayscale && !f->opt_blur &&
           (!f->opt_alpha_threshold || f->opt_alpha) &&
           !f->used_textures[1] && f->num_inputs <= 1;
}

static bool ps2_shader_item_is_tex0_alpha(uint8_t item)
{
    /*
     * CURRENT IMPLEMENTATION: gfx_generate_cc() encodes G_ACMUX_TEXEL0 as
     * SHADER_TEXEL0 in the alpha half. SHADER_TEXEL0A is used when texture
     * alpha enters through the RGB combiner. Both mean texel0.a when consumed
     * as an alpha scalar, matching the current OpenGL backend's interpretation.
     */
    return item == SHADER_TEXEL0 || item == SHADER_TEXEL0A;
}

static enum Ps2ColorRecipe ps2_classify_color_recipe(const struct CCFeatures *f)
{
    if (f->do_single[0][0]) {
        if (f->c[0][0][3] == SHADER_INPUT_1) {
            return PS2_COLOR_INPUT1;
        }
        if (f->c[0][0][3] == SHADER_TEXEL0) {
            return PS2_COLOR_TEX0;
        }
    }

    if (f->do_multiply[0][0]) {
        const uint8_t a = f->c[0][0][0];
        const uint8_t c = f->c[0][0][2];
        if ((a == SHADER_TEXEL0 && c == SHADER_INPUT_1) ||
            (a == SHADER_INPUT_1 && c == SHADER_TEXEL0)) {
            return PS2_COLOR_TEX0_MUL_INPUT1;
        }
    }

    return PS2_COLOR_UNSUPPORTED;
}

static enum Ps2AlphaRecipe ps2_classify_alpha_recipe(const struct CCFeatures *f)
{
    if (!f->opt_alpha) {
        return PS2_ALPHA_OPAQUE;
    }

    if (f->do_single[0][1]) {
        const uint8_t d = f->c[0][1][3];
        if (d == SHADER_0) {
            return PS2_ALPHA_ZERO;
        }
        if (d == SHADER_1) {
            return PS2_ALPHA_ONE;
        }
        if (d == SHADER_INPUT_1) {
            return PS2_ALPHA_INPUT1;
        }
        if (ps2_shader_item_is_tex0_alpha(d)) {
            return PS2_ALPHA_TEX0;
        }
    }

    if (f->do_multiply[0][1]) {
        const uint8_t a = f->c[0][1][0];
        const uint8_t c = f->c[0][1][2];
        if ((ps2_shader_item_is_tex0_alpha(a) && c == SHADER_INPUT_1) ||
            (a == SHADER_INPUT_1 && ps2_shader_item_is_tex0_alpha(c))) {
            return PS2_ALPHA_TEX0_MUL_INPUT1;
        }
    }

    return PS2_ALPHA_UNSUPPORTED;
}

static bool ps2_color_recipe_textured(enum Ps2ColorRecipe recipe)
{
    return recipe == PS2_COLOR_TEX0 || recipe == PS2_COLOR_TEX0_MUL_INPUT1;
}

static bool ps2_color_recipe_uses_input1(enum Ps2ColorRecipe recipe)
{
    return recipe == PS2_COLOR_INPUT1 || recipe == PS2_COLOR_TEX0_MUL_INPUT1;
}

static bool ps2_alpha_recipe_textured(enum Ps2AlphaRecipe recipe)
{
    return recipe == PS2_ALPHA_TEX0 || recipe == PS2_ALPHA_TEX0_MUL_INPUT1;
}

static bool ps2_alpha_recipe_uses_input1(enum Ps2AlphaRecipe recipe)
{
    return recipe == PS2_ALPHA_INPUT1 || recipe == PS2_ALPHA_TEX0_MUL_INPUT1;
}

static bool ps2_shader_recipe_supported(struct ShaderProgram *prg)
{
    const struct CCFeatures *f = &prg->features;
    if (!ps2_shader_common_supported(f) ||
        prg->color_recipe == PS2_COLOR_UNSUPPORTED ||
        prg->alpha_recipe == PS2_ALPHA_UNSUPPORTED) {
        return false;
    }

    const bool color_textured = ps2_color_recipe_textured(prg->color_recipe);
    const bool alpha_textured = ps2_alpha_recipe_textured(prg->alpha_recipe);
    const bool uses_input1 = ps2_color_recipe_uses_input1(prg->color_recipe) ||
                             ps2_alpha_recipe_uses_input1(prg->alpha_recipe);

    /*
     * GS MODULATE applies the texture function to RGB and alpha together. A
     * texture used only by the alpha equation while RGB is untextured would
     * therefore alter RGB and needs another mapping/pass; reject it for now.
     */
    if (alpha_textured && !color_textured) {
        return false;
    }

    if (f->used_textures[0] != color_textured) {
        return false;
    }

    if (uses_input1 ? (f->num_inputs != 1) : (f->num_inputs != 0)) {
        return false;
    }

    prg->textured = color_textured;
    prg->texture_alpha = alpha_textured;
    return true;
}

static void ps2_log_shader_recipe(int slot, const struct ShaderProgram *prg)
{
    const struct CCFeatures *f = &prg->features;

    sysLogPrintf(prg->supported ? LOG_NOTE : LOG_WARNING,
        "GfxPS2 shader %02d id=%016llx/%08x supported=%d tex=%d%d inputs=%d "
        "recipe=%d/%d tcc=%d rgb=[%u,%u,%u,%u] a=[%u,%u,%u,%u] "
        "opts=a%d f%d e%d n%d 2c%d at%d inv%d g%d b%d",
        slot,
        (unsigned long long)prg->shader_id0,
        (unsigned int)prg->shader_id1,
        prg->supported ? 1 : 0,
        f->used_textures[0] ? 1 : 0,
        f->used_textures[1] ? 1 : 0,
        f->num_inputs,
        (int)prg->color_recipe,
        (int)prg->alpha_recipe,
        prg->texture_alpha ? 1 : 0,
        f->c[0][0][0], f->c[0][0][1], f->c[0][0][2], f->c[0][0][3],
        f->c[0][1][0], f->c[0][1][1], f->c[0][1][2], f->c[0][1][3],
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

    const bool threshold = new_prg && new_prg->supported &&
                           new_prg->features.opt_alpha_threshold;
    ps2GsCoreSetAlphaTest(threshold, threshold ? PS2_GFX_ALPHA_THRESHOLD : 0u);

    const bool fog = new_prg && new_prg->supported && new_prg->features.opt_fog;
    ps2GsCoreSetFog(fog, 0u, 0u, 0u);

    const bool texture_alpha = new_prg && new_prg->supported && new_prg->texture_alpha;
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
            prg->color_recipe = ps2_classify_color_recipe(&prg->features);
            prg->alpha_recipe = ps2_classify_alpha_recipe(&prg->features);
            prg->supported = ps2_shader_recipe_supported(prg);

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
    return (uint32_t)ps2GsCoreCreateTexture();
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
    ps2GsCoreUploadTextureRgba32(
        s_selected_texture[s_active_texture_tile], rgba32_buf, width, height);
}

static void ps2_set_sampler_parameters(int sampler, bool linear_filter, uint32_t cms, uint32_t cmt, bool mipmaps)
{
    if (sampler < 0 || sampler > 1) {
        return;
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
    (void)zmode;

    s_depth_test = depth_test;
    s_depth_update = depth_update;
    ps2GsCoreSetDepthMode(depth_test, depth_update, depth_compare);

    if (depth_source_prim && !s_warned_depth_prim) {
        sysLogPrintf(LOG_WARNING,
            "GfxPS2 primitive depth source requested; bring-up backend still consumes per-vertex clip Z");
        s_warned_depth_prim = true;
    }
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
    ps2GsCoreSetScissor(x, y, width, height);
}

static void ps2_set_use_alpha(bool use_alpha, bool modulate)
{
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

static void ps2_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris)
{
    if (!ps2GsCoreIsReady() || !s_shader || !buf_vbo || buf_vbo_num_tris == 0) {
        return;
    }

    if (!s_shader->supported) {
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

    if (s_shader->textured && !ps2GsCoreTextureReady(s_selected_texture[0])) {
        return;
    }

    bool fog_color_emitted = false;
    size_t base_vertex = 0;
    while (base_vertex < vertex_count) {
        size_t batch_vertices = vertex_count - base_vertex;
        if (batch_vertices > PS2_GFX_TRANSLATE_VERTS) {
            batch_vertices = PS2_GFX_TRANSLATE_VERTS;
            batch_vertices -= batch_vertices % 3;
        }

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
                tex_u[t] = src[pos++];
                tex_v[t] = src[pos++];
                if (s_shader->features.clamp[t][0]) ++pos;
                if (s_shader->features.clamp[t][1]) ++pos;
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
                    ps2GsCoreSetFog(true,
                        ps2_u8_component(fog_r),
                        ps2_u8_component(fog_g),
                        ps2_u8_component(fog_b));
                    fog_color_emitted = true;
                }
            }

            if (s_shader->features.opt_grayscale) pos += 4;

            float input_r = 1.0f;
            float input_g = 1.0f;
            float input_b = 1.0f;
            float input_a = 1.0f;
            if (s_shader->features.num_inputs > 0) {
                input_r = src[pos++];
                input_g = src[pos++];
                input_b = src[pos++];
                if (s_shader->features.opt_alpha) {
                    input_a = src[pos++];
                }
            }

            const float sx = (float)s_viewport.x + (ndc_x * 0.5f + 0.5f) * (float)s_viewport.width;
            const float sy = (float)s_viewport.y + (0.5f - ndc_y * 0.5f) * (float)s_viewport.height;
            const int iz = ps2_map_clip_depth(clip_z, clip_w);

            uint8_t cr = 0;
            uint8_t cg = 0;
            uint8_t cb = 0;
            switch (s_shader->color_recipe) {
                case PS2_COLOR_INPUT1:
                    cr = ps2_u8_component(input_r);
                    cg = ps2_u8_component(input_g);
                    cb = ps2_u8_component(input_b);
                    break;
                case PS2_COLOR_TEX0:
                    cr = cg = cb = 0x80;
                    break;
                case PS2_COLOR_TEX0_MUL_INPUT1:
                    cr = ps2_modulate_component(input_r);
                    cg = ps2_modulate_component(input_g);
                    cb = ps2_modulate_component(input_b);
                    break;
                default:
                    break;
            }

            uint8_t ca = 0x80;
            switch (s_shader->alpha_recipe) {
                case PS2_ALPHA_ZERO:
                    ca = 0x00;
                    break;
                case PS2_ALPHA_INPUT1:
                    ca = ps2_modulate_component(input_a);
                    break;
                case PS2_ALPHA_TEX0:
                    ca = 0x40;
                    break;
                case PS2_ALPHA_TEX0_MUL_INPUT1:
                    ca = ps2_texture_alpha_fragment_component(input_a);
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

            if (s_shader->textured) {
                s_stq_vertices[i].rgbaq = ps2_pack_rgbaq(cr, cg, cb, ca, inv_w);
                s_stq_vertices[i].st = ps2_pack_st(tex_u[0] * inv_w, tex_v[0] * inv_w);
                s_stq_vertices[i].xyz2 = packed_position;
            } else {
                s_color_vertices[i].rgbaq = ps2_pack_rgbaq(cr, cg, cb, ca, 0.0f);
                s_color_vertices[i].xyz2 = packed_position;
            }
        }

        if (s_shader->textured) {
            ps2GsCoreDrawTexturedTriangles(
                s_selected_texture[0], s_stq_vertices, (uint32_t)batch_vertices);
        } else {
            ps2GsCoreDrawColorTriangles(s_color_vertices, (uint32_t)batch_vertices);
        }

        base_vertex += batch_vertices;
    }
}

static void ps2_reset_viewport(void)
{
    if (!ps2GsCoreIsReady()) {
        s_viewport.x = 0;
        s_viewport.y = 0;
        s_viewport.width = 0;
        s_viewport.height = 0;
        return;
    }

    s_viewport.x = 0;
    s_viewport.y = 0;
    s_viewport.width = ps2GsCoreGetWidth();
    s_viewport.height = ps2GsCoreGetHeight();
}

static void ps2_init(void)
{
    ps2_clear_shaders();
    s_selected_texture[0] = PS2_GS_TEXTURE_INVALID;
    s_selected_texture[1] = PS2_GS_TEXTURE_INVALID;
    s_active_texture_tile = 0;
    s_depth_near = 0.0f;
    s_depth_far = 1.0f;
    s_depth_test = true;
    s_depth_update = true;
    s_modulate = false;
    s_filter_mode = FILTER_LINEAR;
    s_mipmap_filter = MIPMAP_DISABLED;
    s_anisotropy = 1;
    ps2GsCoreSetAlphaTest(false, 0u);
    ps2GsCoreSetFog(false, 0u, 0u, 0u);
    ps2GsCoreSetTextureAlpha(false);
    ps2_reset_viewport();

    sysLogPrintf(LOG_NOTE,
        "GfxPS2 init: shaders=%d translate_batch=%d native fog/alpha-test one-cycle recipes",
        PS2_GFX_MAX_SHADERS, PS2_GFX_TRANSLATE_VERTS);
}

static void ps2_on_resize(void)
{
    ps2_reset_viewport();
}

static void ps2_start_frame(void)
{
    ps2GsCoreBeginFrame();
}

static void ps2_end_frame(void)
{
    ps2GsCoreSubmit();
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
