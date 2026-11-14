#define NOMINMAX

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <cstdio>

#if !defined(PLATFORM_PS2)
#include <list>
#include <map>
#include <unordered_map>
#include <vector>
#endif
#include <limits>

#ifndef _LANGUAGE_C
#define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#include "platform.h"

#include "gfx_pc.h"
#include "gfx_exact_div.h"
#include "gfx_cc.h"
#include "gfx_window_manager_api.h"
#include "gfx_rendering_api.h"
#include "gfx_primitive_depth.h"
#include "gfx_screen_config.h"

uintptr_t gfxFramebuffer;

#define ALIGN(x, a) (((x) + (a - 1)) & ~(a - 1))

#define SUPPORT_CHECK(x) assert(x)

// SCALE_M_N: upscale/downscale M-bit integer to N-bit
#define SCALE_5_8(VAL_) (((VAL_)*0xFF) / 0x1F)
#define SCALE_8_5(VAL_) ((((VAL_) + 4) * 0x1F) / 0xFF)
#define SCALE_4_8(VAL_) ((VAL_)*0x11)
#define SCALE_8_4(VAL_) ((VAL_) / 0x11)
#define SCALE_3_8(VAL_) ((VAL_)*0x24)
#define SCALE_8_3(VAL_) ((VAL_) / 0x24)

// SCREEN_WIDTH and SCREEN_HEIGHT are defined in the headerfile
#define HALF_SCREEN_WIDTH (SCREEN_WIDTH / 2.f)
#define HALF_SCREEN_HEIGHT (SCREEN_HEIGHT / 2.f)

#define RATIO_X (gfx_current_dimensions.width / (float)SCREEN_WIDTH)
#define RATIO_Y (gfx_current_dimensions.height / (float)SCREEN_HEIGHT)

#define MAX_BUFFERED 256
#define MAX_LIGHTS 4
#define MAX_VERTICES 128
#define MAX_VERTEX_COLORS 64

#ifndef TEXTURE_CACHE_MAX_SIZE
#define TEXTURE_CACHE_MAX_SIZE 1024
#endif

#if defined(PLATFORM_PS2)
/*
 * N64 TMEM stores at most 4096 source bytes.  The widest expansion performed
 * by the compatibility importers is I4/CI4 -> RGBA32: 8192 texels, or 32 KiB.
 * Keep a second 32 KiB as an explicit guard margin without inheriting the
 * desktop backend's max_texture_dimension^2 allocation (4 MiB on GS).
 */
enum { PS2_TEXTURE_UPLOAD_SCRATCH_SIZE = 64 * 1024 };
static_assert(TEXTURE_CACHE_MAX_SIZE > 0, "PS2 texture cache must not be empty");
#endif

#define C0(pos, width) ((cmd->words.w0 >> (pos)) & ((1U << width) - 1))
#define C1(pos, width) ((cmd->words.w1 >> (pos)) & ((1U << width) - 1))

struct RGBA {
    uint8_t r, g, b, a;
};

struct NormalColor {
    union {
        struct { uint8_t r, g, b, a; };
        struct { int8_t x, y, z, w; };
    };
};

struct LoadedVertex {
    float x, y, z, w;
    float u, v;
    struct RGBA color;
    uint8_t fog;
    uint8_t clip_rej;
};

#if defined(PLATFORM_PS2)
enum { PS2_TEXTURE_CACHE_BUCKETS = 1024 };
static_assert((PS2_TEXTURE_CACHE_BUCKETS & (PS2_TEXTURE_CACHE_BUCKETS - 1)) == 0,
    "bucket count must be a power of two");
static_assert(TEXTURE_CACHE_MAX_SIZE < 65535, "node links are 16-bit");

static struct {
    TextureCacheNode nodes[TEXTURE_CACHE_MAX_SIZE];
    uint32_t free_texture_ids[TEXTURE_CACHE_MAX_SIZE];
    uint16_t buckets[PS2_TEXTURE_CACHE_BUCKETS]; // 1-based node index, 0 = empty
    size_t free_texture_id_count;
    size_t size;
    uint64_t lru_clock;
} gfx_texture_cache;

// Must agree with TextureCacheKey::operator==: hash exactly the compared fields.
static inline uint32_t ps2_texture_cache_bucket(const TextureCacheKey& key) {
    uint64_t h;
    if (key.content_identity_valid) {
        h = key.content_identity;
    } else {
        h = (uint64_t)(uintptr_t)key.texture_addr * UINT64_C(0x9E3779B97F4A7C15) ^
            (uint64_t)(uintptr_t)key.palette_addrs[0] * UINT64_C(0xC2B2AE3D27D4EB4F) ^
            (uint64_t)(uintptr_t)key.palette_addrs[1] * UINT64_C(0x165667B19E3779F9);
    }
    uint32_t x = (uint32_t)h ^ (uint32_t)(h >> 32) ^
        ((uint32_t)key.fmt << 3) ^ ((uint32_t)key.siz << 7) ^
        ((uint32_t)key.palette_index << 11) ^ (key.content_identity_valid ? 0x5bd1e995u : 0u);
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    return x & (PS2_TEXTURE_CACHE_BUCKETS - 1);
}

static void ps2_texture_cache_link(TextureCacheNode* node) {
    const uint32_t bucket = ps2_texture_cache_bucket(node->first);
    node->hash_next = gfx_texture_cache.buckets[bucket];
    gfx_texture_cache.buckets[bucket] = (uint16_t)(node - gfx_texture_cache.nodes + 1);
}

static void ps2_texture_cache_unlink(TextureCacheNode* node) {
    const uint16_t self = (uint16_t)(node - gfx_texture_cache.nodes + 1);
    uint16_t* link = &gfx_texture_cache.buckets[ps2_texture_cache_bucket(node->first)];
    while (*link != 0) {
        if (*link == self) {
            *link = node->hash_next;
            node->hash_next = 0;
            return;
        }
        link = &gfx_texture_cache.nodes[*link - 1].hash_next;
    }
    assert(!"texture cache node missing from its bucket");
}

// GS residency can be dropped under VRAM pressure while the frontend still
// owns the handle; such a node must be re-imported instead of bound.
extern "C" bool gfxPs2TextureResident(uint32_t texture_id);
#else
static struct {
    TextureCacheMap map;
    std::list<TextureCacheMapIter> lru;
    std::vector<uint32_t> free_texture_ids;
} gfx_texture_cache;
#endif

struct ColorCombiner {
    uint64_t shader_id0;
    uint32_t shader_id1;
    bool used_textures[2];
    struct ShaderProgram* prg[16];
    uint8_t shader_input_mapping[2][7];
};

#if defined(PLATFORM_PS2)
enum { PS2_COLOR_COMBINER_CACHE_SIZE = 256 };
static_assert(PS2_COLOR_COMBINER_CACHE_SIZE > 0, "PS2 combiner cache must not be empty");

struct Ps2ColorCombinerEntry {
    ColorCombinerKey key;
    struct ColorCombiner combiner;
    uint64_t lru_stamp;
    bool occupied;
};

static Ps2ColorCombinerEntry color_combiner_pool[PS2_COLOR_COMBINER_CACHE_SIZE];
static struct ColorCombiner* prev_combiner;
static ColorCombinerKey prev_combiner_key{};
static uint64_t color_combiner_lru_clock;
#else
static std::map<ColorCombinerKey, struct ColorCombiner> color_combiner_pool;
static std::map<ColorCombinerKey, struct ColorCombiner>::iterator prev_combiner = color_combiner_pool.end();
#endif

/*
 * u8 -> [0,1] float as `x / 255.0f`, precomputed. Filled at gfx_init with a
 * run-time division (volatile divisor) so every entry is bit-identical to
 * the inline division the VBO packer used before, on any FPU.
 */
static float gfx_u8_unit[256];

static void gfx_init_u8_unit_table(void) {
    volatile float divisor = 255.0f;
    for (int i = 0; i < 256; ++i) {
        gfx_u8_unit[i] = (float)i / divisor;
    }
}

static uint8_t* tex_upload_buffer = nullptr;
#if defined(PLATFORM_PS2)
static size_t tex_upload_buffer_size;

static void ps2_ensure_texture_upload_scratch(size_t source_size, size_t expansion) {
    if (expansion == 0 || source_size > SIZE_MAX / expansion) {
        abort();
    }
    const size_t required_size = source_size * expansion;

    if (required_size <= tex_upload_buffer_size) {
        return;
    }

    uint8_t* resized = (uint8_t*)realloc(tex_upload_buffer, required_size);
    if (resized == nullptr) {
        abort();
    }
    tex_upload_buffer = resized;
    tex_upload_buffer_size = required_size;
}
#endif

static struct RSP {
    float modelview_matrix_stack[11][4][4];
    uint8_t modelview_matrix_stack_size;

    float MP_matrix[4][4];
    float P_matrix[4][4];

    Light_t lookat[2];
    bool lookat_enabled;

    Light_t current_lights[MAX_LIGHTS + 1];
    float current_lights_coeffs[MAX_LIGHTS][3];
    float current_lookat_coeffs[2][3]; // lookat_x, lookat_y
    uint8_t current_num_lights;        // includes ambient light
    bool lights_changed;

    uint32_t geometry_mode;
    int16_t fog_mul, fog_offset;

    uint32_t extra_geometry_mode;

    uint32_t aspect_mode;
    float aspect_ofs;
    float aspect_scale;

    struct {
        // U0.16
        uint16_t s, t;
    } texture_scaling_factor;

    struct LoadedVertex loaded_vertices[MAX_VERTICES + 4];

    const struct NormalColor *vertex_colors; //[MAX_VERTEX_COLORS];
} rsp;

struct RawTexMetadata {
    uint16_t width, height;
    float h_byte_scale = 1, v_pixel_scale = 1;
};

struct LoadedTexture {
    const uint8_t* addr;
    uint32_t orig_size_bytes;
    uint32_t full_size_bytes; // full_image_line_size_bytes * height
    uint32_t size_bytes; // line_size_bytes * height
    uint32_t full_image_line_size_bytes;
    uint32_t line_size_bytes;
    uint32_t tex_flags;
    struct RawTexMetadata raw_tex_metadata;
};

static struct RDP {
    uint16_t palette[256];
    const uint8_t* palette_addrs[2];
    uint32_t palette_fmt;
    struct {
        const uint8_t* addr;
        uint8_t siz;
        uint32_t width;
        uint32_t tex_flags;
        struct RawTexMetadata raw_tex_metadata;
    } texture_to_load;
    struct {
        uint8_t fmt;
        uint8_t siz;
        uint8_t cms, cmt;
        uint8_t shifts, shiftt;
        uint16_t uls, ult, lrs, lrt; // U10.2
        uint16_t width, height;      // in texels
        uint16_t tmem;               // 0-511, in 64-bit word units
        uint32_t line_size_bytes;
        uint8_t palette;
    } texture_tile[8];
    LoadedTexture loaded_texture[512]; // for each tmem location
    bool textures_changed[2];

    uint8_t first_tile_index;
    uint8_t tex_min_lod;
    uint8_t tex_max_lod;

    uint32_t other_mode_l, other_mode_h;
    uint64_t combine_mode;
    bool grayscale;
    bool tex_lod;
    bool tex_detail;

    uint8_t prim_lod_fraction;
    uint16_t primitive_depth_z;
    uint16_t primitive_depth_dz;
    struct RGBA env_color, prim_color, fog_color, fill_color, grayscale_color;
    struct XYWidthHeight viewport, scissor;
    bool viewport_or_scissor_changed;
    void* z_buf_address;
    void* color_image_address;

    int16_t subpixel_ofs_x;
    int16_t subpixel_ofs_y;
} rdp;

static struct RenderingState {
    uint8_t depth_mode;
    bool alpha_blend;
    bool modulate;
    struct XYWidthHeight viewport, scissor;
    struct ShaderProgram* shader_program;
    TextureCacheNode* textures[SHADER_MAX_TEXTURES];
} rendering_state;

struct GfxDimensions gfx_current_window_dimensions;
int32_t gfx_current_window_position_x;
int32_t gfx_current_window_position_y;
struct GfxDimensions gfx_current_dimensions;
static struct GfxDimensions gfx_prev_dimensions;
struct XYWidthHeight gfx_current_game_window_viewport;
struct XYWidthHeight gfx_current_native_viewport;
float gfx_current_native_aspect = 4.f / 3.f;
bool gfx_framebuffers_enabled = true;
bool gfx_detail_textures_enabled = true;

static bool game_renders_to_framebuffer;
static int game_framebuffer;
static int game_framebuffer_msaa_resolved;

uint32_t gfx_msaa_level = 1;

static bool dropped_frame;

static float buf_vbo[MAX_BUFFERED * (32 * 3)]; // 3 vertices in a triangle and 32 floats per vtx
static size_t buf_vbo_len;
static size_t buf_vbo_num_tris;

static struct GfxWindowManagerAPI* gfx_wapi;
static struct GfxRenderingAPI* gfx_rapi;

static uintptr_t segmentPointers[16];

struct FBInfo {
    uint32_t orig_width, orig_height;
    uint32_t applied_width, applied_height;
    bool upscale, autoresize;
};

static bool fbActive = 0;
#if defined(PLATFORM_PS2)
enum { PS2_FRAMEBUFFER_INFO_COUNT = 16 };
static_assert(PS2_FRAMEBUFFER_INFO_COUNT > 0, "PS2 framebuffer table must not be empty");

struct Ps2FramebufferInfo {
    int id;
    FBInfo info;
    bool occupied;
};

static Ps2FramebufferInfo framebuffers[PS2_FRAMEBUFFER_INFO_COUNT];
#else
static std::map<int, FBInfo>::iterator active_fb;
static std::map<int, FBInfo> framebuffers;
#endif

static constexpr float clampf(const float x, const float min, const float max) {
    return (x < min) ? min : (x > max) ? max : x;
}

#if defined(PLATFORM_PS2) && defined(PERFECT_DARK_PS2_VU1_COLOR_BATCH)
#define GFX_PS2_DIRECT 1
#include <cfloat>
#include "gs_clip.h"
#include "gs_vu1_transform.h"

enum { GFX_PS2_DIRECT_MAX_VERTICES = 81 }; /* PS2_GS_VU1_MAX_TEXTURED_VERTICES */

struct GfxPs2DirectKey {
    struct GfxPs2DirectContext ctx;
    uint8_t z_is_from_0_to_1;
    uint8_t invert_y;
    uint8_t in_kind[2][3];
    uint8_t in_const_u8[3][4];
    float u_scale, u_bias, v_scale, v_bias;
};

#if defined(PD_PS2_DIRECT_VERIFY)
extern "C" {
struct Ps2GsVu1TransformVertex gfx_ps2_direct_verify_records[MAX_BUFFERED * 3];
uint8_t gfx_ps2_direct_verify_valid[MAX_BUFFERED];
const float* gfx_ps2_direct_verify_vbo;
}
#endif

static struct {
    bool active;
    uint32_t serial; /* bumped whenever the key changes; 0 = never valid */
    struct GfxPs2DirectKey key;
    uint32_t tag[MAX_VERTICES];
    uint32_t codes[MAX_VERTICES];
    bool w_ok[MAX_VERTICES];
    struct Ps2GsVu1TransformVertex records[MAX_VERTICES] __attribute__((aligned(16)));
    struct Ps2GsVu1TransformVertex batch[GFX_PS2_DIRECT_MAX_VERTICES] __attribute__((aligned(16)));
    uint32_t batch_count;
    bool batch_trilerp;
    bool batch_w_ok;
    uint8_t batch_fog[3];
    /* Per-key record colour program: channel c of a record is
     * rec_const[c] when rec_src[c] == 0xff, else rec_lut[c][bank[rec_src[c]]]
     * with bank = { r, g, b, a, lod, 0 } of the vertex. */
    uint8_t rec_src[4];
    uint8_t rec_const[4];
    const uint8_t* rec_lut[4];
    bool rec_lod;
} gfx_ps2_direct;

static void gfx_ps2_at_flush(void);
static void gfx_ps2_direct_flush_records(void);
static inline void gfx_ps2_at_invalidate_vertices(size_t first, size_t count);

/*
 * Object-space geometry (gfx_ps2_object.h). A G_VTX block whose bounding box
 * is inside the guard band under the current matrix is not transformed: its
 * vertices stay in object space, triangles over them are culled in object
 * space and drawn by VU1's object program with the block's clip matrix. Any
 * other use of such a slot first replays the original G_VTX for it
 * (gfx_obj_materialize), so every other path sees exactly what it used to.
 */
#include "gfx_ps2_object.h"
enum {
    GFX_OBJ_CTX = 32,
    GFX_OBJ_MAX_VERTICES = PS2_GS_VU1_OBJECT_MAX_VERTICES,
    GFX_OBJ_NORMAL = 0, /* loaded_vertices[] is current */
    GFX_OBJ_INSIDE = 1, /* deferred, block inside the guard band */
    GFX_OBJ_OUTSIDE = 2, /* deferred, block outside the planes in mask[] */
};

/* Everything gfx_sp_vertex reads for an unlit load, plus derived data. */
struct GfxObjCtx {
    float mp[4][4] __attribute__((aligned(16)));
    bool fb_active;
    float aspect_ofs, aspect_scale, aspect_ratio;
    uint32_t geometry_mode;
    uint16_t tex_s, tex_t;
    int16_t fog_mul, fog_offset;
    uint8_t fog_alpha;
    uint8_t z_is_from_0_to_1, invert_y;
    struct XYWidthHeight viewport;
    float guard_x, guard_y;
    struct Ps2GsVu1ObjectTransform xf __attribute__((aligned(16)));
    float alpha[4];
    /* box tests as object-space planes (gfxPs2ObjectBuildPlanes) */
    float pl[4][PS2_OBJECT_PLANES];
    float pa[3][PS2_OBJECT_PLANES];
    /* the same, as three groups of four planes for VU0: x, y, z, constant,
     * |x|, |y|, |z| rows */
    float vpl[3][7][4] __attribute__((aligned(16)));
};

static struct {
    bool enabled;
    bool materializing;
    int8_t fog_linear; /* -1 unknown */
    int batch_ctx;     /* ctx of the buffered direct batch, -1: clip space */
    uint32_t next_ctx;
    int last_ctx;
    uint8_t state[MAX_VERTICES];
    uint8_t mask[MAX_VERTICES];
    uint8_t ctx[MAX_VERTICES];
    /* The display list is complete and unchanging while gfx_run walks it,
     * so a deferred slot keeps pointers instead of copies. */
    const Vtx* src[MAX_VERTICES];
    const struct NormalColor* cols[MAX_VERTICES];
    struct GfxObjCtx ctxs[GFX_OBJ_CTX];
    /* constants of the buffered object batch, and the record program of the
     * current direct key (gfx_obj_update_prog) */
    struct Ps2GsVu1ObjectTransform batch_xf __attribute__((aligned(16)));
    uint32_t prog_serial;
    bool prog_ok;
    /* whether the last triangle setup could take object batches: G_VTX only
     * defers while it could, so states that end up on the VBO path do not
     * pay a replay for every block */
    bool setup_ok;
    struct Ps2GsVu1ObjectTransform prog __attribute__((aligned(16)));
    /* per-frame counters (PD_PS2_OBJECT_STATS) */
    uint32_t n_inside, n_outside, n_mixed, n_tris, n_replays;
#if defined(PD_PS2_DIRECT_VERIFY) || defined(PD_PS2_NO_OBJECT_GEOMETRY)
} gfx_obj = { false, false, -1, -1, 0u, -1, {}, {}, {}, {}, {}, {}, {}, 0u, false, {}, true };
#else
} gfx_obj = { true, false, -1, -1, 0u, -1, {}, {}, {}, {}, {}, {}, {}, 0u, false, {}, true };
#endif

static bool gfx_obj_try_defer(size_t n_vertices, size_t dest_index, const Vtx* vertices);
static void gfx_obj_materialize_ctx(int k);

static void gfx_ps2_direct_flush(void) {
    gfx_ps2_at_flush();
    gfx_ps2_direct_flush_records();
}

static void gfx_ps2_direct_flush_records(void) {
    if (gfx_ps2_direct.batch_count == 0) {
        return;
    }
    if (gfx_obj.batch_ctx >= 0) {
        gfxPs2DirectDrawObject((const struct Ps2GsVu1ObjectVertex*)gfx_ps2_direct.batch, gfx_ps2_direct.batch_count,
                               gfx_ps2_direct.batch_trilerp, gfx_ps2_direct.batch_fog, &gfx_obj.batch_xf);
    } else {
        gfxPs2DirectDraw(gfx_ps2_direct.batch, gfx_ps2_direct.batch_count,
                         gfx_ps2_direct.batch_trilerp, gfx_ps2_direct.batch_w_ok, gfx_ps2_direct.batch_fog);
    }
    gfx_ps2_direct.batch_count = 0;
    gfx_obj.batch_ctx = -1;
}

static inline void gfx_ps2_direct_invalidate_vertices(size_t first, size_t count) {
    if (first >= MAX_VERTICES) {
        return;
    }
    if (count > MAX_VERTICES - first) {
        count = MAX_VERTICES - first;
    }
    /* Plain word stores: newlib's memset takes its byte loop for these
     * short, odd-sized ranges, and this runs on every G_VTX. */
    uint32_t* direct_tag = &gfx_ps2_direct.tag[first];
    for (size_t i = 0; i < count; ++i) {
        direct_tag[i] = 0;
    }
    gfx_ps2_at_invalidate_vertices(first, count);
}

#endif

static void gfx_flush(void) {
#if defined(GFX_PS2_DIRECT)
    gfx_ps2_direct_flush();
#endif
    if (buf_vbo_len > 0) {
        gfx_rapi->draw_triangles(buf_vbo, buf_vbo_len, buf_vbo_num_tris);
        buf_vbo_len = 0;
        buf_vbo_num_tris = 0;
    }
}

static struct ShaderProgram* gfx_lookup_or_create_shader_program(uint64_t shader_id0, uint32_t shader_id1) {
    struct ShaderProgram* prg = gfx_rapi->lookup_shader(shader_id0, shader_id1);
    if (prg == NULL) {
        gfx_rapi->unload_shader(rendering_state.shader_program);
        prg = gfx_rapi->create_and_load_new_shader(shader_id0, shader_id1);
        rendering_state.shader_program = prg;
    }
    return prg;
}

static const char* ccmux_to_string(uint32_t ccmux) {
    static const char* const tbl[] = {
        "G_CCMUX_COMBINED",
        "G_CCMUX_TEXEL0",
        "G_CCMUX_TEXEL1",
        "G_CCMUX_PRIMITIVE",
        "G_CCMUX_SHADE",
        "G_CCMUX_ENVIRONMENT",
        "G_CCMUX_1",
        "G_CCMUX_COMBINED_ALPHA",
        "G_CCMUX_TEXEL0_ALPHA",
        "G_CCMUX_TEXEL1_ALPHA",
        "G_CCMUX_PRIMITIVE_ALPHA",
        "G_CCMUX_SHADE_ALPHA",
        "G_CCMUX_ENV_ALPHA",
        "G_CCMUX_LOD_FRACTION",
        "G_CCMUX_PRIM_LOD_FRAC",
        "G_CCMUX_K5",
    };
    if (ccmux > 15) {
        return "G_CCMUX_0";

    } else {
        return tbl[ccmux];
    }
}

static const char* acmux_to_string(uint32_t acmux) {
    static const char* const tbl[] = {
        "G_ACMUX_COMBINED or G_ACMUX_LOD_FRACTION",
        "G_ACMUX_TEXEL0",
        "G_ACMUX_TEXEL1",
        "G_ACMUX_PRIMITIVE",
        "G_ACMUX_SHADE",
        "G_ACMUX_ENVIRONMENT",
        "G_ACMUX_1 or G_ACMUX_PRIM_LOD_FRAC",
        "G_ACMUX_0",
    };
    return tbl[acmux];
}

static void gfx_generate_cc(struct ColorCombiner* comb, const ColorCombinerKey& key) {
    bool is_2cyc = (key.options & (uint64_t)SHADER_OPT_2CYC) != 0;

    uint8_t c[2][2][4] = { { { 0 } } };
    uint64_t shader_id0 = 0;
    uint32_t shader_id1 = key.options;
    uint8_t shader_input_mapping[2][7] = { { 0 } };
    bool used_textures[2] = { false, false };
    for (int i = 0; i < 2 && (i == 0 || is_2cyc); i++) {
        uint32_t rgb_a = (key.combine_mode >> (i * 28)) & 0xf;
        uint32_t rgb_b = (key.combine_mode >> (i * 28 + 4)) & 0xf;
        uint32_t rgb_c = (key.combine_mode >> (i * 28 + 8)) & 0x1f;
        uint32_t rgb_d = (key.combine_mode >> (i * 28 + 13)) & 7;
        uint32_t alpha_a = (key.combine_mode >> (i * 28 + 16)) & 7;
        uint32_t alpha_b = (key.combine_mode >> (i * 28 + 16 + 3)) & 7;
        uint32_t alpha_c = (key.combine_mode >> (i * 28 + 16 + 6)) & 7;
        uint32_t alpha_d = (key.combine_mode >> (i * 28 + 16 + 9)) & 7;

        if (rgb_a >= 8) {
            rgb_a = G_CCMUX_0;
        }
        if (rgb_b >= 8) {
            rgb_b = G_CCMUX_0;
        }
        if (rgb_c >= 16) {
            rgb_c = G_CCMUX_0;
        }
        if (rgb_d == 7) {
            rgb_d = G_CCMUX_0;
        }

        if (rgb_a == rgb_b || rgb_c == G_CCMUX_0) {
            // Normalize
            rgb_a = G_CCMUX_0;
            rgb_b = G_CCMUX_0;
            rgb_c = G_CCMUX_0;
        }
        if (alpha_a == alpha_b || alpha_c == G_ACMUX_0) {
            // Normalize
            alpha_a = G_ACMUX_0;
            alpha_b = G_ACMUX_0;
            alpha_c = G_ACMUX_0;
        }
        if (i == 1) {
            if (rgb_a != G_CCMUX_COMBINED && rgb_b != G_CCMUX_COMBINED && rgb_c != G_CCMUX_COMBINED &&
                rgb_d != G_CCMUX_COMBINED) {
                // First cycle RGB not used, so clear it away
                c[0][0][0] = c[0][0][1] = c[0][0][2] = c[0][0][3] = G_CCMUX_0;
            }
            if (rgb_c != G_CCMUX_COMBINED_ALPHA && alpha_a != G_ACMUX_COMBINED && alpha_b != G_ACMUX_COMBINED &&
                alpha_d != G_ACMUX_COMBINED) {
                // First cycle ALPHA not used, so clear it away
                c[0][1][0] = c[0][1][1] = c[0][1][2] = c[0][1][3] = G_ACMUX_0;
            }
        }

        c[i][0][0] = rgb_a;
        c[i][0][1] = rgb_b;
        c[i][0][2] = rgb_c;
        c[i][0][3] = rgb_d;
        c[i][1][0] = alpha_a;
        c[i][1][1] = alpha_b;
        c[i][1][2] = alpha_c;
        c[i][1][3] = alpha_d;
    }
    if (!is_2cyc) {
        for (int i = 0; i < 2; i++) {
            for (int k = 0; k < 4; k++) {
                c[1][i][k] = i == 0 ? G_CCMUX_0 : G_ACMUX_0;
            }
        }
    }
    {
        uint8_t input_number[32] = { 0 };
        int next_input_number = SHADER_INPUT_1;
        for (int i = 0; i < 2 && (i == 0 || is_2cyc); i++) {
            for (int j = 0; j < 4; j++) {
                uint32_t val = 0;
                switch (c[i][0][j]) {
                    case G_CCMUX_0:
                        val = SHADER_0;
                        break;
                    case G_CCMUX_1:
                        val = SHADER_1;
                        break;
                    case G_CCMUX_TEXEL0:
                        val = SHADER_TEXEL0;
                        used_textures[0] = true;
                        break;
                    case G_CCMUX_TEXEL1:
                        val = SHADER_TEXEL1;
                        used_textures[1] = true;
                        break;
                    case G_CCMUX_TEXEL0_ALPHA:
                        val = SHADER_TEXEL0A;
                        used_textures[0] = true;
                        break;
                    case G_CCMUX_TEXEL1_ALPHA:
                        val = SHADER_TEXEL1A;
                        used_textures[1] = true;
                        break;
                    case G_CCMUX_NOISE:
                        val = SHADER_NOISE;
                        break;
                    case G_CCMUX_PRIMITIVE:
                    case G_CCMUX_PRIMITIVE_ALPHA:
                    case G_CCMUX_PRIM_LOD_FRAC:
                    case G_CCMUX_SHADE:
                    case G_CCMUX_SHADE_ALPHA:
                    case G_CCMUX_ENVIRONMENT:
                    case G_CCMUX_ENV_ALPHA:
                    case G_CCMUX_LOD_FRACTION:
                        if (input_number[c[i][0][j]] == 0) {
                            shader_input_mapping[0][next_input_number - 1] = c[i][0][j];
                            input_number[c[i][0][j]] = next_input_number++;
                        }
                        val = input_number[c[i][0][j]];
                        break;
                    case G_CCMUX_COMBINED:
                        val = SHADER_COMBINED;
                        break;
                    default:
                        sysLogPrintf(LOG_WARNING, "Unsupported ccmux: %d", c[i][0][j]);
                        break;
                }
                shader_id0 |= (uint64_t)val << (i * 32 + j * 4);
            }
        }
    }
    {
        uint8_t input_number[16] = { 0 };
        int next_input_number = SHADER_INPUT_1;
        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 4; j++) {
                uint32_t val = 0;
                switch (c[i][1][j]) {
                    case G_ACMUX_0:
                        val = SHADER_0;
                        break;
                    case G_ACMUX_TEXEL0:
                        val = SHADER_TEXEL0;
                        used_textures[0] = true;
                        break;
                    case G_ACMUX_TEXEL1:
                        val = SHADER_TEXEL1;
                        used_textures[1] = true;
                        break;
                    case G_ACMUX_LOD_FRACTION:
                        // case G_ACMUX_COMBINED: same numerical value
                        if (j != 2) {
                            val = SHADER_COMBINED;
                            break;
                        }
                        c[i][1][j] = G_CCMUX_LOD_FRACTION;
                        [[fallthrough]]; // for G_ACMUX_LOD_FRACTION
                    case G_ACMUX_1:
                        // case G_ACMUX_PRIM_LOD_FRAC: same numerical value
                        if (j != 2) {
                            val = SHADER_1;
                            break;
                        }
                        [[fallthrough]]; // for G_ACMUX_PRIM_LOD_FRAC
                    case G_ACMUX_PRIMITIVE:
                    case G_ACMUX_SHADE:
                    case G_ACMUX_ENVIRONMENT:
                        if (input_number[c[i][1][j]] == 0) {
                            shader_input_mapping[1][next_input_number - 1] = c[i][1][j];
                            input_number[c[i][1][j]] = next_input_number++;
                        }
                        val = input_number[c[i][1][j]];
                        break;
                }
                shader_id0 |= (uint64_t)val << (i * 32 + 16 + j * 4);
            }
        }
    }
    comb->shader_id0 = shader_id0;
    comb->shader_id1 = shader_id1;
    comb->used_textures[0] = used_textures[0];
    comb->used_textures[1] = used_textures[1];
    // comb->prg = gfx_lookup_or_create_shader_program(shader_id0, shader_id1);
    memcpy(comb->shader_input_mapping, shader_input_mapping, sizeof(shader_input_mapping));
}

static struct ColorCombiner* gfx_lookup_or_create_color_combiner(const ColorCombinerKey& key) {
#if defined(PLATFORM_PS2)
    if (prev_combiner != nullptr && prev_combiner_key == key) {
        return prev_combiner;
    }

    Ps2ColorCombinerEntry* free_entry = nullptr;
    Ps2ColorCombinerEntry* oldest_entry = nullptr;

    for (size_t i = 0; i < PS2_COLOR_COMBINER_CACHE_SIZE; ++i) {
        Ps2ColorCombinerEntry* entry = &color_combiner_pool[i];

        if (entry->occupied) {
            if (entry->key == key) {
                entry->lru_stamp = ++color_combiner_lru_clock;
                prev_combiner_key = key;
                prev_combiner = &entry->combiner;
                return prev_combiner;
            }

            if (oldest_entry == nullptr || entry->lru_stamp < oldest_entry->lru_stamp) {
                oldest_entry = entry;
            }
        } else if (free_entry == nullptr) {
            free_entry = entry;
        }
    }

    gfx_flush();
    Ps2ColorCombinerEntry* entry = free_entry != nullptr ? free_entry : oldest_entry;
    assert(entry != nullptr);
    entry->key = key;
    entry->combiner = {};
    entry->lru_stamp = ++color_combiner_lru_clock;
    entry->occupied = true;
    gfx_generate_cc(&entry->combiner, key);
    prev_combiner_key = key;
    prev_combiner = &entry->combiner;
    return prev_combiner;
#else
    if (prev_combiner != color_combiner_pool.end() && prev_combiner->first == key) {
        return &prev_combiner->second;
    }

    prev_combiner = color_combiner_pool.find(key);
    if (prev_combiner != color_combiner_pool.end()) {
        return &prev_combiner->second;
    }
    gfx_flush();
    prev_combiner = color_combiner_pool.insert(std::make_pair(key, ColorCombiner())).first;
    gfx_generate_cc(&prev_combiner->second, key);
    return &prev_combiner->second;
#endif
}

void gfx_texture_cache_clear() {
    gfx_flush();
#if defined(PLATFORM_PS2)
    for (size_t i = 0; i < TEXTURE_CACHE_MAX_SIZE; ++i) {
        TextureCacheNode* node = &gfx_texture_cache.nodes[i];

        if (node->occupied) {
            assert(gfx_texture_cache.free_texture_id_count < TEXTURE_CACHE_MAX_SIZE);
            gfx_texture_cache.free_texture_ids[gfx_texture_cache.free_texture_id_count++] =
                node->second.texture_id;
            node->occupied = false;
            node->hash_next = 0;
        }
    }
    memset(gfx_texture_cache.buckets, 0, sizeof(gfx_texture_cache.buckets));
    gfx_texture_cache.size = 0;
#else
    for (const auto& entry : gfx_texture_cache.map) {
        gfx_texture_cache.free_texture_ids.push_back(entry.second.texture_id);
    }
    gfx_texture_cache.map.clear();
    gfx_texture_cache.lru.clear();
#endif
    rdp.textures_changed[0] = rdp.textures_changed[1] = true;
    memset(rendering_state.textures, 0, sizeof(rendering_state.textures));
}

static bool gfx_texture_cache_lookup(int i, const TextureCacheKey& key) {
#if defined(PLATFORM_PS2)
    TextureCacheNode** selected = &rendering_state.textures[i];

    for (uint16_t link = gfx_texture_cache.buckets[ps2_texture_cache_bucket(key)];
         link != 0; link = gfx_texture_cache.nodes[link - 1].hash_next) {
        TextureCacheNode* node = &gfx_texture_cache.nodes[link - 1];
        if (node->first == key) {
            node->lru_stamp = ++gfx_texture_cache.lru_clock;
            if (!gfxPs2TextureResident(node->second.texture_id)) {
                // Evicted from GS VRAM: keep node and handle, re-import.
                node->second = TextureCacheValue{ node->second.texture_id };
                gfx_rapi->select_texture(i, node->second.texture_id, false);
                gfx_rapi->set_sampler_parameters(i, false, 0, 0, rdp.tex_lod);
                *selected = node;
                return false;
            }
            gfx_rapi->select_texture(i, node->second.texture_id, node->second.linear_filter);
            *selected = node;
            return true;
        }
    }

    // Miss: only now pay for a full scan to find a free or LRU node.
    TextureCacheNode* free_node = nullptr;
    TextureCacheNode* oldest_node = nullptr;
    for (size_t index = 0; index < TEXTURE_CACHE_MAX_SIZE; ++index) {
        TextureCacheNode* node = &gfx_texture_cache.nodes[index];

        if (node->occupied) {
            if (oldest_node == nullptr || node->lru_stamp < oldest_node->lru_stamp) {
                oldest_node = node;
            }
        } else if (free_node == nullptr) {
            free_node = node;
            break;
        }
    }

    TextureCacheNode* node = free_node;
    uint32_t texture_id;

    if (node == nullptr) {
        node = oldest_node;
        assert(node != nullptr);
        texture_id = node->second.texture_id;
        ps2_texture_cache_unlink(node);

        for (int slot = 0; slot < SHADER_MAX_TEXTURES; ++slot) {
            if (rendering_state.textures[slot] == node) {
                rendering_state.textures[slot] = nullptr;
                rdp.textures_changed[slot] = true;
            }
        }
    } else {
        ++gfx_texture_cache.size;

        if (gfx_texture_cache.free_texture_id_count != 0) {
            texture_id = gfx_texture_cache.free_texture_ids[--gfx_texture_cache.free_texture_id_count];
        } else {
            texture_id = gfx_rapi->new_texture();
        }
    }

    node->first = key;
    node->second = {};
    node->second.texture_id = texture_id;
    node->lru_stamp = ++gfx_texture_cache.lru_clock;
    node->occupied = true;
    ps2_texture_cache_link(node);

    gfx_rapi->select_texture(i, texture_id, false);
    gfx_rapi->set_sampler_parameters(i, false, 0, 0, rdp.tex_lod);
    *selected = node;
    return false;
#else
    TextureCacheMap::iterator it = gfx_texture_cache.map.find(key);
    TextureCacheNode** n = &rendering_state.textures[i];

    if (it != gfx_texture_cache.map.end()) {
        gfx_rapi->select_texture(i, it->second.texture_id, it->second.linear_filter);
        *n = &*it;
        gfx_texture_cache.lru.splice(gfx_texture_cache.lru.end(), gfx_texture_cache.lru,
                                     it->second.lru_location); // move to back
        return true;
    }

    if (gfx_texture_cache.map.size() >= TEXTURE_CACHE_MAX_SIZE) {
        // Remove the texture that was least recently used
        it = gfx_texture_cache.lru.front().it;
        gfx_texture_cache.free_texture_ids.push_back(it->second.texture_id);
        gfx_texture_cache.map.erase(it);
        gfx_texture_cache.lru.pop_front();
    }

    uint32_t texture_id;
    if (!gfx_texture_cache.free_texture_ids.empty()) {
        texture_id = gfx_texture_cache.free_texture_ids.back();
        gfx_texture_cache.free_texture_ids.pop_back();
    } else {
        texture_id = gfx_rapi->new_texture();
    }

    it = gfx_texture_cache.map.insert(std::make_pair(key, TextureCacheValue())).first;
    TextureCacheNode* node = &*it;
    node->second.texture_id = texture_id;
    node->second.lru_location = gfx_texture_cache.lru.insert(gfx_texture_cache.lru.end(), { it });

    gfx_rapi->select_texture(i, texture_id, false);
    gfx_rapi->set_sampler_parameters(i, false, 0, 0, rdp.tex_lod);
    *n = node;
    return false;
#endif
}

void gfx_texture_cache_delete(const uint8_t* orig_addr) {
    gfx_flush();

    for (int i = 0; i < 2; ++i) {
        if (rendering_state.textures[i] && rendering_state.textures[i]->first.texture_addr == orig_addr) {
            rdp.textures_changed[i] = true;
            rendering_state.textures[i] = nullptr;
        }
    }

#if defined(PLATFORM_PS2)
    for (size_t i = 0; i < TEXTURE_CACHE_MAX_SIZE; ++i) {
        TextureCacheNode* node = &gfx_texture_cache.nodes[i];

        if (node->occupied && !node->first.content_identity_valid &&
            node->first.texture_addr == orig_addr) {
            assert(gfx_texture_cache.free_texture_id_count < TEXTURE_CACHE_MAX_SIZE);
            gfx_texture_cache.free_texture_ids[gfx_texture_cache.free_texture_id_count++] =
                node->second.texture_id;
            ps2_texture_cache_unlink(node);
            node->occupied = false;
            --gfx_texture_cache.size;
        }
    }
#else
    while (gfx_texture_cache.map.bucket_count() > 0) {
        TextureCacheKey key = { orig_addr, { 0 }, 0, 0 }; // bucket index only depends on the address
        size_t bucket = gfx_texture_cache.map.bucket(key);
        bool again = false;
        for (auto it = gfx_texture_cache.map.begin(bucket); it != gfx_texture_cache.map.end(bucket); ++it) {
            if (it->first.texture_addr == orig_addr) {
                gfx_texture_cache.lru.erase(it->second.lru_location);
                gfx_texture_cache.free_texture_ids.push_back(it->second.texture_id);
                gfx_texture_cache.map.erase(it->first);
                again = true;
                break;
            }
        }
        if (!again) {
            break;
        }
    }
#endif
}

void gfx_texture_cache_delete_range(const uint8_t* start, const uint8_t* end) {
    gfx_flush();

    for (int i = 0; i < 2; ++i) {
        if (rendering_state.textures[i]
                && rendering_state.textures[i]->first.texture_addr >= start
                && rendering_state.textures[i]->first.texture_addr < end) {
            rdp.textures_changed[i] = true;
            rendering_state.textures[i] = nullptr;
        }
    }

#if defined(PLATFORM_PS2)
    for (size_t i = 0; i < TEXTURE_CACHE_MAX_SIZE; ++i) {
        TextureCacheNode* node = &gfx_texture_cache.nodes[i];

        if (node->occupied && !node->first.content_identity_valid &&
            node->first.texture_addr >= start && node->first.texture_addr < end) {
            assert(gfx_texture_cache.free_texture_id_count < TEXTURE_CACHE_MAX_SIZE);
            gfx_texture_cache.free_texture_ids[gfx_texture_cache.free_texture_id_count++] =
                node->second.texture_id;
            ps2_texture_cache_unlink(node);
            node->occupied = false;
            --gfx_texture_cache.size;
        }
    }
#else
    for (auto it = gfx_texture_cache.map.begin(); it != gfx_texture_cache.map.end(); ) {
        if (it->first.texture_addr >= start && it->first.texture_addr < end) {
            gfx_texture_cache.lru.erase(it->second.lru_location);
            gfx_texture_cache.free_texture_ids.push_back(it->second.texture_id);
            it = gfx_texture_cache.map.erase(it);
        } else {
            ++it;
        }
    }
#endif
}

static void import_texture_rgba16(int tile, const LoadedTexture& loaded_texture, bool gen_mipmaps) {
    const uint8_t* addr = loaded_texture.addr;
    const uint32_t size_bytes = loaded_texture.size_bytes;
    const uint32_t full_image_line_size_bytes =
        loaded_texture.full_image_line_size_bytes;
    const uint32_t line_size_bytes = loaded_texture.line_size_bytes;
    // SUPPORT_CHECK(full_image_line_size_bytes == line_size_bytes);
    // TODO: this trips in some places with a garbage size in full_image_line_size_bytes
    // probably wherever framebuffer effects are used

#if defined(PLATFORM_PS2)
    ps2_ensure_texture_upload_scratch(size_bytes, 2u);
#endif

    uint8_t *dest = tex_upload_buffer;
    for (uint32_t i = 0; i < size_bytes / 2; i++, dest += 4) {
        const uint16_t col16 = (addr[2 * i] << 8) | addr[2 * i + 1];
        const uint8_t a = col16 & 1;
        const uint8_t r = col16 >> 11;
        const uint8_t g = (col16 >> 6) & 0x1f;
        const uint8_t b = (col16 >> 1) & 0x1f;
        dest[0] = SCALE_5_8(r);
        dest[1] = SCALE_5_8(g);
        dest[2] = SCALE_5_8(b);
        dest[3] = a ? 255 : 0;
    }

    const uint32_t width = rdp.texture_tile[tile].line_size_bytes / 2;
    const uint32_t height = size_bytes / rdp.texture_tile[tile].line_size_bytes;

	gfx_rapi->upload_texture(tex_upload_buffer, width, height, gen_mipmaps);
    // DumpTexture(loaded_texture.otr_path, rgba32_buf, width, height);
}

static void import_texture_rgba32(int tile, const LoadedTexture& loaded_texture, bool gen_mipmaps) {
    const RawTexMetadata* metadata = &loaded_texture.raw_tex_metadata;
    const uint8_t* addr = loaded_texture.addr;
    const uint32_t size_bytes = loaded_texture.size_bytes;
    const uint32_t full_image_line_size_bytes =
        loaded_texture.full_image_line_size_bytes;
    const uint32_t line_size_bytes = loaded_texture.line_size_bytes;
    SUPPORT_CHECK(full_image_line_size_bytes == line_size_bytes);

#if defined(PLATFORM_PS2)
    ps2_ensure_texture_upload_scratch(size_bytes, 1u);
#endif

    uint32_t *dest = (uint32_t *)tex_upload_buffer;
    const uint32_t *src = (const uint32_t *)addr;
    for (uint32_t i = 0; i < size_bytes; i += 4, ++dest, ++src) {
        *dest = PD_BE32(*src);
    }

    const uint32_t width = rdp.texture_tile[tile].line_size_bytes / 2;
    const uint32_t height = (size_bytes / 2) / rdp.texture_tile[tile].line_size_bytes;
	gfx_rapi->upload_texture(tex_upload_buffer, width, height, gen_mipmaps);
    // DumpTexture(loaded_texture.otr_path, addr, width, height);
}

static void import_texture_ia4(int tile, const LoadedTexture& loaded_texture, bool gen_mipmaps) {
    const RawTexMetadata* metadata = &loaded_texture.raw_tex_metadata;
    const uint8_t* addr = loaded_texture.addr;
    const uint32_t size_bytes = loaded_texture.size_bytes;
    const uint32_t full_image_line_size_bytes =
        loaded_texture.full_image_line_size_bytes;
    const uint32_t line_size_bytes = loaded_texture.line_size_bytes;
    SUPPORT_CHECK(full_image_line_size_bytes == line_size_bytes);

#if defined(PLATFORM_PS2)
    ps2_ensure_texture_upload_scratch(size_bytes, 8u);
#endif

    uint8_t *dest = tex_upload_buffer;
    for (uint32_t i = 0; i < size_bytes * 2; i++, dest += 4) {
        const uint8_t byte = addr[i / 2];
        const uint8_t part = (byte >> (4 - (i % 2) * 4)) & 0xf;
        const uint8_t intensity = part >> 1;
        const uint8_t alpha = part & 1;
        const uint8_t c = SCALE_3_8(intensity);
        dest[0] = c;
        dest[1] = c;
        dest[2] = c;
        dest[3] = alpha ? 255 : 0;
    }

    const uint32_t width = rdp.texture_tile[tile].line_size_bytes * 2;
    const uint32_t height = size_bytes / rdp.texture_tile[tile].line_size_bytes;

	gfx_rapi->upload_texture(tex_upload_buffer, width, height, gen_mipmaps);
    // DumpTexture(loaded_texture.otr_path, rgba32_buf, width, height);
}

static void import_texture_ia8(int tile, const LoadedTexture& loaded_texture, bool gen_mipmaps) {
    const RawTexMetadata* metadata = &loaded_texture.raw_tex_metadata;
    const uint8_t* addr = loaded_texture.addr;
    const uint32_t size_bytes = loaded_texture.size_bytes;
    const uint32_t full_image_line_size_bytes =
        loaded_texture.full_image_line_size_bytes;
    const uint32_t line_size_bytes = loaded_texture.line_size_bytes;
    SUPPORT_CHECK(full_image_line_size_bytes == line_size_bytes);

#if defined(PLATFORM_PS2)
    ps2_ensure_texture_upload_scratch(size_bytes, 4u);
#endif

    uint8_t *dest = tex_upload_buffer;
    for (uint32_t i = 0; i < size_bytes; i++, dest += 4) {
        const uint8_t intensity = SCALE_4_8(addr[i] >> 4);
        const uint8_t alpha = SCALE_4_8(addr[i] & 0xf);
        dest[0] = intensity;
        dest[1] = intensity;
        dest[2] = intensity;
        dest[3] = alpha;
    }

    const uint32_t width = rdp.texture_tile[tile].line_size_bytes;
    const uint32_t height = size_bytes / rdp.texture_tile[tile].line_size_bytes;

	gfx_rapi->upload_texture(tex_upload_buffer, width, height, gen_mipmaps);
    // DumpTexture(loaded_texture.otr_path, rgba32_buf, width, height);
}

static void import_texture_ia16(int tile, const LoadedTexture& loaded_texture, bool gen_mipmaps) {
    const RawTexMetadata* metadata = &loaded_texture.raw_tex_metadata;
    const uint8_t* addr = loaded_texture.addr;
    const uint32_t size_bytes = loaded_texture.size_bytes;
    const uint32_t full_image_line_size_bytes =
        loaded_texture.full_image_line_size_bytes;
    const uint32_t line_size_bytes = loaded_texture.line_size_bytes;
    SUPPORT_CHECK(full_image_line_size_bytes == line_size_bytes);

#if defined(PLATFORM_PS2)
    ps2_ensure_texture_upload_scratch(size_bytes, 2u);
#endif

    uint8_t *dest = tex_upload_buffer;
    for (uint32_t i = 0; i < size_bytes / 2; i++, dest += 4) {
        const uint8_t intensity = addr[2 * i];
        const uint8_t alpha = addr[2 * i + 1];
        dest[0] = intensity;
        dest[1] = intensity;
        dest[2] = intensity;
        dest[3] = alpha;
    }

    const uint32_t width = rdp.texture_tile[tile].line_size_bytes / 2;
    const uint32_t height = size_bytes / rdp.texture_tile[tile].line_size_bytes;

	gfx_rapi->upload_texture(tex_upload_buffer, width, height, gen_mipmaps);
    // DumpTexture(loaded_texture.otr_path, rgba32_buf, width, height);
}

static void import_texture_i4(int tile, const LoadedTexture& loaded_texture, bool gen_mipmaps) {
    const RawTexMetadata* metadata = &loaded_texture.raw_tex_metadata;
    const uint8_t* addr = loaded_texture.addr;
    const uint32_t size_bytes = loaded_texture.size_bytes;
    const uint32_t full_image_line_size_bytes =
        loaded_texture.full_image_line_size_bytes;
    const uint32_t line_size_bytes = loaded_texture.line_size_bytes;
    SUPPORT_CHECK(full_image_line_size_bytes == line_size_bytes);

#if defined(PLATFORM_PS2)
    ps2_ensure_texture_upload_scratch(size_bytes, 8u);
#endif

    uint8_t *dest = tex_upload_buffer;
    for (uint32_t i = 0; i < size_bytes * 2; i++, dest += 4) {
        const uint8_t byte = addr[i / 2];
        const uint8_t part = (byte >> (4 - (i % 2) * 4)) & 0xf;
        const uint8_t intensity = SCALE_4_8(part);
        dest[0] = intensity;
        dest[1] = intensity;
        dest[2] = intensity;
        dest[3] = intensity;
    }

    const uint32_t width = rdp.texture_tile[tile].line_size_bytes * 2;
    const uint32_t height = size_bytes / rdp.texture_tile[tile].line_size_bytes;

	gfx_rapi->upload_texture(tex_upload_buffer, width, height, gen_mipmaps);
    // DumpTexture(loaded_texture.otr_path, rgba32_buf, width, height);
}

static void import_texture_i8(int tile, const LoadedTexture& loaded_texture, bool gen_mipmaps) {
    const RawTexMetadata* metadata = &loaded_texture.raw_tex_metadata;
    const uint8_t* addr = loaded_texture.addr;
    const uint32_t size_bytes = loaded_texture.size_bytes;
    uint32_t full_image_line_size_bytes =
        loaded_texture.full_image_line_size_bytes;
    const uint32_t line_size_bytes = loaded_texture.line_size_bytes;
    SUPPORT_CHECK(full_image_line_size_bytes == line_size_bytes);

#if defined(PLATFORM_PS2)
    ps2_ensure_texture_upload_scratch(size_bytes, 4u);
#endif

    uint8_t *dest = tex_upload_buffer;
    for (uint32_t i = 0; i < size_bytes; i++, dest += 4) {
        const uint8_t intensity = addr[i];
        dest[0] = intensity;
        dest[1] = intensity;
        dest[2] = intensity;
        dest[3] = intensity;
    }

    const uint32_t width = rdp.texture_tile[tile].line_size_bytes;
    const uint32_t height = size_bytes / rdp.texture_tile[tile].line_size_bytes;

	gfx_rapi->upload_texture(tex_upload_buffer, width, height, gen_mipmaps);
    // DumpTexture(loaded_texture.otr_path, rgba32_buf, width, height);
}

static inline void palette_to_rgba32(const uint16_t palentry, uint8_t *rgba32_buf) {
    if (rdp.palette_fmt == G_TT_IA16) {
        const uint8_t intensity = (palentry & 0xff);
        const uint8_t alpha = palentry >> 8;
        rgba32_buf[0] = intensity;
        rgba32_buf[1] = intensity;
        rgba32_buf[2] = intensity;
        rgba32_buf[3] = alpha;
    } else {
        // assume G_TT_RGBA16
        const uint8_t a = palentry & 1;
        const uint8_t r = palentry >> 11;
        const uint8_t g = (palentry >> 6) & 0x1f;
        const uint8_t b = (palentry >> 1) & 0x1f;
        rgba32_buf[0] = SCALE_5_8(r);
        rgba32_buf[1] = SCALE_5_8(g);
        rgba32_buf[2] = SCALE_5_8(b);
        rgba32_buf[3] = a ? 255 : 0;
    }
}

static void import_texture_ci4(int tile, const LoadedTexture& loaded_texture, bool gen_mipmaps) {
	const RawTexMetadata* metadata = &loaded_texture.raw_tex_metadata;
    const uint8_t* addr = loaded_texture.addr;
    const uint32_t size_bytes = loaded_texture.size_bytes;
    const uint32_t full_image_line_size_bytes =
        loaded_texture.full_image_line_size_bytes;
    const uint32_t line_size_bytes = loaded_texture.line_size_bytes;
    const uint32_t pal_idx = rdp.texture_tile[tile].palette; // 0-15
    const uint16_t* palette = (const uint16_t *)(rdp.palette + pal_idx * 16); // 16 pixel entries, 16 bits each
    SUPPORT_CHECK(full_image_line_size_bytes == line_size_bytes);

#if defined(PLATFORM_PS2)
    ps2_ensure_texture_upload_scratch(size_bytes, 8u);
#endif

    for (uint32_t i = 0; i < size_bytes * 2; i++) {
        const uint8_t byte = addr[i / 2];
        const uint8_t idx = (byte >> (4 - (i % 2) * 4)) & 0xf;
        palette_to_rgba32(palette[idx], tex_upload_buffer +4 * i);
    }

    uint32_t result_line_size = rdp.texture_tile[tile].line_size_bytes;
    if (metadata->h_byte_scale != 1) {
        result_line_size *= metadata->h_byte_scale;
    }

    const uint32_t width = result_line_size * 2;
    const uint32_t height = size_bytes / result_line_size;

	gfx_rapi->upload_texture(tex_upload_buffer, width, height, gen_mipmaps);
}

static void import_texture_ci8(int tile, const LoadedTexture& loaded_texture, bool gen_mipmaps) {
	const RawTexMetadata* metadata = &loaded_texture.raw_tex_metadata;
    const uint8_t* addr = loaded_texture.addr;
    const uint32_t size_bytes = loaded_texture.size_bytes;
    const uint32_t full_image_line_size_bytes =
        loaded_texture.full_image_line_size_bytes;
    const uint32_t line_size_bytes = loaded_texture.line_size_bytes;

#if defined(PLATFORM_PS2)
    ps2_ensure_texture_upload_scratch(size_bytes, 4u);
#endif

    for (uint32_t i = 0, j = 0; i < size_bytes; j += full_image_line_size_bytes - line_size_bytes) {
        for (uint32_t k = 0; k < line_size_bytes; i++, k++, j++) {
            const uint8_t idx = addr[j];
            palette_to_rgba32(rdp.palette[idx], tex_upload_buffer + 4 * i);
        }
    }

    uint32_t result_line_size = rdp.texture_tile[tile].line_size_bytes;
    if (metadata->h_byte_scale != 1) {
        result_line_size *= metadata->h_byte_scale;
    }

    const uint32_t width = result_line_size;
    const uint32_t height = size_bytes / result_line_size;

	gfx_rapi->upload_texture(tex_upload_buffer, width, height, gen_mipmaps);
}

static void import_texture(int i, int tile, bool importReplacement) {
    LoadedTexture& loaded_texture = rdp.loaded_texture[rdp.texture_tile[tile].tmem];
    const uint8_t fmt = rdp.texture_tile[tile].fmt;
    const uint8_t siz = rdp.texture_tile[tile].siz;
    const uint32_t tex_flags = loaded_texture.tex_flags;
    const uint8_t palette_index = rdp.texture_tile[tile].palette;

    if ((rdp.tex_lod && tile >= rdp.first_tile_index + rdp.tex_detail) || !loaded_texture.addr) {
        // set up miplevel 0; also acts as a catch-all for when .addr is NULL because my texture loader sucks
        loaded_texture.addr = rdp.texture_to_load.addr;
        loaded_texture.line_size_bytes = rdp.texture_tile[tile].line_size_bytes;
        loaded_texture.full_image_line_size_bytes = rdp.texture_tile[tile].line_size_bytes;
        loaded_texture.full_size_bytes = loaded_texture.full_image_line_size_bytes * rdp.texture_tile[tile].height;
        loaded_texture.size_bytes = loaded_texture.line_size_bytes * rdp.texture_tile[tile].height;
        if (siz == G_IM_SIZ_32b) {
            // HACK: fixup 32-bit LODed texture height
            loaded_texture.size_bytes <<= 1;
            loaded_texture.full_size_bytes <<= 1;
        }
        loaded_texture.orig_size_bytes = loaded_texture.size_bytes;
    }

    const RawTexMetadata* metadata = &loaded_texture.raw_tex_metadata;
    const uint8_t* orig_addr = loaded_texture.addr;
    SUPPORT_CHECK(orig_addr);

    TextureCacheKey key;
    if (fmt == G_IM_FMT_CI) {
        key = { orig_addr, { rdp.palette_addrs[0], rdp.palette_addrs[1] }, fmt, siz, palette_index };
    } else {
        key = { orig_addr, {}, fmt, siz, palette_index };
    }

    if (gfx_texture_cache_lookup(i, key)) {
        return;
    }

    if (fmt == G_IM_FMT_RGBA) {
        if (siz == G_IM_SIZ_16b) {
            import_texture_rgba16(tile, loaded_texture, rdp.tex_lod);
        } else if (siz == G_IM_SIZ_32b) {
            import_texture_rgba32(tile, loaded_texture, rdp.tex_lod);
        } else {
            sysFatalError("Bad size for RGBA texture in tile %d: %02x", tile, siz);
        }
    } else if (fmt == G_IM_FMT_IA) {
        if (siz == G_IM_SIZ_4b) {
            import_texture_ia4(tile, loaded_texture, rdp.tex_lod);
        } else if (siz == G_IM_SIZ_8b) {
            import_texture_ia8(tile, loaded_texture, rdp.tex_lod);
        } else if (siz == G_IM_SIZ_16b) {
            import_texture_ia16(tile, loaded_texture, rdp.tex_lod);
        } else {
            sysFatalError("Bad size for IA texture in tile %d: %02x", tile, siz);
        }
    } else if (fmt == G_IM_FMT_CI) {
        if (siz == G_IM_SIZ_4b) {
            import_texture_ci4(tile, loaded_texture, rdp.tex_lod);
        } else if (siz == G_IM_SIZ_8b) {
            import_texture_ci8(tile, loaded_texture, rdp.tex_lod);
        } else {
            sysFatalError("Bad size for CI texture in tile %d: %02x", tile, siz);
        }
    } else if (fmt == G_IM_FMT_I) {
        if (siz == G_IM_SIZ_4b) {
            import_texture_i4(tile, loaded_texture, rdp.tex_lod);
        } else if (siz == G_IM_SIZ_8b) {
            import_texture_i8(tile, loaded_texture, rdp.tex_lod);
        } else {
            sysFatalError("Bad size for I texture in tile %d: %02x", tile, siz);
        }
    } else {
        sysFatalError("Bad texture format in tile %d: %02x %02x", tile, fmt, siz);
    }
}

static void gfx_normalize_vector(float v[3]) {
    float s = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    v[0] /= s;
    v[1] /= s;
    v[2] /= s;
}

static void gfx_transposed_matrix_mul(float res[3], const float a[3], const float b[4][4]) {
    res[0] = a[0] * b[0][0] + a[1] * b[0][1] + a[2] * b[0][2];
    res[1] = a[0] * b[1][0] + a[1] * b[1][1] + a[2] * b[1][2];
    res[2] = a[0] * b[2][0] + a[1] * b[2][1] + a[2] * b[2][2];
}

static void calculate_normal_dir(const Light_t* light, float coeffs[3]) {
    const float light_dir[3] = { light->dir[0] / 127.f, light->dir[1] / 127.f, light->dir[2] / 127.f };

    gfx_transposed_matrix_mul(coeffs, light_dir, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
    gfx_normalize_vector(coeffs);
}

static void calculate_normal_dir(const struct NormalColor *vcn, float coeffs[3]) {
    const float light_dir[3] = { vcn->x / 127.f, vcn->y / 127.f, vcn->z / 127.f };

    gfx_transposed_matrix_mul(coeffs, light_dir, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
    gfx_normalize_vector(coeffs);
}

static void gfx_matrix_mul(float res[4][4], const float a[4][4], const float b[4][4]) {
#if defined(PLATFORM_PS2) && defined(_EE)
    /*
     * VU0 macro mode: each result row is a[i].x*b0 + a[i].y*b1 + a[i].z*b2 +
     * a[i].w*b3, one MULA/MADDA/MADDA/MADD chain per row instead of 64 FPU
     * multiplies. LQC2/SQC2 need 16-byte alignment, so go through aligned
     * copies (res may alias a or b).
     */
    float ta[4][4] __attribute__((aligned(16)));
    float tb[4][4] __attribute__((aligned(16)));
    float tr[4][4] __attribute__((aligned(16)));
    memcpy(ta, a, sizeof(ta));
    memcpy(tb, b, sizeof(tb));
    __asm__ __volatile__(
        "lqc2    $vf4, 0(%2)\n\t"
        "lqc2    $vf5, 16(%2)\n\t"
        "lqc2    $vf6, 32(%2)\n\t"
        "lqc2    $vf7, 48(%2)\n\t"
        "lqc2    $vf1, 0(%1)\n\t"
        "lqc2    $vf2, 16(%1)\n\t"
        "lqc2    $vf3, 32(%1)\n\t"
        "lqc2    $vf8, 48(%1)\n\t"
        "vmulax.xyzw  $ACC, $vf4, $vf1x\n\t"
        "vmadday.xyzw $ACC, $vf5, $vf1y\n\t"
        "vmaddaz.xyzw $ACC, $vf6, $vf1z\n\t"
        "vmaddw.xyzw  $vf1, $vf7, $vf1w\n\t"
        "vmulax.xyzw  $ACC, $vf4, $vf2x\n\t"
        "vmadday.xyzw $ACC, $vf5, $vf2y\n\t"
        "vmaddaz.xyzw $ACC, $vf6, $vf2z\n\t"
        "vmaddw.xyzw  $vf2, $vf7, $vf2w\n\t"
        "vmulax.xyzw  $ACC, $vf4, $vf3x\n\t"
        "vmadday.xyzw $ACC, $vf5, $vf3y\n\t"
        "vmaddaz.xyzw $ACC, $vf6, $vf3z\n\t"
        "vmaddw.xyzw  $vf3, $vf7, $vf3w\n\t"
        "vmulax.xyzw  $ACC, $vf4, $vf8x\n\t"
        "vmadday.xyzw $ACC, $vf5, $vf8y\n\t"
        "vmaddaz.xyzw $ACC, $vf6, $vf8z\n\t"
        "vmaddw.xyzw  $vf8, $vf7, $vf8w\n\t"
        "sqc2    $vf1, 0(%0)\n\t"
        "sqc2    $vf2, 16(%0)\n\t"
        "sqc2    $vf3, 32(%0)\n\t"
        "sqc2    $vf8, 48(%0)\n\t"
        :
        : "r"(tr), "r"(ta), "r"(tb)
        : "memory");
    memcpy(res, tr, sizeof(tr));
#else
    float tmp[4][4];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            tmp[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j] + a[i][3] * b[3][j];
        }
    }
    memcpy(res, tmp, sizeof(tmp));
#endif
}

static void gfx_sp_matrix(uint8_t parameters, const int32_t* addr) {
    float matrix[4][4];

#ifndef GBI_FLOATS
    // Original GBI where fixed point matrices are used
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j += 2) {
            int32_t int_part = addr[i * 2 + j / 2];
            uint32_t frac_part = addr[8 + i * 2 + j / 2];
            matrix[i][j] = (int32_t)((int_part & 0xffff0000) | (frac_part >> 16)) / 65536.0f;
            matrix[i][j + 1] = (int32_t)((int_part << 16) | (frac_part & 0xffff)) / 65536.0f;
        }
    }
#else
    // For a modified GBI where fixed point values are replaced with floats
    memcpy(matrix, addr, sizeof(matrix));
#endif

    if (parameters & G_MTX_PROJECTION) {
        if (parameters & G_MTX_LOAD) {
            memcpy(rsp.P_matrix, matrix, sizeof(matrix));
        } else {
            gfx_matrix_mul(rsp.P_matrix, matrix, rsp.P_matrix);
        }
    } else { // G_MTX_MODELVIEW
        if ((parameters & G_MTX_PUSH) && rsp.modelview_matrix_stack_size < 11) {
            ++rsp.modelview_matrix_stack_size;
            memcpy(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1],
                   rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 2], sizeof(matrix));
        }
        if (parameters & G_MTX_LOAD) {
            memcpy(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], matrix, sizeof(matrix));
        } else {
            gfx_matrix_mul(rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], matrix,
                           rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1]);
        }
        rsp.lights_changed = 1;
    }
    gfx_matrix_mul(rsp.MP_matrix, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], rsp.P_matrix);
}

static void gfx_sp_pop_matrix(uint32_t count) {
    while (count--) {
        if (rsp.modelview_matrix_stack_size > 0) {
            --rsp.modelview_matrix_stack_size;
            if (rsp.modelview_matrix_stack_size > 0) {
                gfx_matrix_mul(rsp.MP_matrix, rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1],
                               rsp.P_matrix);
            }
        }
    }
}

static float gfx_adjust_x_for_aspect_ratio(float x, float w = 1.f) {
    if (fbActive) {
        return x;
    } else {
        return (rsp.aspect_ofs * w + x) * rsp.aspect_scale / gfx_current_dimensions.aspect_ratio;
    }
}

static void gfx_adjust_width_height_for_scale(uint32_t& width, uint32_t& height) {
    width = std::round(width * RATIO_Y);
    height = std::round(height * RATIO_Y);
    if (width == 0) {
        width = 1;
    }
    if (height == 0) {
        height = 1;
    }
}

#if defined(PLATFORM_PS2) && defined(_EE)
/* VU0 CLIP flags (bit0 +x, 1 -x, 2 +y, 3 -y, 4 +z) -> LoadedVertex.clip_rej. */
static const uint8_t gfx_vclip_to_rej[32] = {
#define R(f) (uint8_t)((((f) & 1) ? 2 : 0) | (((f) & 2) ? 1 : 0) | (((f) & 4) ? 8 : 0) | \
                      (((f) & 8) ? 4 : 0) | (((f) & 16) ? 32 : 0))
    R(0), R(1), R(2), R(3), R(4), R(5), R(6), R(7), R(8), R(9), R(10), R(11), R(12), R(13), R(14), R(15),
    R(16), R(17), R(18), R(19), R(20), R(21), R(22), R(23), R(24), R(25), R(26), R(27), R(28), R(29), R(30), R(31),
#undef R
};
#endif

static void gfx_sp_vertex(size_t n_vertices, size_t dest_index, const Vtx* vertices) {
    SUPPORT_CHECK(n_vertices <= MAX_VERTICES);
#if defined(GFX_PS2_DIRECT)
    if (gfx_obj_try_defer(n_vertices, dest_index, vertices)) {
        return;
    }
#endif

    /*
     * Per-call invariants. Writes through `d` alias rsp, so without local
     * copies the compiler reloads all sixteen matrix floats per vertex; the
     * aspect correction's division becomes one multiply by a constant.
     */
    float m[4][4] __attribute__((aligned(16)));
    memcpy(m, rsp.MP_matrix, sizeof(m));
#if defined(PLATFORM_PS2) && defined(_EE)
    int32_t vtx_in[4] __attribute__((aligned(16))) = { 0, 0, 0, 1 };
    float vtx_out[4] __attribute__((aligned(16)));
    uint32_t vclip = 0;
    __asm__ __volatile__(
        "lqc2    $vf4, 0(%0)\n\t"
        "lqc2    $vf5, 16(%0)\n\t"
        "lqc2    $vf6, 32(%0)\n\t"
        "lqc2    $vf7, 48(%0)\n\t"
        :
        : "r"(m)
        : "memory");
#endif
    const bool aspect_adjust = !fbActive;
    const float aspect_ofs = rsp.aspect_ofs;
    const float aspect_k = rsp.aspect_scale / gfx_current_dimensions.aspect_ratio;
#if defined(PLATFORM_PS2) && defined(_EE)
    /* x' = (ofs * w + x) * k on VU0 (identity when not adjusting), so the
     * transformed vertex in vf2 is final and VCLIP can classify it. */
    {
        float asp_k[4] __attribute__((aligned(16))) = { aspect_adjust ? aspect_k : 1.0f, 1.0f, 1.0f, 1.0f };
        float asp_o[4] __attribute__((aligned(16))) = { aspect_adjust ? aspect_ofs : 0.0f, 0.0f, 0.0f, 0.0f };
        __asm__ __volatile__(
            "lqc2    $vf21, 0(%0)\n\t"
            "lqc2    $vf22, 0(%1)\n\t"
            :
            : "r"(asp_k), "r"(asp_o)
            : "memory");
    }
#endif

    /* Lighting state is per call too; the 1/127 normal scale folds into it. */
    const bool lighting = (rsp.geometry_mode & G_LIGHTING) != 0;
    /* More per-call state: the stores through `d` may alias rsp, so reading
     * these inside the loop reloads them for every vertex. */
    const uint32_t geom_mode = rsp.geometry_mode;
    const int32_t tex_scale_s = rsp.texture_scaling_factor.s;
    const int32_t tex_scale_t = rsp.texture_scaling_factor.t;
    const float fog_mul = rsp.fog_mul;
    const float fog_offset = rsp.fog_offset;
    const uint8_t fog_alpha = rdp.fog_color.a;
    int num_dir_lights = 0;
    int ambient_r = 0, ambient_g = 0, ambient_b = 0;
    float light_coeffs[MAX_LIGHTS][3];
    float light_cols[MAX_LIGHTS][3];
    if (lighting) {
        if (rsp.lights_changed) {
            for (int i = 0; i < rsp.current_num_lights - 1; i++) {
                calculate_normal_dir(&rsp.current_lights[i], rsp.current_lights_coeffs[i]);
            }
            if (rsp.lookat_enabled) {
                calculate_normal_dir(&rsp.lookat[0], rsp.current_lookat_coeffs[0]);
                calculate_normal_dir(&rsp.lookat[1], rsp.current_lookat_coeffs[1]);
            }
            rsp.lights_changed = false;
        }
        num_dir_lights = rsp.current_num_lights - 1;
        if (num_dir_lights > MAX_LIGHTS) {
            num_dir_lights = MAX_LIGHTS;
        }
        ambient_r = rsp.current_lights[rsp.current_num_lights - 1].col[0];
        ambient_g = rsp.current_lights[rsp.current_num_lights - 1].col[1];
        ambient_b = rsp.current_lights[rsp.current_num_lights - 1].col[2];
        for (int i = 0; i < num_dir_lights; i++) {
            for (int k = 0; k < 3; k++) {
                light_coeffs[i][k] = rsp.current_lights_coeffs[i][k] * (1.0f / 127.0f);
                light_cols[i][k] = rsp.current_lights[i].col[k];
            }
        }
    }
#if defined(PLATFORM_PS2) && defined(_EE)
    /*
     * VU0 lighting: the four directional lights' coefficients as columns
     * (one intensity per lane), their colours, the ambient colour and the
     * two lookat rows stay in VF registers for the whole call. Unused lights
     * have zero coefficients and colours, so every vertex runs the same
     * branch-free sequence; a zero or negative intensity adds exactly 0.
     */
    float lit_cx[4] __attribute__((aligned(16))) = { 0, 0, 0, 0 };
    float lit_cy[4] __attribute__((aligned(16))) = { 0, 0, 0, 0 };
    float lit_cz[4] __attribute__((aligned(16))) = { 0, 0, 0, 0 };
    float lit_col[4][4] __attribute__((aligned(16))) = { { 0 } };
    int32_t lit_amb[4] __attribute__((aligned(16))) = { ambient_r, ambient_g, ambient_b, 0 };
    float la_x[4] __attribute__((aligned(16))) = { 0, 0, 0, 0 };
    float la_y[4] __attribute__((aligned(16))) = { 0, 0, 0, 0 };
    float la_z[4] __attribute__((aligned(16))) = { 0, 0, 0, 0 };
    int32_t nrm_in[4] __attribute__((aligned(16))) = { 0, 0, 0, 0 };
    int32_t lit_out[4] __attribute__((aligned(16)));
    float la_out[4] __attribute__((aligned(16)));
    if (lighting) {
        for (int i = 0; i < num_dir_lights && i < 4; i++) {
            lit_cx[i] = light_coeffs[i][0];
            lit_cy[i] = light_coeffs[i][1];
            lit_cz[i] = light_coeffs[i][2];
            lit_col[i][0] = light_cols[i][0];
            lit_col[i][1] = light_cols[i][1];
            lit_col[i][2] = light_cols[i][2];
        }
        if (rsp.lookat_enabled) {
            for (int i = 0; i < 2; i++) {
                la_x[i] = rsp.current_lookat_coeffs[i][0];
                la_y[i] = rsp.current_lookat_coeffs[i][1];
                la_z[i] = rsp.current_lookat_coeffs[i][2];
            }
        }
        __asm__ __volatile__(
            "lqc2    $vf8, 0(%0)\n\t"
            "lqc2    $vf9, 0(%1)\n\t"
            "lqc2    $vf10, 0(%2)\n\t"
            "lqc2    $vf11, 0(%3)\n\t"
            "lqc2    $vf12, 16(%3)\n\t"
            "lqc2    $vf13, 32(%3)\n\t"
            "lqc2    $vf14, 48(%3)\n\t"
            "lqc2    $vf20, 0(%4)\n\t"
            "vitof0.xyz $vf20, $vf20\n\t"
            "lqc2    $vf16, 0(%5)\n\t"
            "lqc2    $vf17, 0(%6)\n\t"
            "lqc2    $vf18, 0(%7)\n\t"
            :
            : "r"(lit_cx), "r"(lit_cy), "r"(lit_cz), "r"(lit_col), "r"(lit_amb),
              "r"(la_x), "r"(la_y), "r"(la_z)
            : "memory");
    }
#endif

#if defined(GFX_PS2_DIRECT)
    gfx_ps2_direct_invalidate_vertices(dest_index, n_vertices);
    if (dest_index < MAX_VERTICES) {
        const size_t end = dest_index + n_vertices < MAX_VERTICES ? dest_index + n_vertices : MAX_VERTICES;
        for (size_t i = dest_index; i < end; ++i) {
            gfx_obj.state[i] = GFX_OBJ_NORMAL;
        }
    }
#endif
    for (size_t i = 0; i < n_vertices; i++, dest_index++) {
        const Vtx* v = &vertices[i];
        struct LoadedVertex* d = &rsp.loaded_vertices[dest_index];

#if defined(PLATFORM_PS2) && defined(_EE)
        /* VU0 macro mode: ITOF + one MULA/MADDA/MADDA/MADD chain (vf4..vf7
         * hold the matrix rows, loaded once before the loop). */
        vtx_in[0] = v->v[0];
        vtx_in[1] = v->v[1];
        vtx_in[2] = v->v[2];
        __asm__ __volatile__(
            "lqc2    $vf1, 0(%2)\n\t"
            "vitof0.xyz $vf1, $vf1\n\t"
            "vmulax.xyzw  $ACC, $vf4, $vf1x\n\t"
            "vmadday.xyzw $ACC, $vf5, $vf1y\n\t"
            "vmaddaz.xyzw $ACC, $vf6, $vf1z\n\t"
            "vmaddw.xyzw  $vf2, $vf7, $vf0w\n\t"
            "vmulw.x      $vf3, $vf22, $vf2w\n\t"
            "vadd.x       $vf2, $vf2, $vf3\n\t"
            "vmul.x       $vf2, $vf2, $vf21\n\t"
            "vclipw.xyz   $vf2xyz, $vf2w\n\t"
            "sqc2    $vf2, 0(%1)\n\t"
            "vnop\n\t"
            "vnop\n\t"
            "vnop\n\t"
            "cfc2    %0, $vi18\n\t"
            : "=r"(vclip)
            : "r"(vtx_out), "r"(vtx_in)
            : "memory");
        float x = vtx_out[0];
        float y = vtx_out[1];
        float z = vtx_out[2];
        float w = vtx_out[3];
#else
        const float vx = v->v[0];
        const float vy = v->v[1];
        const float vz = v->v[2];
        float x = vx * m[0][0] + vy * m[1][0] + vz * m[2][0] + m[3][0];
        float y = vx * m[0][1] + vy * m[1][1] + vz * m[2][1] + m[3][1];
        float z = vx * m[0][2] + vy * m[1][2] + vz * m[2][2] + m[3][2];
        float w = vx * m[0][3] + vy * m[1][3] + vz * m[2][3] + m[3][3];
#endif

#if !(defined(PLATFORM_PS2) && defined(_EE))
        if (aspect_adjust) {
            x = (aspect_ofs * w + x) * aspect_k;
        }
#endif

        short U = v->s * tex_scale_s >> 16;
        short V = v->t * tex_scale_t >> 16;

        const struct NormalColor *vcn = &rsp.vertex_colors[v->colour >> 2];

        if (lighting) {
#if defined(PLATFORM_PS2) && defined(_EE)
            /* Intensities of all four lights in one MULA/MADDA/MADD chain,
             * clamped at zero, then accumulated per light with the same
             * truncate-to-int after every light as the scalar loop. Also the
             * two lookat dot products for texture generation. */
            nrm_in[0] = vcn->x;
            nrm_in[1] = vcn->y;
            nrm_in[2] = vcn->z;
            __asm__ __volatile__(
                "lqc2    $vf1, 0(%2)\n\t"
                "vitof0.xyz $vf1, $vf1\n\t"
                "vmulax.xyzw  $ACC, $vf8, $vf1x\n\t"
                "vmadday.xyzw $ACC, $vf9, $vf1y\n\t"
                "vmaddz.xyzw  $vf2, $vf10, $vf1z\n\t"
                "vmulax.xy    $ACC, $vf16, $vf1x\n\t"
                "vmadday.xy   $ACC, $vf17, $vf1y\n\t"
                "vmaddz.xy    $vf19, $vf18, $vf1z\n\t"
                "vmaxx.xyzw   $vf2, $vf2, $vf0x\n\t"
                "vaddax.xyz   $ACC, $vf20, $vf0x\n\t"
                "vmaddx.xyz   $vf3, $vf11, $vf2x\n\t"
                "vftoi0.xyz   $vf3, $vf3\n\t"
                "vitof0.xyz   $vf3, $vf3\n\t"
                "vaddax.xyz   $ACC, $vf3, $vf0x\n\t"
                "vmaddy.xyz   $vf3, $vf12, $vf2y\n\t"
                "vftoi0.xyz   $vf3, $vf3\n\t"
                "vitof0.xyz   $vf3, $vf3\n\t"
                "vaddax.xyz   $ACC, $vf3, $vf0x\n\t"
                "vmaddz.xyz   $vf3, $vf13, $vf2z\n\t"
                "vftoi0.xyz   $vf3, $vf3\n\t"
                "vitof0.xyz   $vf3, $vf3\n\t"
                "vaddax.xyz   $ACC, $vf3, $vf0x\n\t"
                "vmaddw.xyz   $vf3, $vf14, $vf2w\n\t"
                "vftoi0.xyz   $vf3, $vf3\n\t"
                "sqc2    $vf3, 0(%0)\n\t"
                "sqc2    $vf19, 0(%1)\n\t"
                :
                : "r"(lit_out), "r"(la_out), "r"(nrm_in)
                : "memory");
            const int r = lit_out[0];
            const int g = lit_out[1];
            const int b = lit_out[2];
#else
            int r = ambient_r;
            int g = ambient_g;
            int b = ambient_b;

            for (int i = 0; i < num_dir_lights; i++) {
                const float intensity = vcn->x * light_coeffs[i][0] +
                    vcn->y * light_coeffs[i][1] + vcn->z * light_coeffs[i][2];
                if (intensity > 0.0f) {
                    r += intensity * light_cols[i][0];
                    g += intensity * light_cols[i][1];
                    b += intensity * light_cols[i][2];
                }
            }
#endif

            d->color.r = r > 255 ? 255 : r;
            d->color.g = g > 255 ? 255 : g;
            d->color.b = b > 255 ? 255 : b;

            if (geom_mode & G_TEXTURE_GEN) {
                float dotx = 0, doty = 0;
                if (rsp.lookat_enabled) {
#if defined(PLATFORM_PS2) && defined(_EE)
                    dotx = la_out[0] / 127.0f;
                    doty = la_out[1] / 127.0f;
#else
                    dotx += vcn->x * rsp.current_lookat_coeffs[0][0];
                    dotx += vcn->y * rsp.current_lookat_coeffs[0][1];
                    dotx += vcn->z * rsp.current_lookat_coeffs[0][2];
                    doty += vcn->x * rsp.current_lookat_coeffs[1][0];
                    doty += vcn->y * rsp.current_lookat_coeffs[1][1];
                    doty += vcn->z * rsp.current_lookat_coeffs[1][2];
                    dotx /= 127.0f;
                    doty /= 127.0f;
#endif
                } else {
                    float tvcn[3];
                    calculate_normal_dir(vcn, tvcn);
                    dotx = tvcn[0];
                    doty = tvcn[1];
                }

                dotx = clampf(dotx, -1.0f, 1.0f);
                doty = clampf(doty, -1.0f, 1.0f);

                if (geom_mode & G_TEXTURE_GEN_LINEAR) {
                    // Not sure exactly what formula we should use to get accurate values
                    /*dotx = (2.906921f * dotx * dotx + 1.36114f) * dotx;
                    doty = (2.906921f * doty * doty + 1.36114f) * doty;
                    dotx = (dotx + 1.0f) / 4.0f;
                    doty = (doty + 1.0f) / 4.0f;*/
                    dotx = acosf(-dotx) /* M_PI */ / 4.0f;
                    doty = acosf(-doty) /* M_PI */ / 4.0f;
                } else {
                    dotx = (dotx + 1.0f) / 4.0f;
                    doty = (doty + 1.0f) / 4.0f;
                }

                U = (int32_t)(dotx * tex_scale_s);
                V = (int32_t)(doty * tex_scale_t);
            }
        } else {
            d->color.r = vcn->r;
            d->color.g = vcn->g;
            d->color.b = vcn->b;
        }

        d->u = U;
        d->v = V;

        // trivial clip rejection
#if defined(PLATFORM_PS2) && defined(_EE)
        /* VCLIP compares against |w|, which matches the signed tests below
         * only for w > 0. Its flags (+x -x +y -y +z -z) map to
         * RIGHT LEFT TOP BOTTOM FAR -. */
        if (w > 0.0f) {
            d->clip_rej = gfx_vclip_to_rej[vclip & 0x1f];
        } else
#endif
        {
            uint8_t rej = 0;
            if (x < -w) {
                rej |= 1; // CLIP_LEFT
            }
            if (x > w) {
                rej |= 2; // CLIP_RIGHT
            }
            if (y < -w) {
                rej |= 4; // CLIP_BOTTOM
            }
            if (y > w) {
                rej |= 8; // CLIP_TOP
            }
            // if (z < -w) rej |= 16; // CLIP_NEAR
            if (z > w) {
                rej |= 32; // CLIP_FAR
            }
            d->clip_rej = rej;
        }

        d->x = x;
        d->y = y;
        d->z = z;
        d->w = w;

        if (geom_mode & G_FOG) {
            if (fabsf(w) < 0.001f) {
                // To avoid division by zero
                w = 0.001f;
            }

            float winv = 1.0f / w;
            if (winv < 0.0f) {
                winv = std::numeric_limits<int16_t>::max();
            }

            float fog_z = z * winv * fog_mul + fog_offset;
            d->fog = clampf(fog_z, 0.f, 255.f);
        } else {
            d->fog = fog_alpha;
        }

        d->color.a = vcn->a; // can be required for SHADE_ALPHA even if fog is enabled
    }
}

static void gfx_sp_modify_vertex(uint16_t vtx_idx, uint8_t where, uint32_t val) {
    SUPPORT_CHECK(where == G_MWO_POINT_ST);

    int16_t s = (int16_t)(val >> 16);
    int16_t t = (int16_t)val;

#if defined(GFX_PS2_DIRECT)
    if (vtx_idx < MAX_VERTICES && gfx_obj.state[vtx_idx] != GFX_OBJ_NORMAL) {
        gfx_obj_materialize_ctx(gfx_obj.ctx[vtx_idx]);
    }
#endif
    struct LoadedVertex* v = &rsp.loaded_vertices[vtx_idx];
    v->u = s;
    v->v = t;
#if defined(GFX_PS2_DIRECT)
    gfx_ps2_direct_invalidate_vertices(vtx_idx, 1);
#endif
}

static inline int gfx_lod_tile_offset(const int i) {
    if (gfx_detail_textures_enabled)
        return ((rdp.tex_lod && !rdp.tex_detail) ? 0 : i);
    return (rdp.tex_lod ? rdp.tex_detail : i);
}

/*
 * Everything gfx_sp_tri1 derives from RDP/RSP state rather than from the
 * triangle's vertices. Display lists issue long runs of G_VTX/G_TRI* with no
 * state change, so the result is cached and only rebuilt after a command that
 * can change state (gfx_run_dl invalidates on every other opcode).
 */
enum { GFX_TRI_IN_CONST, GFX_TRI_IN_SHADE, GFX_TRI_IN_SHADE_ALPHA, GFX_TRI_IN_LOD };

struct GfxTriSetup {
    bool valid;
    bool depth_source_prim;
    bool z_is_from_0_to_1;
    bool invert_y;
    bool use_alpha;
    bool use_fog;
    bool use_grayscale;
    bool used_textures[2];
    bool clamp_s[2];
    bool clamp_t[2];
    uint8_t num_inputs;
    float u_scale[2], u_bias[2], v_scale[2], v_bias[2];
    float tex_clamp_s[2], tex_clamp_t[2];
    uint8_t in_kind[2][7];
    float in_const[7][4];
    uint8_t in_const_u8[7][4]; /* the bytes in_const was built from */
};

static struct GfxTriSetup gfx_tri_setup_cache;

static inline void gfx_tri_setup_invalidate(void) {
    gfx_tri_setup_cache.valid = false;
}

static void gfx_tri_setup(struct GfxTriSetup* S, bool is_rect) {
    bool depth_test = ((rsp.geometry_mode & G_ZBUFFER) == G_ZBUFFER || (rdp.other_mode_l & G_ZS_PRIM) == G_ZS_PRIM) &&
                      ((rdp.other_mode_h & G_CYC_1CYCLE) == G_CYC_1CYCLE || (rdp.other_mode_h & G_CYC_2CYCLE) == G_CYC_2CYCLE);
    bool depth_update = (rdp.other_mode_l & Z_UPD) == Z_UPD;
    bool depth_compare = (rdp.other_mode_l & Z_CMP) == Z_CMP;
    bool depth_source_prim = (rdp.other_mode_l & G_ZS_PRIM) == G_ZS_PRIM /* && gDP.primDepth.z == 1.0f */;
    uint16_t zmode = rdp.other_mode_l & ZMODE_DEC;
    uint8_t depth_mode = (depth_test ? 1 : 0) | (depth_update ? 2 : 0) | (depth_compare ? 4 : 0) | (depth_source_prim ? 8 : 0) | (zmode >> 6);

    if (depth_mode != rendering_state.depth_mode) {
        gfx_flush();
        gfx_rapi->set_depth_mode(depth_test, depth_update, depth_compare, depth_source_prim, zmode);
        rendering_state.depth_mode = depth_mode;
    }

    if (rdp.viewport_or_scissor_changed) {
        if (memcmp(&rdp.viewport, &rendering_state.viewport, sizeof(rdp.viewport)) != 0) {
            gfx_flush();
            gfx_rapi->set_viewport(rdp.viewport.x, rdp.viewport.y, rdp.viewport.width, rdp.viewport.height);
            rendering_state.viewport = rdp.viewport;
        }
        if (memcmp(&rdp.scissor, &rendering_state.scissor, sizeof(rdp.scissor)) != 0) {
            gfx_flush();
            gfx_rapi->set_scissor(rdp.scissor.x, rdp.scissor.y, rdp.scissor.width, rdp.scissor.height);
            rendering_state.scissor = rdp.scissor;
        }
        rdp.viewport_or_scissor_changed = false;
    }

    uint64_t cc_options = 0;
    bool use_alpha =
        (rdp.other_mode_l & (3 << 20)) == (G_BL_CLR_MEM << 20) && (rdp.other_mode_l & (3 << 16)) == (G_BL_1MA << 16);
    const bool use_fog = ((rdp.other_mode_l >> 30) == G_BL_CLR_FOG) || ((rdp.other_mode_l >> 26) == G_BL_A_FOG);
    const bool texture_edge = (rdp.other_mode_l & CVG_X_ALPHA) == CVG_X_ALPHA;
    const bool use_noise = (rdp.other_mode_l & (3U << G_MDSFT_ALPHACOMPARE)) == G_AC_DITHER;
    const bool use_2cyc = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_2CYCLE;
    const bool alpha_threshold = (rdp.other_mode_l & (3U << G_MDSFT_ALPHACOMPARE)) == G_AC_THRESHOLD;
    const bool invisible = (rdp.other_mode_l & (3 << 24)) == (G_BL_0 << 24) && (rdp.other_mode_l & (3 << 20)) == (G_BL_CLR_MEM << 20);
    const bool use_grayscale = rdp.grayscale;
    const bool use_modulate = use_alpha && (rsp.extra_geometry_mode & G_MODULATE_EXT) != 0;
    const bool use_blur = (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) == G_TF_BLUR_EXT;

    if (texture_edge) {
        use_alpha = true;
    }

    if (use_alpha) {
        cc_options |= (uint64_t)SHADER_OPT_ALPHA;
    }
    if (use_fog) {
        cc_options |= (uint64_t)SHADER_OPT_FOG;
    }
    if (texture_edge) {
        cc_options |= (uint64_t)SHADER_OPT_TEXTURE_EDGE;
    }
    if (use_noise) {
        cc_options |= (uint64_t)SHADER_OPT_NOISE;
    }
    if (use_2cyc) {
        cc_options |= (uint64_t)SHADER_OPT_2CYC;
    }
    if (alpha_threshold) {
        cc_options |= (uint64_t)SHADER_OPT_ALPHA_THRESHOLD;
    }
    if (invisible) {
        cc_options |= (uint64_t)SHADER_OPT_INVISIBLE;
    }
    if (use_grayscale) {
        cc_options |= (uint64_t)SHADER_OPT_GRAYSCALE;
    }
    if (use_blur) {
        cc_options |= (uint64_t)SHADER_OPT_BLUR;
    }

    // If we are not using alpha, clear the alpha components of the combiner as they have no effect
    if (!use_alpha) {
        cc_options &= ~((0xfff << 16) | ((uint64_t)0xfff << 44));
    }

    ColorCombinerKey key;
    key.combine_mode = rdp.combine_mode;
    key.options = cc_options;

    ColorCombiner* comb = gfx_lookup_or_create_color_combiner(key);

    uint32_t tm = 0;
    uint32_t tex_width[2], tex_height[2], tex_width2[2], tex_height2[2];

    for (int i = 0; i < 2; i++) {
        // TODO: fix this; for now just ignore smaller mips
        const uint32_t tile = rdp.first_tile_index + gfx_lod_tile_offset(i);
        if (comb->used_textures[i]) {
            if (rdp.textures_changed[i]) {
                gfx_flush();
                import_texture(i, tile, false);
                rdp.textures_changed[i] = false;
            }

            uint8_t cms = rdp.texture_tile[tile].cms;
            uint8_t cmt = rdp.texture_tile[tile].cmt;

            uint32_t tex_size_bytes = rdp.loaded_texture[rdp.texture_tile[tile].tmem].orig_size_bytes;
            uint32_t line_size = rdp.texture_tile[tile].line_size_bytes;

            if (line_size == 0) {
                line_size = 1;
            }

            tex_height[i] = tex_size_bytes / line_size;
            switch (rdp.texture_tile[tile].siz) {
                case G_IM_SIZ_4b:
                    line_size <<= 1;
                    break;
                case G_IM_SIZ_8b:
                    break;
                case G_IM_SIZ_16b:
                    line_size /= G_IM_SIZ_16b_LINE_BYTES;
                    break;
                case G_IM_SIZ_32b:
                    line_size /= G_IM_SIZ_32b_LINE_BYTES; // this is 2!
                    tex_height[i] /= 2;
                    break;
            }
            tex_width[i] = line_size;

            tex_width2[i] = (rdp.texture_tile[tile].lrs - rdp.texture_tile[tile].uls + 4) / 4;
            tex_height2[i] = (rdp.texture_tile[tile].lrt - rdp.texture_tile[tile].ult + 4) / 4;

            uint32_t tex_width1 = tex_width[i] << (cms & G_TX_MIRROR);
            uint32_t tex_height1 = tex_height[i] << (cmt & G_TX_MIRROR);

            if ((cms & G_TX_CLAMP) && ((cms & G_TX_MIRROR) || tex_width1 != tex_width2[i])) {
                tm |= 1 << 2 * i;
                cms &= ~G_TX_CLAMP;
            }
            if ((cmt & G_TX_CLAMP) && ((cmt & G_TX_MIRROR) || tex_height1 != tex_height2[i])) {
                tm |= 1 << (2 * i + 1);
                cmt &= ~G_TX_CLAMP;
            }

            if (rendering_state.textures[i]) {
                bool linear_filter = (rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT;
                if (linear_filter != rendering_state.textures[i]->second.linear_filter ||
                    cms != rendering_state.textures[i]->second.cms || cmt != rendering_state.textures[i]->second.cmt) {
                    gfx_flush();
                    gfx_rapi->set_sampler_parameters(i, linear_filter, cms, cmt, rdp.tex_lod);
                    rendering_state.textures[i]->second.linear_filter = linear_filter;
                    rendering_state.textures[i]->second.cms = cms;
                    rendering_state.textures[i]->second.cmt = cmt;
                }
            }
        }
    }

    struct ShaderProgram* prg = comb->prg[tm];
    if (prg == NULL) {
        comb->prg[tm] = prg =
            gfx_lookup_or_create_shader_program(comb->shader_id0, comb->shader_id1 | (tm * SHADER_OPT_TEXEL0_CLAMP_S));
    }
    if (prg != rendering_state.shader_program) {
        gfx_flush();
        gfx_rapi->unload_shader(rendering_state.shader_program);
        gfx_rapi->load_shader(prg);
        rendering_state.shader_program = prg;
    }
    if (use_alpha != rendering_state.alpha_blend || use_modulate != rendering_state.modulate) {
        gfx_flush();
        gfx_rapi->set_use_alpha(use_alpha, use_modulate);
        rendering_state.alpha_blend = use_alpha;
        rendering_state.modulate = use_modulate;
    }
    uint8_t num_inputs;
    bool used_textures[2];

    gfx_rapi->shader_get_info(prg, &num_inputs, used_textures);

    struct GfxClipParameters clip_parameters = gfx_rapi->get_clip_parameters();

    /*
     * Per-triangle constants for the texture coordinate loop below. Dividing
     * by a power of two equals multiplying by its exactly representable
     * reciprocal under IEEE rounding, so these replace per-vertex divisions
     * with bit-identical multiplies; NPOT sizes keep the true division.
     */
    float tex_width_rcp[2] = { 0.0f, 0.0f };
    float tex_height_rcp[2] = { 0.0f, 0.0f };
    float tex_clamp_s[2] = { 0.0f, 0.0f };
    float tex_clamp_t[2] = { 0.0f, 0.0f };
    for (int t = 0; t < 2; t++) {
        if (!used_textures[t]) {
            continue;
        }
        tex_width_rcp[t] = gfx_exact_pow2_reciprocal(tex_width[t]);
        tex_height_rcp[t] = gfx_exact_pow2_reciprocal(tex_height[t]);
        if (tm & (1 << 2 * t)) {
            tex_clamp_s[t] = (tex_width2[t] - 0.5f) / tex_width[t];
        }
        if (tm & (1 << (2 * t + 1))) {
            tex_clamp_t[t] = (tex_height2[t] - 0.5f) / tex_height[t];
        }
    }


    S->depth_source_prim = depth_source_prim;
    S->z_is_from_0_to_1 = clip_parameters.z_is_from_0_to_1;
    S->invert_y = clip_parameters.invert_y;
    S->use_alpha = use_alpha;
    S->use_fog = use_fog;
    S->use_grayscale = use_grayscale;
    S->num_inputs = num_inputs;
    for (int t = 0; t < 2; t++) {
        S->used_textures[t] = used_textures[t];
        S->clamp_s[t] = (tm & (1 << 2 * t)) != 0;
        S->clamp_t[t] = (tm & (1 << (2 * t + 1))) != 0;
        S->tex_clamp_s[t] = tex_clamp_s[t];
        S->tex_clamp_t[t] = tex_clamp_t[t];
        if (!used_textures[t]) {
            continue;
        }
        // TODO: fix this; for now just ignore smaller mips
        const auto* tt = &rdp.texture_tile[rdp.first_tile_index + gfx_lod_tile_offset(t)];
        const int shifts = tt->shifts;
        const int shiftt = tt->shiftt;
        const float su = shifts == 0 ? 1.0f
            : (shifts <= 10 ? gfx_pow2_reciprocal[shifts] : (float)(1 << (16 - shifts)));
        const float sv = shiftt == 0 ? 1.0f
            : (shiftt <= 10 ? gfx_pow2_reciprocal[shiftt] : (float)(1 << (16 - shiftt)));
        float half = 1.0f;
        float add = 0.0f;
        if (!is_rect) {
            if (!(rdp.other_mode_h & G_TP_PERSP)) {
                half = 0.5f;
            }
            if ((rdp.other_mode_h & (3U << G_MDSFT_TEXTFILT)) != G_TF_POINT) {
                // Linear filter adds 0.5f to the coordinates
                add = 0.5f;
            }
        }
        const float rw = tex_width_rcp[t] != 0.0f ? tex_width_rcp[t] : 1.0f / tex_width[t];
        const float rh = tex_height_rcp[t] != 0.0f ? tex_height_rcp[t] : 1.0f / tex_height[t];
        /* u' = ((u / 32 * su - uls / 4) * half + add) / width, folded. */
        S->u_scale[t] = su / 32.0f * half * rw;
        S->u_bias[t] = (add - tt->uls / 4.0f * half) * rw;
        S->v_scale[t] = sv / 32.0f * half * rh;
        S->v_bias[t] = (add - tt->ult / 4.0f * half) * rh;
    }

    for (int j = 0; j < num_inputs; j++) {
        for (int k = 0; k < 1 + (use_alpha ? 1 : 0); k++) {
            struct RGBA tmp = { 0 };
            uint8_t kind = GFX_TRI_IN_CONST;
            switch (comb->shader_input_mapping[k][j]) {
                    // Note: CCMUX constants and ACMUX constants used here have same value, which is why this works
                    // (except LOD fraction).
                case G_CCMUX_PRIMITIVE:
                    tmp = rdp.prim_color;
                    break;
                case G_CCMUX_SHADE:
                    kind = GFX_TRI_IN_SHADE;
                    break;
                case G_CCMUX_SHADE_ALPHA:
                    kind = GFX_TRI_IN_SHADE_ALPHA;
                    break;
                case G_CCMUX_ENVIRONMENT:
                    tmp = rdp.env_color;
                    break;
                case G_CCMUX_PRIMITIVE_ALPHA:
                    tmp.r = tmp.g = tmp.b = rdp.prim_color.a;
                    break;
                case G_CCMUX_ENV_ALPHA:
                    tmp.r = tmp.g = tmp.b = rdp.env_color.a;
                    break;
                case G_CCMUX_PRIM_LOD_FRAC:
                    tmp.r = tmp.g = tmp.b = rdp.prim_lod_fraction;
                    break;
                case G_CCMUX_LOD_FRACTION:
                    if (rdp.other_mode_h & G_TL_LOD) {
                        kind = GFX_TRI_IN_LOD;
                    } else {
                        tmp.r = tmp.g = tmp.b = tmp.a = 255;
                    }
                    break;
                case G_ACMUX_PRIM_LOD_FRAC:
                    tmp.a = rdp.prim_lod_fraction;
                    break;
                default:
                    break;
            }
            S->in_kind[k][j] = kind;
            if (k == 0) {
                S->in_const[j][0] = gfx_u8_unit[tmp.r];
                S->in_const[j][1] = gfx_u8_unit[tmp.g];
                S->in_const[j][2] = gfx_u8_unit[tmp.b];
                S->in_const_u8[j][0] = tmp.r;
                S->in_const_u8[j][1] = tmp.g;
                S->in_const_u8[j][2] = tmp.b;
            } else {
                S->in_const[j][3] = gfx_u8_unit[tmp.a];
                S->in_const_u8[j][3] = tmp.a;
            }
        }
    }
}

#if defined(GFX_PS2_DIRECT)
/*
 * Shared-vertex fast path (PS2). Each loaded vertex is turned into its VU1
 * transform input record and clip outcodes once per (vertex, state) pair;
 * triangles then copy three records. Guard-band-accepted triangles of the
 * eligible shaders go straight to gfxPs2DirectDraw; everything else (rects,
 * triangles that need clipping, other shaders) keeps the float-VBO path.
 * The two buffers are never both non-empty, so draw order is preserved.
 */
/* Re-evaluated whenever the triangle setup is rebuilt. */
static void gfx_ps2_direct_refresh(const struct GfxTriSetup* S) {
    struct GfxPs2DirectKey key;
    memset(&key, 0, sizeof(key));
    bool ok = !S->depth_source_prim && !S->use_grayscale &&
              !S->clamp_s[0] && !S->clamp_t[0] && !S->clamp_s[1] && !S->clamp_t[1] && S->num_inputs <= 3 &&
              gfxPs2DirectQuery(&key.ctx);
    if (ok) {
        ok = key.ctx.num_inputs == S->num_inputs && (key.ctx.use_alpha != 0) == S->use_alpha &&
             (key.ctx.fog != 0) == S->use_fog &&
             (key.ctx.color_only ? !S->used_textures[0] && !S->used_textures[1] : S->used_textures[0]);
    }
    gfx_ps2_direct.active = ok;
    if (!ok) {
        return;
    }
    key.z_is_from_0_to_1 = S->z_is_from_0_to_1;
    key.invert_y = S->invert_y;
    for (int j = 0; j < S->num_inputs; ++j) {
        key.in_kind[0][j] = S->in_kind[0][j];
        key.in_kind[1][j] = S->use_alpha ? S->in_kind[1][j] : 0;
        memcpy(key.in_const_u8[j], S->in_const_u8[j], 4);
        if (!S->use_alpha) {
            key.in_const_u8[j][3] = 0;
        }
    }
    if (!key.ctx.color_only) {
        key.u_scale = S->u_scale[0];
        key.u_bias = S->u_bias[0];
        key.v_scale = S->v_scale[0];
        key.v_bias = S->v_bias[0];
    }

    {
        /* gfx_ps2_direct_input + LUTs, decided once per state */
        const struct GfxPs2DirectContext* C = &key.ctx;
        for (int c = 0; c < 4; ++c) {
            const int j = c < 3 ? C->rgb_input : C->alpha_input;
            const uint8_t* lut = c < 3 ? C->rgb_lut : C->alpha_lut;
            uint8_t src = 0xff;
            uint8_t cval = c < 3 ? C->rgb_const : C->alpha_const;
            if (j >= 0) {
                if (j >= S->num_inputs || (c == 3 && !S->use_alpha)) {
                    cval = lut[255];
                } else {
                    switch (S->in_kind[c < 3 ? 0 : 1][j]) {
                        case GFX_TRI_IN_SHADE: src = (uint8_t)c; break;
                        case GFX_TRI_IN_SHADE_ALPHA: src = c < 3 ? 3 : 5; break;
                        case GFX_TRI_IN_LOD: src = 4; break;
                        default: cval = lut[S->in_const_u8[j][c]]; break;
                    }
                }
            }
            gfx_ps2_direct.rec_src[c] = src;
            gfx_ps2_direct.rec_const[c] = cval;
            gfx_ps2_direct.rec_lut[c] = lut;
        }
        gfx_ps2_direct.rec_lod = gfx_ps2_direct.rec_src[0] == 4 || gfx_ps2_direct.rec_src[1] == 4 ||
                                 gfx_ps2_direct.rec_src[2] == 4 || gfx_ps2_direct.rec_src[3] == 4;
    }
    const bool fog_differs = key.ctx.fog && gfx_ps2_direct.batch_count != 0 &&
        memcmp(gfx_ps2_direct.batch_fog, &rdp.fog_color, 3) != 0;
    if (memcmp(&key, &gfx_ps2_direct.key, sizeof(key)) != 0 || fog_differs) {
        /* Buffered triangles were built for the old state. */
        gfx_ps2_direct_flush();
        gfx_ps2_direct.key = key;
        if (++gfx_ps2_direct.serial == 0) {
            gfx_ps2_direct.serial = 1;
        }
    }
}

/* Byte value of combiner input j for one vertex; channel 3 is alpha. */
[[maybe_unused]] static inline void gfx_ps2_direct_input(const struct GfxTriSetup* S, const struct LoadedVertex* V, int j,
                                        uint8_t out[4]) {
    if (j < 0 || j >= S->num_inputs) {
        out[0] = out[1] = out[2] = out[3] = 255; /* backend default 1.0 */
        return;
    }
    uint8_t lod = 0;
    if (S->in_kind[0][j] == GFX_TRI_IN_LOD || (S->use_alpha && S->in_kind[1][j] == GFX_TRI_IN_LOD)) {
        const float distance_frac = std::max(0.f, std::min(V->w / 1024.f, 1.f));
        lod = (uint8_t)((0.7f + distance_frac * 0.3f) * 255.f);
    }
    switch (S->in_kind[0][j]) {
        case GFX_TRI_IN_SHADE:
            out[0] = V->color.r; out[1] = V->color.g; out[2] = V->color.b;
            break;
        case GFX_TRI_IN_SHADE_ALPHA:
            out[0] = out[1] = out[2] = V->color.a;
            break;
        case GFX_TRI_IN_LOD:
            out[0] = out[1] = out[2] = lod;
            break;
        default:
            out[0] = S->in_const_u8[j][0]; out[1] = S->in_const_u8[j][1]; out[2] = S->in_const_u8[j][2];
            break;
    }
    if (!S->use_alpha) {
        out[3] = 255;
        return;
    }
    switch (S->in_kind[1][j]) {
        case GFX_TRI_IN_SHADE: out[3] = V->color.a; break;
        case GFX_TRI_IN_SHADE_ALPHA: out[3] = 0; break;
        case GFX_TRI_IN_LOD: out[3] = lod; break;
        default: out[3] = S->in_const_u8[j][3]; break;
    }
}

static inline uint32_t gfx_ps2_direct_vertex(const struct GfxTriSetup* S, uint8_t idx) {
    if (gfx_ps2_direct.tag[idx] == gfx_ps2_direct.serial) {
        return gfx_ps2_direct.codes[idx];
    }
    const struct GfxPs2DirectContext* C = &gfx_ps2_direct.key.ctx;
    const struct LoadedVertex* V = &rsp.loaded_vertices[idx];
    struct Ps2GsVu1TransformVertex* R = &gfx_ps2_direct.records[idx];

    /* Same arithmetic as the VBO writer in gfx_sp_tri1 followed by the
     * backend translator; see ps2_draw_triangles_unclipped. */
    float z = V->z;
    const float w = V->w;
    if (S->z_is_from_0_to_1) {
        z = (z + w) / 2.0f;
    }
    R->clip[0] = V->x;
    R->clip[1] = S->invert_y ? -V->y : V->y;
    R->clip[2] = z;
    R->clip[3] = w;
    if (C->color_only) {
        R->texcoord.s = 0.0f;
        R->texcoord.t = 0.0f;
    } else {
        const float vbo_u = V->u * S->u_scale[0] + S->u_bias[0];
        const float vbo_v = V->v * S->v_scale[0] + S->v_bias[0];
        R->texcoord.s = vbo_u * C->scale_s;
        R->texcoord.t = vbo_v * C->scale_t;
    }
    R->texcoord.q = 1.0f;
    R->texcoord.xyz_control = C->fog ? C->fog_lut[V->fog] : 0u;

    uint8_t bank[6];
    bank[0] = V->color.r;
    bank[1] = V->color.g;
    bank[2] = V->color.b;
    bank[3] = V->color.a;
    bank[5] = 0;
    if (gfx_ps2_direct.rec_lod) {
        const float distance_frac = std::max(0.f, std::min(w / 1024.f, 1.f));
        bank[4] = (uint8_t)((0.7f + distance_frac * 0.3f) * 255.f);
    }
    for (int c = 0; c < 4; ++c) {
        const uint8_t src = gfx_ps2_direct.rec_src[c];
        R->rgba[c] = src == 0xff ? gfx_ps2_direct.rec_const[c] : gfx_ps2_direct.rec_lut[c][bank[src]];
    }

    /* DIV on VU treats subnormal W as zero; such batches stay on the EE. */
    gfx_ps2_direct.w_ok[idx] = w >= FLT_MIN && w <= FLT_MAX;
    /* ps2GsClipVertexCodes under this context's guard band (clamped to
     * the exact frustum like ps2GsClipSetGuardBand) */
    const uint32_t codes = ps2GsClipVertexCodesInline(R->clip,
        C->guard_x >= 1.0f ? C->guard_x : 1.0f, C->guard_y >= 1.0f ? C->guard_y : 1.0f);
    gfx_ps2_direct.codes[idx] = codes;
    gfx_ps2_direct.tag[idx] = gfx_ps2_direct.serial;
    return codes;
}

static inline void gfx_ps2_direct_copy_record(struct Ps2GsVu1TransformVertex* dst,
                                              const struct Ps2GsVu1TransformVertex* src) {
#if defined(_EE)
    __asm__ __volatile__("lq $8, 0(%1)\n\t"
                         "lq $9, 16(%1)\n\t"
                         "lq $10, 32(%1)\n\t"
                         "sq $8, 0(%0)\n\t"
                         "sq $9, 16(%0)\n\t"
                         "sq $10, 32(%0)\n\t"
                         :
                         : "r"(dst), "r"(src)
                         : "$8", "$9", "$10", "memory");
#else
    *dst = *src;
#endif
}

/*
 * Triangles that cross the near/far planes or leave the guard band are
 * clipped here, on the 11 values the record carries (clip XYZW, S, T, fog
 * coefficient, RGBA), with the same homogeneous clipper and guard band the
 * VBO path uses on its full vertex. New vertices interpolate the converted
 * colour/fog values rather than the combiner inputs; both conversions are
 * linear, so they agree to the last rounding step.
 */
static void gfx_ps2_direct_begin_batch(int obj_ctx = -1) {
    gfx_ps2_at_flush();
    if (gfx_ps2_direct.batch_count != 0 && gfx_obj.batch_ctx != obj_ctx) {
        gfx_ps2_direct_flush_records();
    }
    if (gfx_ps2_direct.batch_count == 0) {
        gfx_obj.batch_ctx = obj_ctx;
        gfx_ps2_direct.batch_trilerp = gfx_ps2_direct.key.ctx.trilerp != 0;
        gfx_ps2_direct.batch_w_ok = true;
        gfx_ps2_direct.batch_fog[0] = rdp.fog_color.r;
        gfx_ps2_direct.batch_fog[1] = rdp.fog_color.g;
        gfx_ps2_direct.batch_fog[2] = rdp.fog_color.b;
    }
}

static bool gfx_ps2_direct_clip_tri(uint8_t a, uint8_t b, uint8_t c) {
    enum { STRIDE = 11 };
    float in[3 * STRIDE];
    const uint8_t idx[3] = { a, b, c };
    for (int k = 0; k < 3; ++k) {
        const struct Ps2GsVu1TransformVertex* r = &gfx_ps2_direct.records[idx[k]];
        float* o = &in[k * STRIDE];
        o[0] = r->clip[0];
        o[1] = r->clip[1];
        o[2] = r->clip[2];
        o[3] = r->clip[3];
        o[4] = r->texcoord.s;
        o[5] = r->texcoord.t;
        o[6] = (float)(r->texcoord.xyz_control >> 4);
        o[7] = (float)r->rgba[0];
        o[8] = (float)r->rgba[1];
        o[9] = (float)r->rgba[2];
        o[10] = (float)r->rgba[3];
    }
    float out[PS2_GS_CLIP_MAX_OUTPUT_VERTICES * STRIDE];
    size_t count = 0;
    if (!ps2GsClipTriangle(in, STRIDE, out, PS2_GS_CLIP_MAX_OUTPUT_VERTICES, &count)) {
        gfx_ps2_direct_flush();
        return false; /* let the VBO path handle it */
    }
    if (count == 0) {
        return true;
    }
    if (buf_vbo_len > 0) {
        gfx_flush();
    }
    if (gfx_ps2_direct.batch_count + count > GFX_PS2_DIRECT_MAX_VERTICES) {
        gfx_ps2_direct_flush();
    }
    gfx_ps2_direct_begin_batch();
    const bool fog = gfx_ps2_direct.key.ctx.fog != 0;
    for (size_t k = 0; k < count; ++k) {
        const float* o = &out[k * STRIDE];
        struct Ps2GsVu1TransformVertex* r = &gfx_ps2_direct.batch[gfx_ps2_direct.batch_count++];
        r->clip[0] = o[0];
        r->clip[1] = o[1];
        r->clip[2] = o[2];
        r->clip[3] = o[3];
        r->texcoord.s = o[4];
        r->texcoord.t = o[5];
        r->texcoord.q = 1.0f;
        r->texcoord.xyz_control = fog ? (uint32_t)(int)(o[6] + 0.5f) << 4 : 0u;
        for (int ch = 0; ch < 4; ++ch) {
            r->rgba[ch] = (uint32_t)(int)(o[7 + ch] + 0.5f);
        }
        if (!(o[3] >= FLT_MIN && o[3] <= FLT_MAX)) {
            gfx_ps2_direct.batch_w_ok = false;
        }
    }
    if (gfx_ps2_direct.batch_count + 3 > GFX_PS2_DIRECT_MAX_VERTICES) {
        gfx_ps2_direct_flush();
    }
    return true;
}

/* True when the triangle was consumed (buffered or rejected). */
static bool gfx_ps2_direct_tri(const struct GfxTriSetup* S, uint8_t a, uint8_t b, uint8_t c) {
    if (a >= MAX_VERTICES || b >= MAX_VERTICES || c >= MAX_VERTICES) {
        gfx_ps2_direct_flush();
        return false;
    }
    const uint32_t ca = gfx_ps2_direct_vertex(S, a);
    const uint32_t cb = gfx_ps2_direct_vertex(S, b);
    const uint32_t cc = gfx_ps2_direct_vertex(S, c);
    switch (ps2GsClipClassifyCodes(ca, cb, cc)) {
        case PS2_GS_CLIP_REJECT:
            return true;
        case PS2_GS_CLIP_PARTIAL:
            return gfx_ps2_direct_clip_tri(a, b, c);
        default:
            break;
    }
    if (buf_vbo_len > 0) {
        gfx_flush();
    }
    gfx_ps2_direct_begin_batch();
    struct Ps2GsVu1TransformVertex* out = &gfx_ps2_direct.batch[gfx_ps2_direct.batch_count];
    gfx_ps2_direct_copy_record(&out[0], &gfx_ps2_direct.records[a]);
    gfx_ps2_direct_copy_record(&out[1], &gfx_ps2_direct.records[b]);
    gfx_ps2_direct_copy_record(&out[2], &gfx_ps2_direct.records[c]);
    gfx_ps2_direct.batch_w_ok = gfx_ps2_direct.batch_w_ok && gfx_ps2_direct.w_ok[a] &&
                                gfx_ps2_direct.w_ok[b] && gfx_ps2_direct.w_ok[c];
    gfx_ps2_direct.batch_count += 3;
    if (gfx_ps2_direct.batch_count + 3 > GFX_PS2_DIRECT_MAX_VERTICES) {
        gfx_ps2_direct_flush();
    }
    return true;
}
#endif

/* One vertex of the float VBO, exactly as gfx_sp_tri1 lays it out. */
static inline float* gfx_emit_vbo_vertex(const struct GfxTriSetup* S, const struct LoadedVertex* V, float* out) {
    const bool use_alpha = S->use_alpha;
    const int num_inputs = S->num_inputs;
    float z = V->z, w = V->w;
    if (S->depth_source_prim) {
        z = gfx_primitive_depth_to_clip_z(rdp.primitive_depth_z, w);
    }
    if (S->z_is_from_0_to_1) {
        z = (z + w) / 2.0f;
    }

    *out++ = V->x;
    *out++ = S->invert_y ? -V->y : V->y;
    *out++ = z;
    *out++ = w;

    for (int t = 0; t < 2; t++) {
        if (!S->used_textures[t]) {
            continue;
        }
        *out++ = V->u * S->u_scale[t] + S->u_bias[t];
        *out++ = V->v * S->v_scale[t] + S->v_bias[t];
        if (S->clamp_s[t]) {
            *out++ = S->tex_clamp_s[t];
        }
        if (S->clamp_t[t]) {
            *out++ = S->tex_clamp_t[t];
        }
    }

    if (S->use_fog) {
        *out++ = gfx_u8_unit[rdp.fog_color.r];
        *out++ = gfx_u8_unit[rdp.fog_color.g];
        *out++ = gfx_u8_unit[rdp.fog_color.b];
        *out++ = gfx_u8_unit[V->fog]; // fog factor
    }

    if (S->use_grayscale) {
        *out++ = gfx_u8_unit[rdp.grayscale_color.r];
        *out++ = gfx_u8_unit[rdp.grayscale_color.g];
        *out++ = gfx_u8_unit[rdp.grayscale_color.b];
        *out++ = gfx_u8_unit[rdp.grayscale_color.a]; // lerp interpolation factor (not alpha)
    }

    for (int j = 0; j < num_inputs; j++) {
        for (int k = 0; k < 1 + (use_alpha ? 1 : 0); k++) {
            uint8_t r, g, b, a;
            switch (S->in_kind[k][j]) {
                case GFX_TRI_IN_SHADE:
                    r = V->color.r; g = V->color.g; b = V->color.b; a = V->color.a;
                    break;
                case GFX_TRI_IN_SHADE_ALPHA:
                    r = g = b = V->color.a; a = 0;
                    break;
                case GFX_TRI_IN_LOD: {
                    // HACK: very roughly eyeballed based on the carpets in Defection
                    // this is actually supposed to be calculated per pixel
                    const float distance_frac = std::max(0.f, std::min(w / 1024.f, 1.f));
                    r = g = b = a = (uint8_t)((0.7f + distance_frac * 0.3f) * 255.f);
                    break;
                }
                default:
                    if (k == 0) {
                        *out++ = S->in_const[j][0];
                        *out++ = S->in_const[j][1];
                        *out++ = S->in_const[j][2];
                    } else {
                        *out++ = S->in_const[j][3];
                    }
                    continue;
            }
            if (k == 0) {
                *out++ = gfx_u8_unit[r];
                *out++ = gfx_u8_unit[g];
                *out++ = gfx_u8_unit[b];
            } else {
                *out++ = gfx_u8_unit[a];
            }
        }
    }
    return out;
}

#if defined(GFX_PS2_DIRECT)
/*
 * Alpha-trilerp shared-vertex path (see gfxPs2DirectFillAT): each loaded
 * vertex is translated by the backend once per triangle setup; triangles
 * append cached slots to the backend's batch. Like the record path, this
 * batch, the record batch and the float VBO are never non-empty together.
 */
static struct {
    int path; /* gfxPs2DirectQueryAT() for the current setup */
    uint32_t serial;
    uint32_t tag[MAX_VERTICES];
    uint32_t codes[MAX_VERTICES];
    uint8_t fog[3];
    /* Per-setup gather: in[] byte i = bank[map[i]], where bank holds the
     * vertex colour (0..3), the LOD byte (4), 0 (5), 255 (6) and the
     * combiner constants (7..18). */
    uint8_t map[12];
    uint8_t bank[20];
    bool need_lod;
    float gx, gy;
} gfx_ps2_at = { 0, 1 };

static void gfx_ps2_at_build_map(const struct GfxTriSetup* S) {
    memset(gfx_ps2_at.map, 6, sizeof(gfx_ps2_at.map));
    gfx_ps2_at.bank[5] = 0;
    gfx_ps2_at.bank[6] = 255;
    gfx_ps2_at.need_lod = false;
    const int n = S->num_inputs < 3 ? S->num_inputs : 3;
    for (int j = 0; j < n; j++) {
        for (int c = 0; c < 4; c++) {
            gfx_ps2_at.bank[7 + j * 4 + c] = S->in_const_u8[j][c];
        }
        uint8_t* m = &gfx_ps2_at.map[j * 4];
        switch (S->in_kind[0][j]) {
            case GFX_TRI_IN_SHADE: m[0] = 0; m[1] = 1; m[2] = 2; break;
            case GFX_TRI_IN_SHADE_ALPHA: m[0] = m[1] = m[2] = 3; break;
            case GFX_TRI_IN_LOD: m[0] = m[1] = m[2] = 4; gfx_ps2_at.need_lod = true; break;
            default: m[0] = 7 + j * 4; m[1] = 8 + j * 4; m[2] = 9 + j * 4; break;
        }
        if (S->use_alpha) {
            switch (S->in_kind[1][j]) {
                case GFX_TRI_IN_SHADE: m[3] = 3; break;
                case GFX_TRI_IN_SHADE_ALPHA: m[3] = 5; break;
                case GFX_TRI_IN_LOD: m[3] = 4; gfx_ps2_at.need_lod = true; break;
                default: m[3] = 10 + j * 4; break;
            }
        }
    }
}

static inline void gfx_ps2_at_invalidate_vertices(size_t first, size_t count) {
    /* range already clamped by gfx_ps2_direct_invalidate_vertices */
    uint32_t* tag = &gfx_ps2_at.tag[first];
    for (size_t i = 0; i < count; ++i) {
        tag[i] = 0;
    }
}

static void gfx_ps2_at_flush(void) {
    if (gfxPs2DirectATCount() != 0) {
        gfxPs2DirectDrawAT(gfx_ps2_at.fog);
    }
}

/* gfx_emit_vbo_vertex for the alpha-trilerp path, as bytes where the VBO
 * has u8-derived floats (no clamps, grayscale or primitive depth here). */
static inline void gfx_ps2_at_source(const struct GfxTriSetup* S, const struct LoadedVertex* V,
                                     struct Ps2ATSource* out) {
    const float w = V->w;
    out->pos[0] = V->x;
    out->pos[1] = S->invert_y ? -V->y : V->y;
    out->pos[2] = S->z_is_from_0_to_1 ? (V->z + w) / 2.0f : V->z;
    out->pos[3] = w;
    for (int t = 0; t < 2; t++) {
        if (S->used_textures[t]) {
            out->u[t] = V->u * S->u_scale[t] + S->u_bias[t];
            out->v[t] = V->v * S->v_scale[t] + S->v_bias[t];
        }
    }
    out->fog = V->fog;
    uint8_t* bank = gfx_ps2_at.bank;
    bank[0] = V->color.r;
    bank[1] = V->color.g;
    bank[2] = V->color.b;
    bank[3] = V->color.a;
    if (gfx_ps2_at.need_lod) {
        const float distance_frac = std::max(0.f, std::min(w / 1024.f, 1.f));
        bank[4] = (uint8_t)((0.7f + distance_frac * 0.3f) * 255.f);
    }
    const uint8_t* map = gfx_ps2_at.map;
    uint8_t* in = &out->in[0][0];
    for (int i = 0; i < 12; i++) {
        in[i] = bank[map[i]];
    }
}

static bool gfx_ps2_at_clipped_tri(const struct GfxTriSetup* S, uint8_t a, uint8_t b, uint8_t c) {
    float tri[3 * PS2_GS_CLIP_MAX_VERTEX_FLOATS];
    float* end = gfx_emit_vbo_vertex(S, &rsp.loaded_vertices[a], tri);
    const size_t stride = (size_t)(end - tri);
    gfx_emit_vbo_vertex(S, &rsp.loaded_vertices[b], tri + stride);
    gfx_emit_vbo_vertex(S, &rsp.loaded_vertices[c], tri + 2 * stride);
    float out[PS2_GS_CLIP_MAX_OUTPUT_VERTICES * PS2_GS_CLIP_MAX_VERTEX_FLOATS];
    size_t n = 0;
    if (!ps2GsClipTriangle(tri, stride, out, PS2_GS_CLIP_MAX_OUTPUT_VERTICES, &n)) {
        gfx_ps2_at_flush();
        return false;
    }
    if (n < 3) {
        return true;
    }
    if (buf_vbo_len > 0) {
        gfx_flush();
    }
    gfx_ps2_direct_flush_records();
    if (gfxPs2DirectATCount() + n > gfxPs2DirectATCapacity()) {
        gfx_ps2_at_flush();
    }
    if (gfxPs2DirectATCount() == 0) {
        gfx_ps2_at.fog[0] = rdp.fog_color.r;
        gfx_ps2_at.fog[1] = rdp.fog_color.g;
        gfx_ps2_at.fog[2] = rdp.fog_color.b;
    }
    for (size_t t = 0; t + 2 < n + 0 && t < n; t += 3) {
        for (int k = 0; k < 3; ++k) {
            gfxPs2DirectFillAT(PS2_AT_SCRATCH_SLOT + (uint32_t)k, out + (t + (size_t)k) * stride);
        }
        gfxPs2DirectAppendAT(PS2_AT_SCRATCH_SLOT, PS2_AT_SCRATCH_SLOT + 1u, PS2_AT_SCRATCH_SLOT + 2u);
    }
    return true;
}

static bool gfx_ps2_at_tri(const struct GfxTriSetup* S, uint8_t a, uint8_t b, uint8_t c) {
    if (a >= MAX_VERTICES || b >= MAX_VERTICES || c >= MAX_VERTICES) {
        gfx_ps2_at_flush();
        return false;
    }
    const uint8_t idx[3] = { a, b, c };
    for (int k = 0; k < 3; ++k) {
        const uint8_t i = idx[k];
        if (gfx_ps2_at.tag[i] != gfx_ps2_at.serial) {
            struct Ps2ATSource src __attribute__((aligned(16)));
            gfx_ps2_at_source(S, &rsp.loaded_vertices[i], &src);
            gfx_ps2_at.codes[i] = ps2GsClipVertexCodesInline(src.pos, gfx_ps2_at.gx, gfx_ps2_at.gy);
            gfxPs2DirectFillATSource(i, &src);
            gfx_ps2_at.tag[i] = gfx_ps2_at.serial;
        }
    }
    switch (ps2GsClipClassifyCodes(gfx_ps2_at.codes[a], gfx_ps2_at.codes[b], gfx_ps2_at.codes[c])) {
        case PS2_GS_CLIP_REJECT:
            return true;
        case PS2_GS_CLIP_PARTIAL:
            return gfx_ps2_at_clipped_tri(S, a, b, c);
        default:
            break;
    }
    if (buf_vbo_len > 0) {
        gfx_flush();
    }
    gfx_ps2_direct_flush_records();
    if (gfxPs2DirectATCount() + 3 > gfxPs2DirectATCapacity()) {
        gfx_ps2_at_flush();
    }
    if (gfxPs2DirectATCount() == 0) {
        gfx_ps2_at.fog[0] = rdp.fog_color.r;
        gfx_ps2_at.fog[1] = rdp.fog_color.g;
        gfx_ps2_at.fog[2] = rdp.fog_color.b;
    }
    gfxPs2DirectAppendAT(a, b, c);
    return true;
}
#endif

#if defined(GFX_PS2_DIRECT)
static void gfx_obj_update_prog(const struct GfxTriSetup* S);
#endif

/* Build the cached triangle setup (and the PS2 direct/AT state) if stale. */
static inline void gfx_tri_ensure_setup(void) {
    if (gfx_tri_setup_cache.valid) {
        return;
    }
    gfx_tri_setup(&gfx_tri_setup_cache, false);
    gfx_tri_setup_cache.valid = true;
#if defined(GFX_PS2_DIRECT)
    {
        const struct GfxTriSetup* T = &gfx_tri_setup_cache;
        gfxPs2DirectSetATSameCoords(T->used_textures[0] && T->used_textures[1] &&
                                    T->u_scale[0] == T->u_scale[1] && T->u_bias[0] == T->u_bias[1] &&
                                    T->v_scale[0] == T->v_scale[1] && T->v_bias[0] == T->v_bias[1]);
    }
    gfx_ps2_direct_refresh(&gfx_tri_setup_cache);
    if (gfx_ps2_direct.active) {
        gfx_obj_update_prog(&gfx_tri_setup_cache);
    }
    gfx_obj.setup_ok = gfx_ps2_direct.active && gfx_obj.prog_ok && !gfx_ps2_direct.rec_lod;
    if (++gfx_ps2_at.serial == 0) {
        gfx_ps2_at.serial = 1;
    }
    gfx_ps2_at.path = 0;
    if (!gfx_ps2_direct.active && !gfx_tri_setup_cache.depth_source_prim &&
        !gfx_tri_setup_cache.use_grayscale) {
        gfx_ps2_at.path = gfxPs2DirectQueryAT();
        if (gfx_ps2_at.path != 0) {
            gfxPs2DirectUpdateGuard(&gfx_ps2_at.gx, &gfx_ps2_at.gy);
            ps2GsClipGetGuardBand(&gfx_ps2_at.gx, &gfx_ps2_at.gy);
            gfx_ps2_at_build_map(&gfx_tri_setup_cache);
            if (gfxPs2DirectATSameSampleLane()) {
                /* the lane reads only input 1 and tap 0 */
                gfx_ps2_at.need_lod = (gfx_ps2_at.map[4] == 4 || gfx_ps2_at.map[5] == 4 ||
                                       gfx_ps2_at.map[6] == 4 || gfx_ps2_at.map[7] == 4);
            }
        }
    }
#endif
}

#if defined(GFX_PS2_DIRECT)
/* True when ctx X was captured from exactly the transform inputs current now. */
static inline bool gfx_obj_ctx_current(const struct GfxObjCtx* X) {
    const uint64_t* a = (const uint64_t*)X->mp;
    const uint64_t* b = (const uint64_t*)rsp.MP_matrix;
    uint64_t diff = 0;
    for (int i = 0; i < 8; ++i) {
        diff |= a[i] ^ b[i];
    }
    return diff == 0 && X->geometry_mode == rsp.geometry_mode && X->fb_active == fbActive &&
           X->aspect_ofs == rsp.aspect_ofs && X->aspect_scale == rsp.aspect_scale &&
           X->aspect_ratio == gfx_current_dimensions.aspect_ratio && X->tex_s == rsp.texture_scaling_factor.s &&
           X->tex_t == rsp.texture_scaling_factor.t && X->fog_mul == rsp.fog_mul &&
           X->fog_offset == rsp.fog_offset && X->fog_alpha == rdp.fog_color.a && X->viewport.x == rdp.viewport.x &&
           X->viewport.y == rdp.viewport.y && X->viewport.width == rdp.viewport.width &&
           X->viewport.height == rdp.viewport.height;
}

/* Capture the current transform inputs into X and derive everything else. */
static void gfx_obj_ctx_build(struct GfxObjCtx* X) {
    memcpy(X->mp, rsp.MP_matrix, sizeof(X->mp));
    X->fb_active = fbActive;
    X->aspect_ofs = rsp.aspect_ofs;
    X->aspect_scale = rsp.aspect_scale;
    X->aspect_ratio = gfx_current_dimensions.aspect_ratio;
    X->geometry_mode = rsp.geometry_mode;
    X->tex_s = rsp.texture_scaling_factor.s;
    X->tex_t = rsp.texture_scaling_factor.t;
    X->fog_mul = rsp.fog_mul;
    X->fog_offset = rsp.fog_offset;
    X->fog_alpha = rdp.fog_color.a;
    X->viewport = rdp.viewport;
    const struct GfxClipParameters clip = gfx_rapi->get_clip_parameters();
    X->z_is_from_0_to_1 = clip.z_is_from_0_to_1;
    X->invert_y = clip.invert_y;
    float gx, gy;
    gfxPs2DirectPeekGuard(&gx, &gy);
    /* clamped to the exact frustum like the direct path's codes */
    X->guard_x = gx >= 1.0f ? gx : 1.0f;
    X->guard_y = gy >= 1.0f ? gy : 1.0f;

    float ml[4][4]; /* LoadedVertex space: aspect applied */
    const float k = X->aspect_scale / X->aspect_ratio;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            ml[i][j] = X->mp[i][j];
        }
        if (!X->fb_active) {
            ml[i][0] = (X->aspect_ofs * X->mp[i][3] + X->mp[i][0]) * k;
        }
    }
    gfxPs2ObjectCullBasis(ml, X->alpha);
    for (int i = 0; i < 4; ++i) {
        X->xf.m[i][0] = ml[i][0];
        X->xf.m[i][1] = X->invert_y ? -ml[i][1] : ml[i][1];
        X->xf.m[i][2] = X->z_is_from_0_to_1 ? (ml[i][2] + ml[i][3]) / 2.0f : ml[i][2];
        X->xf.m[i][3] = ml[i][3];
    }
    /* Fast3D: fog byte = trunc(clamp(z_raw / w * fog_mul + fog_offset)), or
     * the fog colour's alpha without G_FOG. VU1 sees z' = (z_raw + w) / 2. */
    float fa = 0.0f, fb = X->fog_alpha;
    if (X->geometry_mode & G_FOG) {
        fa = X->z_is_from_0_to_1 ? 2.0f * X->fog_mul : (float)X->fog_mul;
        fb = X->z_is_from_0_to_1 ? (float)X->fog_offset - X->fog_mul : (float)X->fog_offset;
    }
    X->xf.fog[0] = 255.0f;
    X->xf.fog[1] = fb;
    X->xf.fog[2] = -1.0f;
    X->xf.fog[3] = fa;

    gfxPs2ObjectBuildPlanes(X->xf.m, X->guard_x, X->guard_y, 1e-3f, X->pl, X->pa);
    for (int g = 0; g < 3; ++g) {
        for (int lane = 0; lane < 4; ++lane) {
            const int j = g * 4 + lane;
            for (int r = 0; r < 4; ++r) {
                X->vpl[g][r][lane] = X->pl[r][j];
            }
            for (int r = 0; r < 3; ++r) {
                X->vpl[g][4 + r][lane] = X->pa[r][j];
            }
        }
    }
}

/* gfxPs2ObjectClassifyPlanes with the twelve plane ranges on VU0. */
static inline enum Ps2ObjectBoxClass gfx_obj_classify(const struct GfxObjCtx* X, const float lo[3], const float hi[3],
                                                      uint32_t* outside_mask) {
#if defined(_EE)
    float ce[2][4] __attribute__((aligned(16))) = {
        { (lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f, 1.0f },
        { (hi[0] - lo[0]) * 0.5f, (hi[1] - lo[1]) * 0.5f, (hi[2] - lo[2]) * 0.5f, 0.0f },
    };
    float vmin[12] __attribute__((aligned(16)));
    float vmax[12] __attribute__((aligned(16)));
    __asm__ __volatile__("lqc2    $vf1, 0(%0)\n\t"
                         "lqc2    $vf2, 16(%0)\n\t"
                         :
                         : "r"(ce)
                         : "memory");
    for (int g = 0; g < 3; ++g) {
        __asm__ __volatile__("lqc2    $vf3, 0(%0)\n\t"
                             "lqc2    $vf4, 16(%0)\n\t"
                             "lqc2    $vf5, 32(%0)\n\t"
                             "lqc2    $vf6, 48(%0)\n\t"
                             "lqc2    $vf7, 64(%0)\n\t"
                             "lqc2    $vf8, 80(%0)\n\t"
                             "lqc2    $vf9, 96(%0)\n\t"
                             "vmulax.xyzw  $ACC, $vf3, $vf1x\n\t"
                             "vmadday.xyzw $ACC, $vf4, $vf1y\n\t"
                             "vmaddaz.xyzw $ACC, $vf5, $vf1z\n\t"
                             "vmaddw.xyzw  $vf10, $vf6, $vf0w\n\t"
                             "vmulax.xyzw  $ACC, $vf7, $vf2x\n\t"
                             "vmadday.xyzw $ACC, $vf8, $vf2y\n\t"
                             "vmaddz.xyzw  $vf11, $vf9, $vf2z\n\t"
                             "vsub.xyzw    $vf12, $vf10, $vf11\n\t"
                             "vadd.xyzw    $vf13, $vf10, $vf11\n\t"
                             "sqc2    $vf12, 0(%1)\n\t"
                             "sqc2    $vf13, 0(%2)\n\t"
                             :
                             : "r"(X->vpl[g]), "r"(&vmin[g * 4]), "r"(&vmax[g * 4])
                             : "memory");
    }
    uint32_t mask = 0;
    mask |= vmax[7] < 0.0f ? 1u : 0u;
    mask |= vmax[8] < 0.0f ? 2u : 0u;
    mask |= vmax[9] < 0.0f ? 4u : 0u;
    mask |= vmax[10] < 0.0f ? 8u : 0u;
    mask |= vmax[4] < 0.0f ? 16u : 0u;
    mask |= vmax[5] < 0.0f ? 32u : 0u;
    *outside_mask = mask;
    if (mask != 0) {
        return PS2_OBJECT_BOX_OUTSIDE;
    }
    for (int j = 0; j < 7; ++j) {
        if (!(vmin[j] >= 0.0f)) {
            return PS2_OBJECT_BOX_MIXED;
        }
    }
    return PS2_OBJECT_BOX_INSIDE;
#else
    return gfxPs2ObjectClassifyPlanes(X->pl, X->pa, lo, hi, outside_mask);
#endif
}

/* Replay the original G_VTX for slots [first, first + count) of ctx k. */
static void gfx_obj_replay(int k, size_t first, size_t count) {
    const struct GfxObjCtx* X = &gfx_obj.ctxs[k];
    float saved_mp[4][4];
    memcpy(saved_mp, rsp.MP_matrix, sizeof(saved_mp));
    const bool saved_fb = fbActive;
    const float saved_ofs = rsp.aspect_ofs, saved_scale = rsp.aspect_scale;
    const float saved_ratio = gfx_current_dimensions.aspect_ratio;
    const uint32_t saved_geom = rsp.geometry_mode;
    const uint16_t saved_s = rsp.texture_scaling_factor.s, saved_t = rsp.texture_scaling_factor.t;
    const int16_t saved_fm = rsp.fog_mul, saved_fo = rsp.fog_offset;
    const uint8_t saved_fa = rdp.fog_color.a;
    const struct NormalColor* saved_cols = rsp.vertex_colors;

    memcpy(rsp.MP_matrix, X->mp, sizeof(X->mp));
    fbActive = X->fb_active;
    rsp.aspect_ofs = X->aspect_ofs;
    rsp.aspect_scale = X->aspect_scale;
    gfx_current_dimensions.aspect_ratio = X->aspect_ratio;
    rsp.geometry_mode = X->geometry_mode;
    rsp.texture_scaling_factor.s = X->tex_s;
    rsp.texture_scaling_factor.t = X->tex_t;
    rsp.fog_mul = X->fog_mul;
    rsp.fog_offset = X->fog_offset;
    rdp.fog_color.a = X->fog_alpha;

    gfx_obj.materializing = true;
    /* one call per run of slots that came from one G_VTX source array */
    size_t i = first;
    while (i < first + count) {
        size_t j = i + 1;
        while (j < first + count && gfx_obj.src[j] == gfx_obj.src[j - 1] + 1 &&
               gfx_obj.cols[j] == gfx_obj.cols[i]) {
            ++j;
        }
        rsp.vertex_colors = gfx_obj.cols[i];
        gfx_sp_vertex(j - i, i, gfx_obj.src[i]);
        i = j;
    }
    gfx_obj.materializing = false;
    ++gfx_obj.n_replays;

    memcpy(rsp.MP_matrix, saved_mp, sizeof(saved_mp));
    fbActive = saved_fb;
    rsp.aspect_ofs = saved_ofs;
    rsp.aspect_scale = saved_scale;
    gfx_current_dimensions.aspect_ratio = saved_ratio;
    rsp.geometry_mode = saved_geom;
    rsp.texture_scaling_factor.s = saved_s;
    rsp.texture_scaling_factor.t = saved_t;
    rsp.fog_mul = saved_fm;
    rsp.fog_offset = saved_fo;
    rdp.fog_color.a = saved_fa;
    rsp.vertex_colors = saved_cols;
}

/* Bring every deferred slot of ctx k up to date in loaded_vertices[]. */
static void gfx_obj_materialize_ctx(int k) {
    size_t i = 0;
    while (i < MAX_VERTICES) {
        if (gfx_obj.state[i] == GFX_OBJ_NORMAL || gfx_obj.ctx[i] != k) {
            ++i;
            continue;
        }
        size_t j = i + 1;
        while (j < MAX_VERTICES && gfx_obj.state[j] != GFX_OBJ_NORMAL && gfx_obj.ctx[j] == k) {
            ++j;
        }
        gfx_obj_replay(k, i, j - i); /* sets the slots back to NORMAL */
        i = j;
    }
}

static void gfx_obj_materialize_all(void) {
    for (size_t i = 0; i < MAX_VERTICES; ++i) {
        if (gfx_obj.state[i] != GFX_OBJ_NORMAL) {
            gfx_obj_materialize_ctx(gfx_obj.ctx[i]);
        }
    }
}

/* Object-space bounds of n vertices (x, y, z of each Vtx). */
static inline void gfx_obj_bounds(const Vtx* v, size_t n, float lo[3], float hi[3]) {
#if defined(_EE)
    /* MMI parallel halfword min/max over x, y, z (lane 3 is flags/colour) */
    typedef unsigned int qword_t __attribute__((mode(TI)));
    uint64_t first;
    memcpy(&first, &v[0], 8);
    qword_t qlo = first, qhi = first;
    for (size_t i = 1; i < n; ++i) {
        uint64_t raw;
        memcpy(&raw, &v[i], 8);
        const qword_t q = raw;
        __asm__("pminh %0, %1, %2" : "=r"(qlo) : "r"(qlo), "r"(q));
        __asm__("pmaxh %0, %1, %2" : "=r"(qhi) : "r"(qhi), "r"(q));
    }
    const uint64_t l = (uint64_t)qlo, h = (uint64_t)qhi;
    for (int k = 0; k < 3; ++k) {
        lo[k] = (float)(int16_t)(l >> (16 * k));
        hi[k] = (float)(int16_t)(h >> (16 * k));
    }
#else
    int l[3] = { v[0].v[0], v[0].v[1], v[0].v[2] };
    int h[3] = { l[0], l[1], l[2] };
    for (size_t i = 1; i < n; ++i) {
        for (int k = 0; k < 3; ++k) {
            const int c = v[i].v[k];
            l[k] = c < l[k] ? c : l[k];
            h[k] = c > h[k] ? c : h[k];
        }
    }
    for (int k = 0; k < 3; ++k) {
        lo[k] = (float)l[k];
        hi[k] = (float)h[k];
    }
#endif
}

static bool gfx_obj_try_defer(size_t n_vertices, size_t dest_index, const Vtx* vertices) {
    if (!gfx_obj.enabled || gfx_obj.materializing || !gfx_obj.setup_ok || n_vertices == 0 ||
        dest_index >= MAX_VERTICES ||
        n_vertices > MAX_VERTICES - dest_index || (rsp.geometry_mode & G_LIGHTING) != 0 ||
        rsp.vertex_colors == nullptr) {
        return false;
    }

    /* Reuse the last context when nothing the transform reads changed. */
    int k = gfx_obj.last_ctx;
    if (k < 0 || !gfx_obj_ctx_current(&gfx_obj.ctxs[k])) {
        k = (int)(gfx_obj.next_ctx++ % GFX_OBJ_CTX);
        /* the slot is recycled: nothing may still refer to it */
        if (gfx_obj.batch_ctx == k) {
            gfx_ps2_direct_flush_records();
        }
        gfx_obj_materialize_ctx(k);
        gfx_obj_ctx_build(&gfx_obj.ctxs[k]);
        gfx_obj.last_ctx = k;
    }
    const struct GfxObjCtx* X = &gfx_obj.ctxs[k];

    float lo[3], hi[3];
    gfx_obj_bounds(vertices, n_vertices, lo, hi);
    uint32_t mask = 0;
    const enum Ps2ObjectBoxClass cls = gfx_obj_classify(X, lo, hi, &mask);
    if (cls == PS2_OBJECT_BOX_MIXED) {
        ++gfx_obj.n_mixed;
        return false;
    }
    cls == PS2_OBJECT_BOX_INSIDE ? ++gfx_obj.n_inside : ++gfx_obj.n_outside;

    /* No direct/AT tag invalidation here: those caches are only read for
     * NORMAL slots, and materializing goes through gfx_sp_vertex again. */
    const uint8_t state = cls == PS2_OBJECT_BOX_INSIDE ? GFX_OBJ_INSIDE : GFX_OBJ_OUTSIDE;
    const struct NormalColor* cols = rsp.vertex_colors;
    for (size_t i = 0; i < n_vertices; ++i) {
        const size_t d = dest_index + i;
        gfx_obj.src[d] = &vertices[i];
        gfx_obj.cols[d] = cols;
        gfx_obj.state[d] = state;
        gfx_obj.mask[d] = (uint8_t)mask;
        gfx_obj.ctx[d] = (uint8_t)k;
    }
    return true;
}

/*
 * Record program of the current direct key for VU1: texture coordinate
 * scale/bias and the colour channel program, which is linear because every
 * direct colour table is round(i / 255 * K). False when it is not
 * expressible (LOD input or an unknown table).
 */
static void gfx_obj_update_prog(const struct GfxTriSetup* S) {
    const struct GfxPs2DirectContext* C = &gfx_ps2_direct.key.ctx;
    struct Ps2GsVu1ObjectTransform* P = &gfx_obj.prog;
    bool ok = true;
    memset(P->color, 0, sizeof(P->color));
    for (int c = 0; c < 4; ++c) {
        const uint8_t src = gfx_ps2_direct.rec_src[c];
        if (src == 0xff) {
            P->color_add[c] = gfx_ps2_direct.rec_const[c] + 0.5f;
        } else if (src == 5) {
            P->color_add[c] = gfx_ps2_direct.rec_lut[c][0] + 0.5f;
        } else if (src <= 3) {
            float k;
            if (!gfxPs2DirectLutScale(gfx_ps2_direct.rec_lut[c], &k)) {
                ok = false;
            }
            P->color[src][c] = k;
            P->color_add[c] = 0.5f;
        } else {
            ok = false; /* LOD */
        }
    }
    for (int i = 0; i < 4; ++i) {
        P->tex_mul[i] = P->tex_add[i] = P->tex_scale[i] = 0.0f;
    }
    if (!C->color_only) {
        P->tex_mul[0] = S->u_scale[0];
        P->tex_mul[1] = S->v_scale[0];
        P->tex_add[0] = S->u_bias[0];
        P->tex_add[1] = S->v_bias[0];
        P->tex_scale[0] = C->scale_s;
        P->tex_scale[1] = C->scale_t;
    }
    gfx_obj.prog_ok = ok;
    gfx_obj.prog_serial = gfx_ps2_direct.serial;
}

/* Batch constants: the ctx's matrix and fog with the key's record program. */
static void gfx_obj_start_batch(int k) {
    struct Ps2GsVu1ObjectTransform* B = &gfx_obj.batch_xf;
    *B = gfx_obj.prog;
    memcpy(B->m, gfx_obj.ctxs[k].xf.m, sizeof(B->m));
    memcpy(B->fog, gfx_obj.ctxs[k].xf.fog, sizeof(B->fog));
    if (!gfx_ps2_direct.key.ctx.fog) {
        /* records without a fog coefficient carry 0 in the fog lane */
        B->fog[1] = 255.0f;
        B->fog[3] = 0.0f;
    }
}

/* The raw vertex VU1 builds the record from (struct Ps2GsVu1ObjectVertex). */
__attribute__((always_inline)) static inline void gfx_obj_pack(uint8_t idx, const struct GfxObjCtx* X, struct Ps2GsVu1ObjectVertex* o) {
    const Vtx* v = gfx_obj.src[idx];
    const struct NormalColor col = gfx_obj.cols[idx][v->colour >> 2];
    o->pos[0] = v->v[0];
    o->pos[1] = v->v[1];
    o->pos[2] = v->v[2];
    o->pos[3] = 0;
    /* LoadedVertex u/v exactly as gfx_sp_vertex derives them */
    o->uv[0] = (short)(v->s * (int32_t)X->tex_s >> 16);
    o->uv[1] = (short)(v->t * (int32_t)X->tex_t >> 16);
    o->uv[2] = 0;
    o->uv[3] = 0;
    o->rgba[0] = col.r;
    o->rgba[1] = col.g;
    o->rgba[2] = col.b;
    o->rgba[3] = col.a;
}

__attribute__((always_inline)) static inline void gfx_obj_pos(uint8_t idx, float p[3]) {
    const Vtx* v = gfx_obj.src[idx];
    p[0] = v->v[0];
    p[1] = v->v[1];
    p[2] = v->v[2];
}

/*
 * Triangle over at least one deferred slot. True when consumed (rejected,
 * culled or buffered in an object batch); false after bringing the slots up
 * to date for the usual path.
 */
static bool gfx_obj_tri(uint8_t a, uint8_t b, uint8_t c) {
    const uint8_t sa = gfx_obj.state[a], sb = gfx_obj.state[b], sc = gfx_obj.state[c];
    if (sa != GFX_OBJ_NORMAL && sb != GFX_OBJ_NORMAL && sc != GFX_OBJ_NORMAL) {
        /* every vertex outside one exact plane: Fast3D rejects it too */
        if ((gfx_obj.mask[a] & gfx_obj.mask[b] & gfx_obj.mask[c]) != 0) {
            return true;
        }
        const int k = gfx_obj.ctx[a];
        if (sa == GFX_OBJ_INSIDE && sb == GFX_OBJ_INSIDE && sc == GFX_OBJ_INSIDE && gfx_obj.ctx[b] == k &&
            gfx_obj.ctx[c] == k) {
            const struct GfxObjCtx* X = &gfx_obj.ctxs[k];
            const uint32_t cull_mode = rsp.geometry_mode & G_CULL_BOTH;
#if defined(PERFECT_DARK_PS2_DISABLE_CULLING)
            if (false) {
#else
            if (cull_mode != 0) {
#endif
                float pa[3], pb[3], pc[3];
                gfx_obj_pos(a, pa);
                gfx_obj_pos(b, pb);
                gfx_obj_pos(c, pc);
                const float cross = gfxPs2ObjectCullCross(X->alpha, pa, pb, pc);
                switch (cull_mode) {
                    case G_CULL_FRONT:
                        if (cross <= 0) {
                            return true;
                        }
                        break;
                    case G_CULL_BACK:
                        if (cross >= 0) {
                            return true;
                        }
                        break;
                    case G_CULL_BOTH:
                        return true;
                }
            }
            gfx_tri_ensure_setup();
            const struct GfxTriSetup* S = &gfx_tri_setup_cache;
            const struct GfxPs2DirectContext* C = &gfx_ps2_direct.key.ctx;
            if (gfx_obj.fog_linear < 0) {
                gfx_obj.fog_linear = gfxPs2DirectFogLinear() ? 1 : 0;
            }
            if (gfx_ps2_direct.active && gfx_obj.prog_serial != gfx_ps2_direct.serial) {
                gfx_obj_update_prog(S);
            }
            if (gfx_ps2_direct.active && gfx_obj.prog_ok && !gfx_ps2_direct.rec_lod &&
                S->z_is_from_0_to_1 == (X->z_is_from_0_to_1 != 0) && S->invert_y == (X->invert_y != 0) &&
                C->guard_x >= X->guard_x && C->guard_y >= X->guard_y && (!C->fog || gfx_obj.fog_linear == 1)) {
                if (buf_vbo_len > 0) {
                    gfx_flush();
                }
                if (gfx_ps2_direct.batch_count == 0 || gfx_obj.batch_ctx != k) {
                    gfx_ps2_direct_begin_batch(k);
                    gfx_obj_start_batch(k);
                }
                struct Ps2GsVu1ObjectVertex* out =
                    (struct Ps2GsVu1ObjectVertex*)&gfx_ps2_direct.batch[gfx_ps2_direct.batch_count];
                gfx_obj_pack(a, X, &out[0]);
                gfx_obj_pack(b, X, &out[1]);
                gfx_obj_pack(c, X, &out[2]);
                gfx_ps2_direct.batch_count += 3;
                ++gfx_obj.n_tris;
                if (gfx_ps2_direct.batch_count + 3 > GFX_OBJ_MAX_VERTICES) {
                    gfx_ps2_direct_flush();
                }
                return true;
            }
        }
    }
    if (sa != GFX_OBJ_NORMAL) {
        gfx_obj_materialize_ctx(gfx_obj.ctx[a]);
    }
    if (gfx_obj.state[b] != GFX_OBJ_NORMAL) {
        gfx_obj_materialize_ctx(gfx_obj.ctx[b]);
    }
    if (gfx_obj.state[c] != GFX_OBJ_NORMAL) {
        gfx_obj_materialize_ctx(gfx_obj.ctx[c]);
    }
    return false;
}
#endif

/*
 * Everything after culling. Kept out of line so the culling front end
 * (about half of all triangles end there) inlines into the G_TRI1/G_TRI4
 * handlers without paying this body's large frame and register saves.
 */
__attribute__((noinline)) static void gfx_sp_tri1_emit(uint8_t vtx1_idx, uint8_t vtx2_idx, uint8_t vtx3_idx, bool is_rect) {
    struct LoadedVertex* v1 = &rsp.loaded_vertices[vtx1_idx];
    struct LoadedVertex* v2 = &rsp.loaded_vertices[vtx2_idx];
    struct LoadedVertex* v3 = &rsp.loaded_vertices[vtx3_idx];
    struct LoadedVertex* v_arr[3] = { v1, v2, v3 };
    (void)v_arr;

    struct GfxTriSetup rect_setup;
    const struct GfxTriSetup* S = &gfx_tri_setup_cache;
    if (is_rect) {
        gfx_tri_setup(&rect_setup, true);
        S = &rect_setup;
    } else {
        gfx_tri_ensure_setup();
    }
#if defined(GFX_PS2_DIRECT) && defined(PD_PS2_DIRECT_VERIFY)
    /* Verification build: draw through the VBO, but record what the direct
     * path would have produced for the backend to compare against. */
    gfx_ps2_direct_verify_valid[buf_vbo_num_tris] = 0;
    if (!is_rect && gfx_ps2_direct.active && vtx1_idx < MAX_VERTICES && vtx2_idx < MAX_VERTICES &&
        vtx3_idx < MAX_VERTICES) {
        const uint32_t ca = gfx_ps2_direct_vertex(&gfx_tri_setup_cache, vtx1_idx);
        const uint32_t cb = gfx_ps2_direct_vertex(&gfx_tri_setup_cache, vtx2_idx);
        const uint32_t cc = gfx_ps2_direct_vertex(&gfx_tri_setup_cache, vtx3_idx);
        const enum Ps2GsClipClass cls = ps2GsClipClassifyCodes(ca, cb, cc);
        gfx_ps2_direct_verify_valid[buf_vbo_num_tris] = (uint8_t)(cls == PS2_GS_CLIP_ACCEPT ? 1 : 2 + (int)cls);
        gfx_ps2_direct_verify_records[buf_vbo_num_tris * 3 + 0] = gfx_ps2_direct.records[vtx1_idx];
        gfx_ps2_direct_verify_records[buf_vbo_num_tris * 3 + 1] = gfx_ps2_direct.records[vtx2_idx];
        gfx_ps2_direct_verify_records[buf_vbo_num_tris * 3 + 2] = gfx_ps2_direct.records[vtx3_idx];
    }
    gfx_ps2_direct_verify_vbo = buf_vbo;
#elif defined(GFX_PS2_DIRECT)
    if (!is_rect && gfx_ps2_direct.active &&
        gfx_ps2_direct_tri(&gfx_tri_setup_cache, vtx1_idx, vtx2_idx, vtx3_idx)) {
        return;
    }
    if (!is_rect && gfx_ps2_at.path != 0 &&
        gfx_ps2_at_tri(&gfx_tri_setup_cache, vtx1_idx, vtx2_idx, vtx3_idx)) {
        return;
    }
    gfx_ps2_direct_flush();
#endif

    for (int i = 0; i < 3; i++) {
        float* out = gfx_emit_vbo_vertex(S, v_arr[i], &buf_vbo[buf_vbo_len]);
        buf_vbo_len = (size_t)(out - buf_vbo);
    }

    if (++buf_vbo_num_tris == MAX_BUFFERED) {
        gfx_flush();
    }
}

static inline void gfx_sp_tri1(uint8_t vtx1_idx, uint8_t vtx2_idx, uint8_t vtx3_idx, bool is_rect) {
#if defined(GFX_PS2_DIRECT)
    if (!is_rect && vtx1_idx < MAX_VERTICES && vtx2_idx < MAX_VERTICES && vtx3_idx < MAX_VERTICES &&
        (gfx_obj.state[vtx1_idx] | gfx_obj.state[vtx2_idx] | gfx_obj.state[vtx3_idx]) != 0 &&
        gfx_obj_tri(vtx1_idx, vtx2_idx, vtx3_idx)) {
        return;
    }
#endif
    struct LoadedVertex* v1 = &rsp.loaded_vertices[vtx1_idx];
    struct LoadedVertex* v2 = &rsp.loaded_vertices[vtx2_idx];
    struct LoadedVertex* v3 = &rsp.loaded_vertices[vtx3_idx];

    if ((rsp.extra_geometry_mode & G_NO_CLIPPING_EXT) == 0) {
        if (v1->clip_rej & v2->clip_rej & v3->clip_rej) {
            // The whole triangle lies outside the visible area
            return;
        }
    }

    if ((rsp.geometry_mode & G_CULL_BOTH) != 0) {
#if defined(PLATFORM_PS2)
        /*
         * Division-free form of the test below. For non-zero W the projected
         * cross product equals -det[x y w] / (w1 w2 w3), and the eye-plane
         * flip multiplies by sign(w1 w2 w3) again, so the flipped cross has
         * the sign of -det. Six FPU divides per triangle become nine
         * multiplies (the EE's DIV.S is not pipelined).
         */
        float cross = v2->x * (v1->y * v3->w - v3->y * v1->w) - v1->x * (v2->y * v3->w - v3->y * v2->w) -
                      v3->x * (v1->y * v2->w - v2->y * v1->w);
#else
        /* v2's projection is shared by both edges; compute it once. */
        const float x2 = v2->x / (v2->w);
        const float y2 = v2->y / (v2->w);
        float dx1 = v1->x / (v1->w) - x2;
        float dy1 = v1->y / (v1->w) - y2;
        float dx2 = v3->x / (v3->w) - x2;
        float dy2 = v3->y / (v3->w) - y2;
        float cross = dx1 * dy2 - dy1 * dx2;

        if ((v1->w < 0) ^ (v2->w < 0) ^ (v3->w < 0)) {
            // If one vertex lies behind the eye, negating cross will give the correct result.
            // If all vertices lie behind the eye, the triangle will be rejected anyway.
            cross = -cross;
        }
#endif

        // If inverted culling is requested, negate the cross
        // if ((rsp.extra_geometry_mode & G_EX_INVERT_CULLING) == 1) {
        //     cross = -cross;
        // }

        switch (rsp.geometry_mode & G_CULL_BOTH) {
            case G_CULL_FRONT:
                if (cross <= 0) {
                    return;
                }
                break;
            case G_CULL_BACK:
                if (cross >= 0) {
                    return;
                }
                break;
            case G_CULL_BOTH:
                // Why is this even an option?
                return;
        }
    }

    gfx_sp_tri1_emit(vtx1_idx, vtx2_idx, vtx3_idx, is_rect);
}

static inline void gfx_sp_tri4(Gfx *cmd) {
    // the game issues gSPTri2 for quads, which uses G_TRI4 with 2 empty triangles
    uint8_t x = C1(0, 4);
    uint8_t y = C1(4, 4);
    uint8_t z = C0(0, 4);

    if(x || y || z) {
        gfx_sp_tri1(x, y, z, false);
    }

    x = C1(8, 4);
    y = C1(12, 4);
    z = C0(4, 4);

    if (x || y || z) {
        gfx_sp_tri1(x, y, z, false);
    }

    x = C1(16, 4);
    y = C1(20, 4);
    z = C0(8, 4);

    if (x || y || z) {
        gfx_sp_tri1(x, y, z, false);
    }

    x = C1(24, 4);
    y = C1(28, 4);
    z = C0(12, 4);

    if (x || y || z) {
        gfx_sp_tri1(x, y, z, false);
    }
}

static void gfx_sp_geometry_mode(uint32_t clear, uint32_t set) {
    rsp.geometry_mode &= ~clear;
    rsp.geometry_mode |= set;
}

static inline void gfx_update_aspect_mode(void) {
    const uint32_t side = rsp.aspect_mode & G_ASPECT_CENTER_EXT;

    rsp.aspect_scale = rsp.aspect_mode ? gfx_current_native_aspect : gfx_current_window_dimensions.aspect_ratio;

    if (side == G_ASPECT_LEFT_EXT) {
        rsp.aspect_ofs = 1.f - gfx_current_dimensions.aspect_ratio / gfx_current_native_aspect;
    } else if (side == G_ASPECT_RIGHT_EXT) {
        rsp.aspect_ofs = gfx_current_dimensions.aspect_ratio / gfx_current_native_aspect - 1.f;
    } else {
        rsp.aspect_ofs = 0.f;
    }

    if (side && (rsp.aspect_mode & G_ASPECT_WIDE_EXT)) {
        constexpr float c = 16.f / 9.f;
        if (gfx_current_dimensions.aspect_ratio > c) {
            rsp.aspect_ofs *= c / gfx_current_dimensions.aspect_ratio;
        }
    }
}

static void gfx_sp_extra_geometry_mode(uint32_t clear, uint32_t set) {
    rsp.extra_geometry_mode &= ~clear;
    rsp.extra_geometry_mode |= set;
    rsp.aspect_mode = (rsp.extra_geometry_mode & G_ASPECT_MODE_EXT);
    gfx_update_aspect_mode();
}

static void gfx_adjust_viewport_or_scissor(XYWidthHeight* area, bool preserve_aspect = false) {
    // HACK: assume all target framebuffers have the same aspect
    // Use floor/ceil to ensure scissor fully contains the logical region
    // and prevents sub-pixel gaps at viewport edges
    float x1 = area->x * RATIO_X;
    float y1 = (SCREEN_HEIGHT - area->y) * RATIO_Y;
    float x2 = (area->x + area->width) * RATIO_X;
    float y2 = (SCREEN_HEIGHT - area->y + area->height) * RATIO_Y;
    
    area->x = std::floor(x1);
    area->y = std::floor(y1);
    area->width = std::ceil(x2) - area->x;
    area->height = std::ceil(y2) - area->y;
    
    if (preserve_aspect) {
        // preserve native aspect ratio
        const float ratio = gfx_current_native_aspect / gfx_current_dimensions.aspect_ratio;
        const float midx = gfx_current_dimensions.width * 0.5f;
        area->x = midx + (area->x - midx) * ratio;
        area->x += rsp.aspect_ofs * gfx_current_dimensions.width * 0.5f;
        area->width *= ratio;
    }

    if (!game_renders_to_framebuffer ||
        (gfx_msaa_level > 1 && gfx_current_dimensions.width == gfx_current_game_window_viewport.width &&
            gfx_current_dimensions.height == gfx_current_game_window_viewport.height)) {
        area->x += gfx_current_game_window_viewport.x;
        area->y += gfx_current_window_dimensions.height -
                    (gfx_current_game_window_viewport.y + gfx_current_game_window_viewport.height);
    }
}

static void gfx_calc_and_set_viewport(const Vp_t* viewport) {
    // 2 bits fraction
    float width = 2.0f * viewport->vscale[0] / 4.0f;
    float height = 2.0f * viewport->vscale[1] / 4.0f;
    float x = (viewport->vtrans[0] / 4.0f) - width / 2.0f;
    float y = ((viewport->vtrans[1] / 4.0f) + height / 2.0f);

    rdp.viewport.x = x;
    rdp.viewport.y = y;
    rdp.viewport.width = width;
    rdp.viewport.height = height;

    gfx_adjust_viewport_or_scissor(&rdp.viewport);

    rdp.viewport_or_scissor_changed = true;
}

static void gfx_sp_movemem(uint8_t index, uint8_t offset, const void* data) {
    switch (index) {
        case G_MV_VIEWPORT:
            gfx_calc_and_set_viewport((const Vp_t*)data);
            break;
        case G_MV_LOOKATY:
        case G_MV_LOOKATX:
            // I think this is only really used for guLookAtReflect
            index = !((index - G_MV_LOOKATY) / 2);
            rsp.lookat[index] = ((const Light *)data)->l;
            rsp.lookat_enabled = (index == 0) || (rsp.lookat[1].dir[0] || rsp.lookat[1].dir[1]);
            rsp.lights_changed = true;
            break;
        case G_MV_L0:
        case G_MV_L1:
        case G_MV_L2:
            // NOTE: reads out of bounds if it is an ambient light
            memcpy(rsp.current_lights + (index - G_MV_L0) / 2, data, sizeof(Light_t));
            break;
    }
}

static void gfx_sp_moveword(uint8_t index, uint16_t offset, uintptr_t data) {
    switch (index) {
        case G_MW_NUMLIGHT:
            // Ambient light is included
            // The 31th bit is a flag that lights should be recalculated
            rsp.current_num_lights = (data - 0x80000000U) / 32;
            rsp.lights_changed = 1;
            break;
        case G_MW_FOG:
            rsp.fog_mul = (int16_t)(data >> 16);
            rsp.fog_offset = (int16_t)data;
            break;
        case G_MW_SEGMENT:
            segmentPointers[(offset >> 2) & 0xff] = data;
            break;
    }
}

static void gfx_sp_texture(uint16_t sc, uint16_t tc, uint8_t level, uint8_t tile, uint8_t on) {
    rsp.texture_scaling_factor.s = sc;
    rsp.texture_scaling_factor.t = tc;
    rdp.tex_max_lod = level;
    if (rdp.first_tile_index != tile) {
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
        rdp.first_tile_index = tile;
    }
}

static void gfx_dp_set_scissor(uint32_t mode, uint32_t ulx, uint32_t uly, uint32_t lrx, uint32_t lry) {
    float x = ulx / 4.0f;
    float y = lry / 4.0f;
    float width = (lrx - ulx) / 4.0f;
    float height = (lry - uly) / 4.0f;

    rdp.scissor.x = x;
    rdp.scissor.y = y;
    rdp.scissor.width = width;
    rdp.scissor.height = height;

    gfx_adjust_viewport_or_scissor(&rdp.scissor, rsp.aspect_mode != 0);

    rdp.viewport_or_scissor_changed = true;
}

static void gfx_dp_set_texture_image(uint32_t format, uint32_t size, uint32_t width, uint32_t tex_flags, const void* addr) {
    rdp.texture_to_load.addr = (const uint8_t*)addr;
    rdp.texture_to_load.siz = size;
    rdp.texture_to_load.width = width;
    rdp.texture_to_load.tex_flags = tex_flags;
}

static void gfx_dp_set_tile(uint8_t fmt, uint32_t siz, uint32_t line, uint32_t tmem, uint8_t tile, uint32_t palette,
                            uint32_t cmt, uint32_t maskt, uint32_t shiftt, uint32_t cms, uint32_t masks,
                            uint32_t shifts) {
    // OTRTODO:
    // SUPPORT_CHECK(tmem == 0 || tmem == 256);
    static uint32_t max_tmem = 0;
    if (cms == G_TX_WRAP && masks == G_TX_NOMASK) {
        cms = G_TX_CLAMP;
    }
    if (cmt == G_TX_WRAP && maskt == G_TX_NOMASK) {
        cmt = G_TX_CLAMP;
    }

    if (fmt == G_IM_FMT_RGBA && siz < G_IM_SIZ_16b) {
        // HACK: sometimes the game will submit G_IM_FMT_RGBA, G_IM_SIZ_8b/4b, intending it to read as CI8/CI4 with RGBA16 palette
        fmt = G_IM_FMT_CI;
    } else if (fmt == G_IM_FMT_IA && siz == G_IM_SIZ_32b) {
        // HACK: ... and sometimes it submits this, apparently intending it to be I8
        fmt = G_IM_FMT_I;
        siz = G_IM_SIZ_8b;
    }

    rdp.texture_tile[tile].palette = palette; // palette should set upper 4 bits of color index in 4b mode
    rdp.texture_tile[tile].fmt = fmt;
    rdp.texture_tile[tile].siz = siz;
    rdp.texture_tile[tile].cms = cms;
    rdp.texture_tile[tile].cmt = cmt;
    rdp.texture_tile[tile].shifts = shifts;
    rdp.texture_tile[tile].shiftt = shiftt;
    rdp.texture_tile[tile].line_size_bytes = line * 8;
    rdp.texture_tile[tile].tmem = tmem;

    rdp.textures_changed[0] = true;
    rdp.textures_changed[1] = true;
}

static void gfx_dp_set_tile_size(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) {
    rdp.texture_tile[tile].uls = uls;
    rdp.texture_tile[tile].ult = ult;
    rdp.texture_tile[tile].lrs = lrs;
    rdp.texture_tile[tile].lrt = lrt;
    rdp.texture_tile[tile].width = (lrs - uls + 4) / 4;
    rdp.texture_tile[tile].height = (lrt - ult + 4) / 4;
    rdp.textures_changed[0] = true;
    rdp.textures_changed[1] = true;
}

static void gfx_dp_load_tlut(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    // SUPPORT_CHECK(tile == G_TX_LOADTILE);
    SUPPORT_CHECK(rdp.texture_to_load.siz == G_IM_SIZ_16b);
    SUPPORT_CHECK(rdp.texture_tile[tile].tmem >= 256);

    rdp.texture_tile[tile].uls = uls;
    rdp.texture_tile[tile].ult = ult;
    rdp.texture_tile[tile].lrs = lrs;
    rdp.texture_tile[tile].lrt = lrt;

    const uint32_t width = (lrs - uls + 1);
    const uint32_t height = (lrt - ult + 1);
    const uint32_t pitch = rdp.texture_to_load.width + 1;
    const uint32_t count =  width * height;
    const uint16_t *base = (const uint16_t *)rdp.texture_to_load.addr + pitch * ult + uls;

    if (rdp.texture_tile[tile].tmem == 256) {
        rdp.palette_addrs[0] = (const uint8_t *)base;
        if (count >= 256) {
            rdp.palette_addrs[1] = (const uint8_t *)(base + 128);
        }
    } else {
        rdp.palette_addrs[1] = (const uint8_t *)base;
    }

    const uint32_t palofs = rdp.texture_tile[tile].tmem - 256;
    SUPPORT_CHECK(palofs + count <= 256);

    const uint16_t *src = base;
    uint16_t *dst = rdp.palette + palofs;
    for (uint32_t i = 0; i < count; ++i) {
        *dst++ = PD_BE16(*src++);
    }

    rdp.textures_changed[0] = rdp.textures_changed[1] = true;
}

static void gfx_dp_load_block(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t dxt) {
    // SUPPORT_CHECK(tile == G_TX_LOADTILE);
    SUPPORT_CHECK(uls == 0);
    SUPPORT_CHECK(ult == 0);

    // The lrs field rather seems to be number of pixels to load
    uint32_t orig_size_bytes = (lrs + 1) << rdp.texture_to_load.siz >> 1;
    uint32_t size_bytes = orig_size_bytes;
    if (rdp.texture_to_load.raw_tex_metadata.h_byte_scale != 1 ||
        rdp.texture_to_load.raw_tex_metadata.v_pixel_scale != 1) {
        size_bytes *= rdp.texture_to_load.raw_tex_metadata.h_byte_scale;
        size_bytes *= rdp.texture_to_load.raw_tex_metadata.v_pixel_scale;
    }

    LoadedTexture& loaded_texture = rdp.loaded_texture[rdp.texture_tile[tile].tmem];
    loaded_texture.orig_size_bytes = orig_size_bytes;
    loaded_texture.size_bytes = size_bytes;
    loaded_texture.full_size_bytes = size_bytes;
    loaded_texture.line_size_bytes = size_bytes;
    loaded_texture.full_image_line_size_bytes = size_bytes;
    loaded_texture.tex_flags = rdp.texture_to_load.tex_flags;
    loaded_texture.raw_tex_metadata = rdp.texture_to_load.raw_tex_metadata;
    loaded_texture.addr = rdp.texture_to_load.addr;

    rdp.textures_changed[0] = rdp.textures_changed[1] = true;
}

static void gfx_dp_load_tile(uint8_t tile, uint32_t uls, uint32_t ult, uint32_t lrs, uint32_t lrt) {
    SUPPORT_CHECK(tile == G_TX_LOADTILE);

    uint32_t offset_x = uls >> G_TEXTURE_IMAGE_FRAC;
    uint32_t offset_y = ult >> G_TEXTURE_IMAGE_FRAC;
    uint32_t tile_width = ((lrs - uls) >> G_TEXTURE_IMAGE_FRAC) + 1;
    uint32_t tile_height = ((lrt - ult) >> G_TEXTURE_IMAGE_FRAC) + 1;
    uint32_t full_image_width = rdp.texture_to_load.width + 1;

    uint32_t offset_x_in_bytes = offset_x << rdp.texture_to_load.siz >> 1;
    uint32_t tile_line_size_bytes = tile_width << rdp.texture_to_load.siz >> 1;
    uint32_t full_image_line_size_bytes = full_image_width << rdp.texture_to_load.siz >> 1;

    uint32_t orig_size_bytes = tile_line_size_bytes * tile_height;
    uint32_t size_bytes = orig_size_bytes;
    uint32_t start_offset_bytes = full_image_line_size_bytes * offset_y + offset_x_in_bytes;

    float h_byte_scale = rdp.texture_to_load.raw_tex_metadata.h_byte_scale;
    float v_pixel_scale = rdp.texture_to_load.raw_tex_metadata.v_pixel_scale;

    if (h_byte_scale != 1 || v_pixel_scale != 1) {
        start_offset_bytes = h_byte_scale * (v_pixel_scale * offset_y * full_image_line_size_bytes + offset_x_in_bytes);
        size_bytes *= h_byte_scale * v_pixel_scale;
        full_image_line_size_bytes *= h_byte_scale;
        tile_line_size_bytes *= h_byte_scale;
    }

    LoadedTexture& loaded_texture = rdp.loaded_texture[rdp.texture_tile[tile].tmem];
    loaded_texture.orig_size_bytes = orig_size_bytes;
    loaded_texture.size_bytes = size_bytes;
    loaded_texture.full_size_bytes = full_image_line_size_bytes * tile_height;
    loaded_texture.full_image_line_size_bytes = full_image_line_size_bytes;
    loaded_texture.line_size_bytes = tile_line_size_bytes;
    loaded_texture.tex_flags = rdp.texture_to_load.tex_flags;
    loaded_texture.raw_tex_metadata = rdp.texture_to_load.raw_tex_metadata;
    loaded_texture.addr = rdp.texture_to_load.addr + start_offset_bytes;

    rdp.texture_tile[tile].uls = uls;
    rdp.texture_tile[tile].ult = ult;
    rdp.texture_tile[tile].lrs = lrs;
    rdp.texture_tile[tile].lrt = lrt;
    rdp.texture_tile[tile].width = ((lrs - uls) >> G_TEXTURE_IMAGE_FRAC) + 1;
    rdp.texture_tile[tile].height = ((lrt - ult) >> G_TEXTURE_IMAGE_FRAC) + 1;

    rdp.textures_changed[0] = rdp.textures_changed[1] = true;
}

static void gfx_dp_set_combine_mode(uint32_t rgb, uint32_t alpha, uint32_t rgb_cyc2, uint32_t alpha_cyc2) {
    rdp.combine_mode = rgb | (alpha << 16) | ((uint64_t)rgb_cyc2 << 28) | ((uint64_t)alpha_cyc2 << 44);
}

static inline uint32_t color_comb(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return (a & 0xf) | ((b & 0xf) << 4) | ((c & 0x1f) << 8) | ((d & 7) << 13);
}

static inline uint32_t alpha_comb(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return (a & 7) | ((b & 7) << 3) | ((c & 7) << 6) | ((d & 7) << 9);
}

static void gfx_dp_set_grayscale_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.grayscale_color.r = r;
    rdp.grayscale_color.g = g;
    rdp.grayscale_color.b = b;
    rdp.grayscale_color.a = a;
}

static void gfx_dp_set_env_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.env_color.r = r;
    rdp.env_color.g = g;
    rdp.env_color.b = b;
    rdp.env_color.a = a;
}

static void gfx_dp_set_prim_color(uint8_t m, uint8_t l, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.prim_lod_fraction = l;
    rdp.prim_color.r = r;
    rdp.prim_color.g = g;
    rdp.prim_color.b = b;
    rdp.prim_color.a = a;
    rdp.fill_color.r = r;
    rdp.fill_color.g = g;
    rdp.fill_color.b = b;
    rdp.fill_color.a = a;
    rdp.tex_min_lod = m;

}

static void gfx_dp_set_prim_depth(uint16_t z, uint16_t dz) {
    rdp.primitive_depth_z = z;
    rdp.primitive_depth_dz = dz;
}

static void gfx_dp_set_fog_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    rdp.fog_color.r = r;
    rdp.fog_color.g = g;
    rdp.fog_color.b = b;
    rdp.fog_color.a = a;
}

static void gfx_dp_set_fill_color(uint32_t packed_color) {
    uint16_t col16 = (uint16_t)packed_color;
    uint32_t r = col16 >> 11;
    uint32_t g = (col16 >> 6) & 0x1f;
    uint32_t b = (col16 >> 1) & 0x1f;
    uint32_t a = col16 & 1;
    rdp.fill_color.r = SCALE_5_8(r);
    rdp.fill_color.g = SCALE_5_8(g);
    rdp.fill_color.b = SCALE_5_8(b);
    rdp.fill_color.a = a * 255;
}

static void gfx_dp_set_subpixel_offset(int16_t x, int16_t y) {
    rdp.subpixel_ofs_x = x;
    rdp.subpixel_ofs_y = y;
}

static void gfx_draw_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    uint32_t saved_other_mode_h = rdp.other_mode_h;
    uint32_t cycle_type = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE));

    if (cycle_type == G_CYC_COPY) {
        rdp.other_mode_h = (rdp.other_mode_h & ~(3U << G_MDSFT_TEXTFILT)) | G_TF_POINT;
    }

    ulx += rdp.subpixel_ofs_x;
    lrx += rdp.subpixel_ofs_x;
    uly += rdp.subpixel_ofs_y;
    lry += rdp.subpixel_ofs_y;

    // U10.2 coordinates
    float ulxf = ulx;
    float ulyf = uly;
    float lrxf = lrx;
    float lryf = lry;

    ulxf = ulxf / (4.0f * HALF_SCREEN_WIDTH) - 1.0f;
    ulyf = -(ulyf / (4.0f * HALF_SCREEN_HEIGHT)) + 1.0f;
    lrxf = lrxf / (4.0f * HALF_SCREEN_WIDTH) - 1.0f;
    lryf = -(lryf / (4.0f * HALF_SCREEN_HEIGHT)) + 1.0f;

    ulxf = gfx_adjust_x_for_aspect_ratio(ulxf);
    lrxf = gfx_adjust_x_for_aspect_ratio(lrxf);

    struct LoadedVertex* ul = &rsp.loaded_vertices[MAX_VERTICES + 0];
    struct LoadedVertex* ll = &rsp.loaded_vertices[MAX_VERTICES + 1];
    struct LoadedVertex* lr = &rsp.loaded_vertices[MAX_VERTICES + 2];
    struct LoadedVertex* ur = &rsp.loaded_vertices[MAX_VERTICES + 3];

    ul->x = ulxf;
    ul->y = ulyf;
    ul->z = -1.0f;
    ul->w = 1.0f;

    ll->x = ulxf;
    ll->y = lryf;
    ll->z = -1.0f;
    ll->w = 1.0f;

    lr->x = lrxf;
    lr->y = lryf;
    lr->z = -1.0f;
    lr->w = 1.0f;

    ur->x = lrxf;
    ur->y = ulyf;
    ur->z = -1.0f;
    ur->w = 1.0f;

    // The coordinates for texture rectangle shall bypass the viewport setting
    struct XYWidthHeight default_viewport = { 0, (int16_t)SCREEN_HEIGHT, (uint32_t)SCREEN_WIDTH, (uint32_t)SCREEN_HEIGHT };
    struct XYWidthHeight viewport_saved = rdp.viewport;
    uint32_t geometry_mode_saved = rsp.geometry_mode;

    gfx_adjust_viewport_or_scissor(&default_viewport);

    rdp.viewport = default_viewport;
    rdp.viewport_or_scissor_changed = true;
    rsp.geometry_mode = 0;

    gfx_sp_tri1(MAX_VERTICES + 0, MAX_VERTICES + 1, MAX_VERTICES + 3, true);
    gfx_sp_tri1(MAX_VERTICES + 1, MAX_VERTICES + 2, MAX_VERTICES + 3, true);

    rsp.geometry_mode = geometry_mode_saved;
    rdp.viewport = viewport_saved;
    rdp.viewport_or_scissor_changed = true;

    if (cycle_type == G_CYC_COPY) {
        rdp.other_mode_h = saved_other_mode_h;
    }
}

static void gfx_dp_texture_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint8_t tile, int16_t uls,
                                     int16_t ult, int16_t dsdx, int16_t dtdy, bool flip) {
    uint64_t saved_combine_mode = rdp.combine_mode;
    if ((rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE)) == G_CYC_COPY) {
        // Per RDP Command Summary Set Tile's shift s and this dsdx should be set to 4 texels
        // Divide by 4 to get 1 instead
        dsdx >>= 2;

        // Color combiner is turned off in copy mode
        gfx_dp_set_combine_mode(color_comb(0, 0, 0, G_CCMUX_TEXEL0), alpha_comb(0, 0, 0, G_ACMUX_TEXEL0), 0, 0);

        // Per documentation one extra pixel is added in this modes to each edge
        lrx += 1 << 2;
        lry += 1 << 2;
    }

    // uls and ult are S10.5
    // dsdx and dtdy are S5.10
    // lrx, lry, ulx, uly are U10.2
    // lrs, lrt are S10.5

    const int16_t width = flip ? lry - uly : lrx - ulx;
    const int16_t height = flip ? lrx - ulx : lry - uly;
    const float lrs = ((uls << 7) + dsdx * width) >> 7;
    const float lrt = ((ult << 7) + dtdy * height) >> 7;

    struct LoadedVertex* ul = &rsp.loaded_vertices[MAX_VERTICES + 0];
    struct LoadedVertex* ll = &rsp.loaded_vertices[MAX_VERTICES + 1];
    struct LoadedVertex* lr = &rsp.loaded_vertices[MAX_VERTICES + 2];
    struct LoadedVertex* ur = &rsp.loaded_vertices[MAX_VERTICES + 3];
    ul->u = uls;
    ul->v = ult;
    lr->u = lrs;
    lr->v = lrt;
    if (!flip) {
        ll->u = uls;
        ll->v = lrt;
        ur->u = lrs;
        ur->v = ult;
    } else {
        ll->u = lrs;
        ll->v = ult;
        ur->u = uls;
        ur->v = lrt;
    }

    uint8_t saved_tile = rdp.first_tile_index;
    if (saved_tile != tile) {
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
    }
    rdp.first_tile_index = tile;

    gfx_draw_rectangle(ulx, uly, lrx, lry);
    if (saved_tile != tile) {
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
    }
    rdp.first_tile_index = saved_tile;
    rdp.combine_mode = saved_combine_mode;
}

static void gfx_dp_image_rectangle(int32_t tile, int32_t w, int32_t h,
                                   int32_t ulx, int32_t uly, int16_t uls, int16_t ult,
                                   int32_t lrx, int32_t lry, int16_t lrs, int16_t lrt) {
    uint64_t saved_combine_mode = rdp.combine_mode;

    struct LoadedVertex* ul = &rsp.loaded_vertices[MAX_VERTICES + 0];
    struct LoadedVertex* ll = &rsp.loaded_vertices[MAX_VERTICES + 1];
    struct LoadedVertex* lr = &rsp.loaded_vertices[MAX_VERTICES + 2];
    struct LoadedVertex* ur = &rsp.loaded_vertices[MAX_VERTICES + 3];
    ul->u = uls * 32;
    ul->v = ult * 32;
    lr->u = lrs * 32;
    lr->v = lrt * 32;
    ll->u = uls * 32;
    ll->v = lrt * 32;
    ur->u = lrs * 32;
    ur->v = ult * 32;

    // ensure we have the correct texture size
    rdp.texture_tile[tile].line_size_bytes = w << rdp.texture_tile[tile].siz >> 1;
    rdp.texture_tile[tile].width = w;
    rdp.texture_tile[tile].height = h;
    rdp.texture_tile[tile].cms = 0;
    rdp.texture_tile[tile].cmt = 0;
    rdp.texture_tile[tile].shifts = 0;
    rdp.texture_tile[tile].shiftt = 0;
    auto& loadtex = rdp.loaded_texture[rdp.texture_tile[tile].tmem];
    loadtex.full_image_line_size_bytes = loadtex.line_size_bytes = rdp.texture_tile[tile].line_size_bytes;
    loadtex.size_bytes = loadtex.orig_size_bytes = loadtex.full_size_bytes = loadtex.line_size_bytes * h;

    uint8_t saved_tile = rdp.first_tile_index;
    if (saved_tile != tile) {
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
    }
    rdp.first_tile_index = tile;

    gfx_draw_rectangle(ulx, uly, lrx, lry);
    if (saved_tile != tile) {
        rdp.textures_changed[0] = true;
        rdp.textures_changed[1] = true;
    }
    rdp.first_tile_index = saved_tile;

    rdp.combine_mode = saved_combine_mode;
}

static void gfx_dp_fill_rectangle(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
    if (rdp.color_image_address == rdp.z_buf_address) {
        // Don't clear Z buffer here since we already did it with glClear
        return;
    }
    uint32_t mode = (rdp.other_mode_h & (3U << G_MDSFT_CYCLETYPE));

    // OTRTODO: This is a bit of a hack for widescreen screen fades, but it'll work for now...
    if (ulx == 0 && uly == 0 && lrx == 319 * 4 && lry == 239 * 4) {
        ulx = -1024;
        uly = -1024;
        lrx = 2048;
        lry = 2048;
    }

    if (mode == G_CYC_COPY || mode == G_CYC_FILL) {
        // Per documentation one extra pixel is added in this modes to each edge
        lrx += 1 << 2;
        lry += 1 << 2;
    }

    for (int i = MAX_VERTICES; i < MAX_VERTICES + 4; i++) {
        struct LoadedVertex* v = &rsp.loaded_vertices[i];
        v->color = rdp.fill_color;
    }

    uint64_t saved_combine_mode = rdp.combine_mode;

    if (mode == G_CYC_FILL) {
        gfx_dp_set_combine_mode(color_comb(0, 0, 0, G_CCMUX_SHADE), alpha_comb(0, 0, 0, G_ACMUX_SHADE), 0, 0);
    }

    gfx_draw_rectangle(ulx, uly, lrx, lry);
    rdp.combine_mode = saved_combine_mode;
}

static void gfx_dp_set_z_image(void* z_buf_address) {
    rdp.z_buf_address = z_buf_address;
}

static void gfx_dp_set_color_image(uint32_t format, uint32_t size, uint32_t width, void* address) {
    rdp.color_image_address = address;
}

/*
 * Commands after which the cached triangle setup is still valid: geometry
 * and matrix loads, syncs (no state) and other-mode writes, which invalidate
 * themselves only when they change a bit (display lists re-send the same
 * render mode constantly).
 */
static inline bool gfx_opcode_keeps_tri_setup(uint32_t opcode) {
    switch (opcode) {
        case (uint8_t)G_VTX:
        case (uint8_t)G_TRI1:
        case (uint8_t)G_TRI4:
        case (uint8_t)G_COL:
        case (uint8_t)G_MTX:
        case (uint8_t)G_POPMTX:
        case (uint8_t)G_NOOP:
        case (uint8_t)G_MOVEWORD: /* lights, fog factor, segments: per-vertex state only */
        case (uint8_t)G_SETOTHERMODE_L:
        case (uint8_t)G_SETOTHERMODE_H:
        case (uint8_t)G_RDPPIPESYNC:
        case (uint8_t)G_RDPFULLSYNC:
        case (uint8_t)G_RDPLOADSYNC:
        case (uint8_t)G_RDPTILESYNC:
            return true;
        default:
            return false;
    }
}

static void gfx_sp_set_other_mode(uint32_t shift, uint32_t num_bits, uint64_t mode) {
    uint64_t mask = (((uint64_t)1 << num_bits) - 1) << shift;
    const uint64_t old_om = rdp.other_mode_l | ((uint64_t)rdp.other_mode_h << 32);
    uint64_t om = (old_om & ~mask) | mode;
    if (om != old_om) {
        gfx_tri_setup_invalidate();
    }
    rdp.other_mode_l = (uint32_t)om;
    rdp.other_mode_h = (uint32_t)(om >> 32);
    rdp.palette_fmt = rdp.other_mode_h & (3U << G_MDSFT_TEXTLUT);
    rdp.tex_lod = (rdp.other_mode_h & G_TL_LOD) != 0;
    rdp.tex_detail = (rdp.other_mode_h & (2U << G_MDSFT_TEXTDETAIL)) == G_TD_DETAIL;
}

static void gfx_sp_set_vertex_colors(uint32_t count, const struct NormalColor *vcn) {
    // common sense dictates that we should copy the colors as the command is supposed to do,
    // but it actually doesn't seem to matter
    // SUPPORT_CHECK(count <= sizeof(rsp.vertex_colors) / sizeof(rsp.vertex_colors[0]));
    // for (uint32_t i = 0; i < count; ++i) {
    //     rsp.vertex_colors[i] = vcn[i];
    // }
    rsp.vertex_colors = vcn;
}

static void gfx_dp_set_other_mode(uint32_t h, uint32_t l) {
    rdp.other_mode_h = h;
    rdp.other_mode_l = l;
}

static inline void *seg_addr(uintptr_t w1) {
    // all segmented addresses have the least significant bit set
    if (w1 & 1) {
        // seg 0 is reserved and doesn't count here
        const uintptr_t seg = (w1 & 0x0f000000) >> 24;
        if (seg && segmentPointers[seg]) {
            const uintptr_t addr = (w1 & 0x00fffffe);
            return (void *)(segmentPointers[seg] + addr);
        }
    }
    return (void *)w1;
}

uintptr_t clearMtx;

static void gfx_run_dl(Gfx* cmd) {
    // puts("dl");
    int dummy = 0;
    char dlName[128];
    const char* fileName;

    Gfx* dListStart = cmd;
    uint64_t ourHash = -1;

    for (;;) {
        uint32_t opcode = cmd->words.w0 >> 24;
        // gfx_print_cmd(cmd);
        if (!gfx_opcode_keeps_tri_setup(opcode)) {
            gfx_tri_setup_invalidate();
        }
        switch (opcode) {
                // RSP commands:
            case G_NOOP:
                break;
            case G_MTX: {
                gfx_sp_matrix(C0(16, 8), (const int32_t*)seg_addr(cmd->words.w1));
                break;
            }
            case (uint8_t)G_POPMTX:
                gfx_sp_pop_matrix(1);
                break;
            case G_MOVEMEM:
                gfx_sp_movemem(C0(16, 8), 0, seg_addr(cmd->words.w1));
                break;
            case (uint8_t)G_MOVEWORD:
                gfx_sp_moveword(C0(0, 8), C0(8, 16), cmd->words.w1);
                break;
            case (uint8_t)G_TEXTURE:
                gfx_sp_texture(C1(16, 16), C1(0, 16), C0(11, 3), C0(8, 3), C0(0, 8));
                break;
            case G_VTX:
                gfx_sp_vertex(C0(0, 16) / sizeof(Vtx), C0(16, 4), (const Vtx*)seg_addr(cmd->words.w1));
                break;
            case G_DL:
                if (C0(16, 1) == 0) {
                    // Push return address
                    Gfx* subGFX = (Gfx*)seg_addr(cmd->words.w1);

                    if (subGFX != nullptr) {
                        gfx_run_dl(subGFX);
                    }
                } else {
                    cmd = (Gfx*)seg_addr(cmd->words.w1);
                    --cmd; // increase after break
                }
                break;
            case (uint8_t)G_ENDDL:
                return;
            case (uint8_t)G_SETGEOMETRYMODE:
                gfx_sp_geometry_mode(0, cmd->words.w1);
                break;
            case (uint8_t)G_CLEARGEOMETRYMODE:
                gfx_sp_geometry_mode(cmd->words.w1, 0);
                break;
            case G_EXTRAGEOMETRYMODE_EXT:
                gfx_sp_extra_geometry_mode(~C0(0, 24), cmd->words.w1);
                break;
            case (uint8_t)G_TRI1:
                gfx_sp_tri1(C1(16, 8) / 10, C1(8, 8) / 10, C1(0, 8) / 10, false);
                break;
            case (uint8_t)G_TRI4:
                gfx_sp_tri4(cmd);
                break;
            case (uint8_t)G_SETOTHERMODE_L:
                gfx_sp_set_other_mode(C0(8, 8), C0(0, 8), cmd->words.w1);
                break;
            case (uint8_t)G_SETOTHERMODE_H:
                gfx_sp_set_other_mode(C0(8, 8) + 32, C0(0, 8), (uint64_t)cmd->words.w1 << 32);
                break;
            case G_COL:
                gfx_sp_set_vertex_colors(C0(0, 16) / 4, (NormalColor *)seg_addr(cmd->words.w1));
                break;

            // RDP Commands:
            case G_SETTIMG: {
                gfx_dp_set_texture_image(C0(21, 3), C0(19, 2), C0(0, 10), 0, seg_addr(cmd->words.w1));
                break;
            }
            case G_SETTIMG_FB_EXT:
                gfx_flush();
                gfx_rapi->select_texture_fb(cmd->words.w1);
                rdp.textures_changed[0] = false;
                rdp.textures_changed[1] = false;
                break;
            case G_SETGRAYSCALE_EXT:
                rdp.grayscale = cmd->words.w1;
                break;
            case G_LOADBLOCK:
                gfx_dp_load_block(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_LOADTILE:
                gfx_dp_load_tile(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_SETTILE:
                gfx_dp_set_tile(C0(21, 3), C0(19, 2), C0(9, 9), C0(0, 9), C1(24, 3), C1(20, 4), C1(18, 2), C1(14, 4),
                                C1(10, 4), C1(8, 2), C1(4, 4), C1(0, 4));
                break;
            case G_SETTILESIZE:
                gfx_dp_set_tile_size(C1(24, 3), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_LOADTLUT:
                gfx_dp_load_tlut(C1(24, 3), C0(14, 10), C0(2, 10), C1(14, 10), C1(2, 10));
                break;
            case G_SETENVCOLOR:
                gfx_dp_set_env_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETPRIMCOLOR:
                gfx_dp_set_prim_color(C0(8, 8), C0(0, 8), C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETPRIMDEPTH:
                gfx_dp_set_prim_depth(C1(16, 16), C1(0, 16));
                break;
            case G_SETFOGCOLOR:
                gfx_dp_set_fog_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETFILLCOLOR:
                gfx_dp_set_fill_color(cmd->words.w1);
                break;
            case G_SETINTENSITY_EXT:
                gfx_dp_set_grayscale_color(C1(24, 8), C1(16, 8), C1(8, 8), C1(0, 8));
                break;
            case G_SETCOMBINE:
                gfx_dp_set_combine_mode(color_comb(C0(20, 4), C1(28, 4), C0(15, 5), C1(15, 3)),
                                        alpha_comb(C0(12, 3), C1(12, 3), C0(9, 3), C1(9, 3)),
                                        color_comb(C0(5, 4), C1(24, 4), C0(0, 5), C1(6, 3)),
                                        alpha_comb(C1(21, 3), C1(3, 3), C1(18, 3), C1(0, 3)));
                break;
            // G_SETPRIMCOLOR, G_CCMUX_PRIMITIVE, G_ACMUX_PRIMITIVE, is used by Goddard
            // G_CCMUX_TEXEL1, LOD_FRACTION is used in Bowser room 1
            case G_SETSUBPIXELOFFSET_EXT: {
                gfx_dp_set_subpixel_offset(C0(0, 16), C1(0, 16));
                break;
            }
            case G_TEXRECT:
            case G_TEXRECTFLIP: {
                int32_t lrx, lry, tile, ulx, uly;
                uint32_t uls, ult, dsdx, dtdy;
                lrx = C0(12, 12);
                lry = C0(0, 12);
                tile = C1(24, 3);
                ulx = C1(12, 12);
                uly = C1(0, 12);
                ++cmd;
                uls = C1(16, 16);
                ult = C1(0, 16);
                ++cmd;
                dsdx = C1(16, 16);
                dtdy = C1(0, 16);
                gfx_dp_texture_rectangle(ulx, uly, lrx, lry, tile, uls, ult, dsdx, dtdy, opcode == G_TEXRECTFLIP);
                break;
            }
            case G_FILLRECT:
                gfx_dp_fill_rectangle(C1(12, 12), C1(0, 12), C0(12, 12), C0(0, 12));
                break;
            case G_FILLRECT_WIDE_EXT: {
                int32_t lrx, lry, ulx, uly;
                lrx = (int32_t)(C0(0, 24) << 8) >> 8;
                lry = (int32_t)(C1(0, 24) << 8) >> 8;
                ++cmd;
                ulx = (int32_t)(C0(0, 24) << 8) >> 8;
                uly = (int32_t)(C1(0, 24) << 8) >> 8;
                gfx_dp_fill_rectangle(ulx, uly, lrx, lry);
                break;
            }
            case G_TEXRECT_WIDE_EXT: {
                int32_t lrx, lry, tile, ulx, uly;
                uint32_t uls, ult, dsdx, dtdy;
                bool flip;
                lrx = (int32_t)((C0(0, 24) << 8)) >> 8;
                lry = (int32_t)((C1(0, 24) << 8)) >> 8;
                tile = C1(24, 3);
                flip = C1(27, 1);
                ++cmd;
                ulx = (int32_t)((C0(0, 24) << 8)) >> 8;
                uly = (int32_t)((C1(0, 24) << 8)) >> 8;
                ++cmd;
                uls = C0(16, 16);
                ult = C0(0, 16);
                dsdx = C1(16, 16);
                dtdy = C1(0, 16);
                gfx_dp_texture_rectangle(ulx, uly, lrx, lry, tile, uls, ult, dsdx, dtdy, flip);
                break;
            }
            case G_IMAGERECT_EXT: {
                int16_t tile, iw, ih;
                int16_t x0, y0, s0, t0;
                int16_t x1, y1, s1, t1;
                tile = C0(0, 3);
                iw = C1(16, 16);
                ih = C1(0, 16);
                ++cmd;
                x0 = C0(16, 16);
                y0 = C0(0, 16);
                s0 = C1(16, 16);
                t0 = C1(0, 16);
                ++cmd;
                x1 = C0(16, 16);
                y1 = C0(0, 16);
                s1 = C1(16, 16);
                t1 = C1(0, 16);
                gfx_dp_image_rectangle(tile, iw, ih, x0, y0, s0, t0, x1, y1, s1, t1);
                break;
            }
            case G_SETSCISSOR:
                gfx_dp_set_scissor(C1(24, 2), C0(12, 12), C0(0, 12), C1(12, 12), C1(0, 12));
                break;
            case G_SETZIMG:
                gfx_dp_set_z_image(seg_addr(cmd->words.w1));
                break;
            case G_SETCIMG:
                gfx_dp_set_color_image(C0(21, 3), C0(19, 2), C0(0, 11), seg_addr(cmd->words.w1));
                break;
            case G_SETFB_EXT:
                gfx_flush();
                if (cmd->words.w1) {
                    // don't care about noise here
                    gfx_set_framebuffer(cmd->words.w1, 1.f);
                    fbActive = true;
                } else {
                    gfx_reset_framebuffer();
                    fbActive = false;
                }
                break;
            case G_COPYFB_EXT:
                gfx_copy_framebuffer(C0(11, 11), C0(0, 11), (int16_t)C1(16, 16), (int16_t)C1(0, 16), C0(22, 1));
                break;
            case G_RDPSETOTHERMODE:
                gfx_dp_set_other_mode(C0(0, 24), cmd->words.w1);
                break;
            case G_INVALTEXCACHE_EXT:
                if (cmd->words.w1) {
                    gfx_texture_cache_delete((const uint8_t *)seg_addr(cmd->words.w1));
                } else {
                    gfx_texture_cache_clear();
                }
                break;
            case (uint8_t)G_RDPHALF_1:
            case (uint8_t)G_RDPHALF_2:
            case (uint8_t)G_RDPHALF_CONT:
                // on N64 skyRender uses these to render some types of skies and skybox water
                // by issuing low-level ucode commands G_TRI_FILL and G_TRI_SHADE_TXTR
                // the port renders the sky in a different manner
                break;
            case G_RDPFLUSH_EXT:
                gfx_flush();
                break;
            case G_CLEAR_DEPTH_EXT:
                gfx_flush();
                gfx_rapi->clear_framebuffer(false, true);
                break;
            case G_RDPPIPESYNC:
            case G_RDPFULLSYNC:
            case G_RDPLOADSYNC:
            case G_RDPTILESYNC:
                break;
            default:
                sysFatalError("Unknown GBI opcode 0x%02x at %p.\nw0 %08x\nw1 %08x", opcode, cmd, cmd->words.w0, cmd->words.w1);
                break;
        }
        ++cmd;
    }
}

static void gfx_sp_reset() {
    rsp.modelview_matrix_stack_size = 1;
    rsp.current_num_lights = 2;
    rsp.lights_changed = true;
}

extern "C" void gfx_get_dimensions(uint32_t* width, uint32_t* height, int32_t* posX, int32_t* posY) {
    gfx_wapi->get_dimensions(width, height, posX, posY);
}

extern "C" void gfx_init(const GfxInitSettings *settings) {
    gfx_wapi = settings->wapi;
    gfx_init_u8_unit_table();
#if defined(GFX_PS2_DIRECT)
    gfxPs2DirectSetUnitTable(gfx_u8_unit);
#endif
    gfx_rapi = settings->rapi;
    gfx_wapi->init(&settings->window_settings);
    gfx_rapi->init();
    gfx_rapi->update_framebuffer_parameters(0, settings->window_settings.width, settings->window_settings.height, 1, false, true, true, true);
    gfx_current_dimensions.internal_mul = 1;
    gfx_current_game_window_viewport.width = gfx_current_dimensions.width = settings->window_settings.width;
    gfx_current_game_window_viewport.height = gfx_current_dimensions.height = settings->window_settings.height;
    game_framebuffer = gfx_rapi->create_framebuffer();
    game_framebuffer_msaa_resolved = gfx_rapi->create_framebuffer();

    if (gfx_msaa_level > 1 && !gfx_framebuffers_enabled) {
        sysLogPrintf(LOG_WARNING, "F3D: MSAA set to %d, but framebuffers are not available; disabling", gfx_msaa_level);
        gfx_msaa_level = 1;
    }

    for (int i = 0; i < 16; i++) {
        segmentPointers[i] = 0;
    }

    if (tex_upload_buffer == nullptr) {
#if defined(PLATFORM_PS2)
        tex_upload_buffer = (uint8_t*)malloc(PS2_TEXTURE_UPLOAD_SCRATCH_SIZE);
        tex_upload_buffer_size = PS2_TEXTURE_UPLOAD_SCRATCH_SIZE;
#else
        // We cap texture max to 8k, because why would you need more?
        int max_tex_size = std::min(8192, gfx_rapi->get_max_texture_size());
        tex_upload_buffer = (uint8_t*)malloc(max_tex_size * max_tex_size * 4);
#endif
        if (tex_upload_buffer == nullptr) {
            abort();
        }
    }

    rsp.lookat[0].dir[0] = rsp.lookat[1].dir[1] = 0x7F;
    rsp.current_lookat_coeffs[0][0] = rsp.current_lookat_coeffs[1][1] = 1.f;
    rsp.lookat_enabled = true;
}

extern "C" void gfx_destroy(void) {
    // TODO: should also destroy rapi and wapi, and any other resources acquired in fast3d

    // Texture cache and loaded textures store references to Resources which need to be unreferenced.
    gfx_texture_cache_clear();
}

extern "C" struct GfxRenderingAPI* gfx_get_current_rendering_api(void) {
    return gfx_rapi;
}

extern "C" void gfx_start_frame(void) {
    gfx_wapi->handle_events();
    gfx_wapi->get_dimensions(&gfx_current_window_dimensions.width, &gfx_current_window_dimensions.height,
                             &gfx_current_window_position_x, &gfx_current_window_position_y);

    if (gfx_current_window_dimensions.height == 0) {
        // Avoid division by zero
        gfx_current_window_dimensions.height = 1;
    }

    gfx_current_window_dimensions.aspect_ratio = (float)gfx_current_window_dimensions.width / gfx_current_window_dimensions.height;

    gfx_current_dimensions = gfx_current_window_dimensions;

    gfx_current_game_window_viewport.width = gfx_current_dimensions.width;
    gfx_current_game_window_viewport.height = gfx_current_dimensions.height;

    if (gfx_current_dimensions.height != gfx_prev_dimensions.height) {
#if defined(PLATFORM_PS2)
        for (size_t i = 0; i < PS2_FRAMEBUFFER_INFO_COUNT; ++i) {
            Ps2FramebufferInfo* entry = &framebuffers[i];
            if (!entry->occupied) {
                continue;
            }
            FBInfo* fb = &entry->info;
            uint32_t width, height;
            if (fb->autoresize) {
                if (fb->upscale) {
                    width = fb->orig_width;
                    height = fb->orig_height;
                    gfx_adjust_width_height_for_scale(width, height);
                } else {
                    // assume this is a fullscreen fb
                    width = gfx_current_dimensions.width;
                    height = gfx_current_dimensions.height;
                }
                if (width != fb->applied_width || height != fb->applied_height) {
                    gfx_rapi->update_framebuffer_parameters(entry->id, width, height, 1, true, true, true, true);
                    fb->applied_width = width;
                    fb->applied_height = height;
                }
            }
        }
#else
        for (auto& fb : framebuffers) {
            uint32_t width, height;
            if (fb.second.autoresize) {
                if (fb.second.upscale) {
                    width = fb.second.orig_width;
                    height = fb.second.orig_height;
                    gfx_adjust_width_height_for_scale(width, height);
                } else {
                    // assume this is a fullscreen fb
                    width = gfx_current_dimensions.width;
                    height = gfx_current_dimensions.height;
                }
                if (width != fb.second.applied_width || height != fb.second.applied_height) {
                    gfx_rapi->update_framebuffer_parameters(fb.first, width, height, 1, true, true, true, true);
                    fb.second.applied_width = width;
                    fb.second.applied_height = height;
                }
            }
        }
#endif
    }
    gfx_prev_dimensions = gfx_current_dimensions;

    bool different_size = gfx_current_dimensions.width != gfx_current_game_window_viewport.width ||
                          gfx_current_dimensions.height != gfx_current_game_window_viewport.height;
    if (gfx_framebuffers_enabled && (different_size || gfx_msaa_level > 1)) {
        game_renders_to_framebuffer = true;
        if (different_size) {
            gfx_rapi->update_framebuffer_parameters(game_framebuffer, gfx_current_dimensions.width,
                                                    gfx_current_dimensions.height, gfx_msaa_level, true, true, true,
                                                    true);
        } else {
            // MSAA framebuffer needs to be resolved to an equally sized target when complete, which must therefore
            // match the window size
            gfx_rapi->update_framebuffer_parameters(game_framebuffer, gfx_current_window_dimensions.width,
                                                    gfx_current_window_dimensions.height, gfx_msaa_level, false, true,
                                                    true, true);
        }
        if (gfx_msaa_level > 1 && different_size) {
            gfx_rapi->update_framebuffer_parameters(game_framebuffer_msaa_resolved, gfx_current_dimensions.width,
                                                    gfx_current_dimensions.height, 1, false, false, false, false);
        }
    } else {
        game_renders_to_framebuffer = false;
    }

    fbActive = 0;

    // update aspect scale and offset
    gfx_update_aspect_mode();
}

uint32_t num_dls = 0;

extern "C" void gfx_run(Gfx* commands) {
    ++num_dls;
    gfx_sp_reset();

    // puts("New frame");

    if (!gfx_wapi->start_frame()) {
        dropped_frame = true;
        return;
    }
    dropped_frame = false;

    gfx_rapi->update_framebuffer_parameters(0, gfx_current_window_dimensions.width,
                                            gfx_current_window_dimensions.height, 1, false, true, true,
                                            !game_renders_to_framebuffer);
    gfx_rapi->start_frame();
    gfx_rapi->start_draw_to_framebuffer(game_renders_to_framebuffer ? game_framebuffer : 0,
                                        (float)gfx_current_dimensions.height / SCREEN_HEIGHT);
    gfx_rapi->clear_framebuffer(true, false);
    rdp.viewport_or_scissor_changed = true;
    rendering_state.viewport = {};
    rendering_state.scissor = {};
    gfx_tri_setup_invalidate();
#if defined(GFX_PS2_DIRECT) && !defined(PD_PS2_DIRECT_VERIFY) && !defined(PD_PS2_NO_OBJECT_GEOMETRY)
    gfx_obj.enabled = gfxPs2ObjectGeometryEnabled();
#endif
    gfx_run_dl(commands);
#if defined(GFX_PS2_DIRECT)
    /* slots a later frame might read without reloading */
    gfx_obj_materialize_all();
#if defined(PD_PS2_OBJECT_STATS)
    {
        static uint32_t frames;
        if (++frames % 60 == 0) {
            sysLogPrintf(LOG_NOTE, "OBJGEOM blocks inside=%u outside=%u mixed=%u tris=%u replays=%u (per frame)",
                         gfx_obj.n_inside / 60, gfx_obj.n_outside / 60, gfx_obj.n_mixed / 60, gfx_obj.n_tris / 60,
                         gfx_obj.n_replays / 60);
            gfx_obj.n_inside = gfx_obj.n_outside = gfx_obj.n_mixed = gfx_obj.n_tris = gfx_obj.n_replays = 0;
        }
    }
#endif
#endif
    gfx_flush();
    gfxFramebuffer = 0;

    if (game_renders_to_framebuffer) {
        gfx_rapi->start_draw_to_framebuffer(0, 1);
        gfx_rapi->clear_framebuffer(true, true);

        if (gfx_msaa_level > 1) {
            bool different_size = gfx_current_dimensions.width != gfx_current_game_window_viewport.width ||
                                  gfx_current_dimensions.height != gfx_current_game_window_viewport.height;

            if (different_size) {
                gfx_rapi->resolve_msaa_color_buffer(game_framebuffer_msaa_resolved, game_framebuffer);
                gfxFramebuffer = (uintptr_t)gfx_rapi->get_framebuffer_texture_id(game_framebuffer_msaa_resolved);
            } else {
                gfx_rapi->resolve_msaa_color_buffer(0, game_framebuffer);
            }
        } else {
            gfxFramebuffer = (uintptr_t)gfx_rapi->get_framebuffer_texture_id(game_framebuffer);
        }
    }

    gfx_rapi->end_frame();
    gfx_wapi->swap_buffers_begin();
}

extern "C" void gfx_end_frame(void) {
    if (!dropped_frame) {
        gfx_rapi->finish_render();
        gfx_wapi->swap_buffers_end();
    }
}

extern "C" void gfx_set_target_fps(int fps) {
    gfx_wapi->set_target_fps(fps);
}

extern "C" void reset_texture_state() {
    gfx_texture_cache_clear();
    if (rendering_state.shader_program) {
        gfx_rapi->unload_shader(rendering_state.shader_program);
        rendering_state.shader_program = nullptr;
    }
    gfx_rapi->clear_shaders();
#if defined(PLATFORM_PS2)
    memset(color_combiner_pool, 0, sizeof(color_combiner_pool));
    prev_combiner = nullptr;
    color_combiner_lru_clock = 0;
#else
    color_combiner_pool.clear();
    prev_combiner = color_combiner_pool.end();
#endif
}

extern "C" void gfx_set_texture_filter(enum FilteringMode mode) {
    reset_texture_state();
    gfx_rapi->set_texture_filter(mode);
}

extern "C" void gfx_set_mipmap_filter(enum MipmapFilteringMode mode) {
    reset_texture_state();
    gfx_rapi->set_mipmap_filter(mode);
}

extern "C" int gfx_create_framebuffer(uint32_t width, uint32_t height, int upscale, int autoresize) {
    int fb = gfx_rapi->create_framebuffer();
    gfx_resize_framebuffer(fb, width, height, upscale, autoresize);
    return fb;
}

extern "C" void gfx_resize_framebuffer(int fb, uint32_t width, uint32_t height, int upscale, int autoresize) {
    uint32_t orig_width, orig_height;

    if (width && height) {
        // user-specified size
        orig_width = width;
        orig_height = height;
        if (upscale) {
            gfx_adjust_width_height_for_scale(width, height);
        }
        gfx_rapi->update_framebuffer_parameters(fb, width, height, 1, true, true, true, true);
    } else {
        // same size as main fb
        orig_width = width = gfx_current_dimensions.width;
        orig_height = height = gfx_current_dimensions.height;
        upscale = false;
        autoresize = true;
        gfx_rapi->update_framebuffer_parameters(fb, width, height, 1, true, true, true, true);
    }

#if defined(PLATFORM_PS2)
    Ps2FramebufferInfo* free_entry = nullptr;
    for (size_t i = 0; i < PS2_FRAMEBUFFER_INFO_COUNT; ++i) {
        if (framebuffers[i].occupied && framebuffers[i].id == fb) {
            free_entry = &framebuffers[i];
            break;
        }
        if (!framebuffers[i].occupied && free_entry == nullptr) {
            free_entry = &framebuffers[i];
        }
    }
    if (free_entry == nullptr) {
        abort();
    }
    free_entry->id = fb;
    free_entry->info = { orig_width, orig_height, width, height, (bool)upscale, (bool)autoresize };
    free_entry->occupied = true;
#else
    framebuffers[fb] = { orig_width, orig_height, width, height, (bool)upscale, (bool)autoresize };
#endif
}

extern "C" void gfx_set_framebuffer(int fb, float noise_scale) {
    gfx_rapi->start_draw_to_framebuffer(fb, noise_scale);
    gfx_rapi->clear_framebuffer(true, true);
#if !defined(PLATFORM_PS2)
    active_fb = framebuffers.find(fb);
#endif
}

extern "C" void gfx_copy_framebuffer(int fb_dst, int fb_src, int left, int top, int use_back) {
    const bool is_main_fb = (fb_src == 0);

    if (is_main_fb) {
        if (left > 0 && top > 0) {
            // upscale the position
            left = left * gfx_current_dimensions.width / gfx_current_native_viewport.width;
            top = top * gfx_current_dimensions.height / gfx_current_native_viewport.height;
            // flip Y
            top = gfx_current_dimensions.height - top - 1;
        }
        if (use_back && gfx_msaa_level > 1) {
            // read from the framebuffer we've been rendering to
            fb_src = game_framebuffer;
        }
    }

    gfx_rapi->copy_framebuffer(fb_dst, fb_src, left, top, is_main_fb, (bool)use_back);
}

extern "C" void gfx_reset_framebuffer(void) {
    gfx_rapi->start_draw_to_framebuffer(0, (float)gfx_current_dimensions.height / SCREEN_HEIGHT);
#if !defined(PLATFORM_PS2)
    active_fb = framebuffers.end();
#endif
}
