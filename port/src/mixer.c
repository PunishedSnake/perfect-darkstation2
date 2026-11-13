#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <ultra64.h>

#include "mixer.h"
#include "platform.h"

#define MINIMP3_IMPLEMENTATION
#include "external/minimp3.h"

#ifdef __SSE4_1__
#include <immintrin.h>
#define HAS_SSE41 1
#define HAS_NEON 0
#elif __ARM_NEON
#include <arm_neon.h>
#define HAS_SSE41 0
#define HAS_NEON 1
#else
#define HAS_SSE41 0
#define HAS_NEON 0
#endif

#pragma GCC optimize ("unroll-loops")

#if HAS_SSE41
#define LOADLH(l, h) _mm_castpd_si128(_mm_loadh_pd(_mm_load_sd((const double *)(l)), (const double *)(h)))
#endif

#if defined(_MIPS_ARCH_R5900)
/*
 * R5900 MMI kernels. Every one is bit-exact with the scalar reference it
 * replaces (see the comments); they are only used on 16-byte aligned DMEM
 * spans. GCC treats GPRs as 64-bit, so each block keeps its 128-bit values in
 * explicitly clobbered temporaries ($8-$15) inside a single asm statement.
 */
#define HAS_MMI 1

typedef struct { int16_t h[8]; } __attribute__((aligned(16))) mmi_h8;
typedef struct { int32_t w[4]; } __attribute__((aligned(16))) mmi_w4;

static const mmi_w4 k_mmi_round = {{0x4000, 0x4000, 0x4000, 0x4000}};
static const mmi_w4 k_mmi_max16 = {{0x7fff, 0x7fff, 0x7fff, 0x7fff}};
static const mmi_w4 k_mmi_min16 = {{-0x8000, -0x8000, -0x8000, -0x8000}};

static inline int mmiAligned(const void *a, const void *b)
{
    return (((uintptr_t)a | (uintptr_t)b) & 15u) == 0u;
}

/* out[i] = clamp16((out[i] * 0x7fff + in[i] * gain + 0x4000) >> 15) */
static void mmiMixGeneral(int16_t *out, const int16_t *in, int16_t gain, int blocks)
{
    mmi_h8 coef;
    for (int k = 0; k < 8; k += 2) {
        coef.h[k] = 0x7fff;
        coef.h[k + 1] = gain;
    }
    for (int b = 0; b < blocks; ++b, out += 8, in += 8) {
        __asm__ __volatile__(
            "lq      $8, 0(%0)\n\t"        /* out */
            "lq      $9, 0(%1)\n\t"        /* in */
            "lq      $10, 0(%2)\n\t"       /* {0x7fff, gain} x4 */
            "lq      $11, 0(%3)\n\t"       /* 0x4000 words */
            "pextlh  $12, $9, $8\n\t"      /* {out0,in0,out1,in1,...} */
            "pextuh  $13, $9, $8\n\t"      /* samples 4..7 */
            "phmadh  $12, $12, $10\n\t"
            "phmadh  $13, $13, $10\n\t"
            "lq      $14, 0(%4)\n\t"       /* 32767 words */
            "lq      $15, 0(%5)\n\t"       /* -32768 words */
            "paddw   $12, $12, $11\n\t"
            "paddw   $13, $13, $11\n\t"
            "psraw   $12, $12, 15\n\t"
            "psraw   $13, $13, 15\n\t"
            "pminw   $12, $12, $14\n\t"
            "pminw   $13, $13, $14\n\t"
            "pmaxw   $12, $12, $15\n\t"
            "pmaxw   $13, $13, $15\n\t"
            "ppach   $12, $13, $12\n\t"
            "sq      $12, 0(%0)\n\t"
            :
            : "r"(out), "r"(in), "r"(&coef), "r"(&k_mmi_round),
              "r"(&k_mmi_max16), "r"(&k_mmi_min16)
            : "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15",
              "hi", "lo", "memory");
    }
}

/* out[i] = clamp16(out[i] - in[i]) */
static void mmiMixSubtract(int16_t *out, const int16_t *in, int blocks)
{
    for (int b = 0; b < blocks; ++b, out += 8, in += 8) {
        __asm__ __volatile__(
            "lq      $8, 0(%0)\n\t"
            "lq      $9, 0(%1)\n\t"
            "psubsh  $8, $8, $9\n\t"
            "sq      $8, 0(%0)\n\t"
            :
            : "r"(out), "r"(in)
            : "$8", "$9", "memory");
    }
}

/*
 * out[i] = clamp16(out[i] + ((in[i] * g) >> 15)), g != -0x8000.
 * With g > -0x8000 the shifted product always fits in int16, so a saturating
 * halfword add reproduces the 32-bit add + clamp exactly.
 */
static void mmiMixScaledAdd(int16_t *out, const int16_t *in, int16_t g, int blocks)
{
    mmi_h8 coef;
    for (int k = 0; k < 8; k += 2) {
        coef.h[k] = g;
        coef.h[k + 1] = 0;
    }
    for (int b = 0; b < blocks; ++b, out += 8, in += 8) {
        __asm__ __volatile__(
            "lq      $8, 0(%0)\n\t"        /* out */
            "lq      $9, 0(%1)\n\t"        /* in */
            "lq      $10, 0(%2)\n\t"       /* {g, 0} x4 */
            "pextlh  $12, $0, $9\n\t"      /* {in0,0,in1,0,...} */
            "pextuh  $13, $0, $9\n\t"
            "phmadh  $12, $12, $10\n\t"    /* in*g words */
            "phmadh  $13, $13, $10\n\t"
            "psraw   $12, $12, 15\n\t"
            "psraw   $13, $13, 15\n\t"
            "ppach   $12, $13, $12\n\t"
            "paddsh  $8, $8, $12\n\t"
            "sq      $8, 0(%0)\n\t"
            :
            : "r"(out), "r"(in), "r"(&coef)
            : "$8", "$9", "$10", "$12", "$13", "hi", "lo", "memory");
    }
}
#else
#define HAS_MMI 0
#endif

#define ROUND_UP_64(v) (((v) + 63) & ~63)
#define ROUND_UP_32(v) (((v) + 31) & ~31)
#define ROUND_UP_16(v) (((v) + 15) & ~15)
#define ROUND_UP_8(v) (((v) + 7) & ~7)
#define ROUND_DOWN_16(v) ((v) & ~0xf)

#define BUF_SIZE 3072 // 2720 + slack
#define BUF_U8(a) (rspa.buf.as_u8 + (a))
#define BUF_S16(a) (rspa.buf.as_s16 + (a) / sizeof(int16_t))

#define NUM_SAMPLES 0xB8
#define NUM_BYTES 0x170

#define OFS_BASE 0
#define OFS_TEMP1 0x170
#define OFS_MAIN_L 0x4E0
#define OFS_MAIN_R 0x650
#define OFS_AUX_L 0x7C0
#define OFS_AUX_R 0x930

static struct {
    int16_t vol[2];

    int16_t target[2];
    int32_t rate[2];

    int16_t vol_dry;
    int16_t vol_wet;

    ADPCM_STATE *adpcm_loop_state;

    int16_t adpcm_table[8][2][8];

    union {
        int16_t as_s16[BUF_SIZE / sizeof(int16_t)];
        uint8_t as_u8[BUF_SIZE];
    } buf __attribute__((aligned(16)));
} rspa;

static int16_t resample_table[64][4] __attribute__((aligned(16))) = {
    {0x0c39, 0x66ad, 0x0d46, 0xffdf}, {0x0b39, 0x6696, 0x0e5f, 0xffd8},
    {0x0a44, 0x6669, 0x0f83, 0xffd0}, {0x095a, 0x6626, 0x10b4, 0xffc8},
    {0x087d, 0x65cd, 0x11f0, 0xffbf}, {0x07ab, 0x655e, 0x1338, 0xffb6},
    {0x06e4, 0x64d9, 0x148c, 0xffac}, {0x0628, 0x643f, 0x15eb, 0xffa1},
    {0x0577, 0x638f, 0x1756, 0xff96}, {0x04d1, 0x62cb, 0x18cb, 0xff8a},
    {0x0435, 0x61f3, 0x1a4c, 0xff7e}, {0x03a4, 0x6106, 0x1bd7, 0xff71},
    {0x031c, 0x6007, 0x1d6c, 0xff64}, {0x029f, 0x5ef5, 0x1f0b, 0xff56},
    {0x022a, 0x5dd0, 0x20b3, 0xff48}, {0x01be, 0x5c9a, 0x2264, 0xff3a},
    {0x015b, 0x5b53, 0x241e, 0xff2c}, {0x0101, 0x59fc, 0x25e0, 0xff1e},
    {0x00ae, 0x5896, 0x27a9, 0xff10}, {0x0063, 0x5720, 0x297a, 0xff02},
    {0x001f, 0x559d, 0x2b50, 0xfef4}, {0xffe2, 0x540d, 0x2d2c, 0xfee8},
    {0xffac, 0x5270, 0x2f0d, 0xfedb}, {0xff7c, 0x50c7, 0x30f3, 0xfed0},
    {0xff53, 0x4f14, 0x32dc, 0xfec6}, {0xff2e, 0x4d57, 0x34c8, 0xfebd},
    {0xff0f, 0x4b91, 0x36b6, 0xfeb6}, {0xfef5, 0x49c2, 0x38a5, 0xfeb0},
    {0xfedf, 0x47ed, 0x3a95, 0xfeac}, {0xfece, 0x4611, 0x3c85, 0xfeab},
    {0xfec0, 0x4430, 0x3e74, 0xfeac}, {0xfeb6, 0x424a, 0x4060, 0xfeaf},
    {0xfeaf, 0x4060, 0x424a, 0xfeb6}, {0xfeac, 0x3e74, 0x4430, 0xfec0},
    {0xfeab, 0x3c85, 0x4611, 0xfece}, {0xfeac, 0x3a95, 0x47ed, 0xfedf},
    {0xfeb0, 0x38a5, 0x49c2, 0xfef5}, {0xfeb6, 0x36b6, 0x4b91, 0xff0f},
    {0xfebd, 0x34c8, 0x4d57, 0xff2e}, {0xfec6, 0x32dc, 0x4f14, 0xff53},
    {0xfed0, 0x30f3, 0x50c7, 0xff7c}, {0xfedb, 0x2f0d, 0x5270, 0xffac},
    {0xfee8, 0x2d2c, 0x540d, 0xffe2}, {0xfef4, 0x2b50, 0x559d, 0x001f},
    {0xff02, 0x297a, 0x5720, 0x0063}, {0xff10, 0x27a9, 0x5896, 0x00ae},
    {0xff1e, 0x25e0, 0x59fc, 0x0101}, {0xff2c, 0x241e, 0x5b53, 0x015b},
    {0xff3a, 0x2264, 0x5c9a, 0x01be}, {0xff48, 0x20b3, 0x5dd0, 0x022a},
    {0xff56, 0x1f0b, 0x5ef5, 0x029f}, {0xff64, 0x1d6c, 0x6007, 0x031c},
    {0xff71, 0x1bd7, 0x6106, 0x03a4}, {0xff7e, 0x1a4c, 0x61f3, 0x0435},
    {0xff8a, 0x18cb, 0x62cb, 0x04d1}, {0xff96, 0x1756, 0x638f, 0x0577},
    {0xffa1, 0x15eb, 0x643f, 0x0628}, {0xffac, 0x148c, 0x64d9, 0x06e4},
    {0xffb6, 0x1338, 0x655e, 0x07ab}, {0xffbf, 0x11f0, 0x65cd, 0x087d},
    {0xffc8, 0x10b4, 0x6626, 0x095a}, {0xffd0, 0x0f83, 0x6669, 0x0a44},
    {0xffd8, 0x0e5f, 0x6696, 0x0b39}, {0xffdf, 0x0d46, 0x66ad, 0x0c39}
};

static inline int16_t clamp16(int32_t v) {
    if (v < -0x8000) {
        return -0x8000;
    } else if (v > 0x7fff) {
        return 0x7fff;
    }
    return (int16_t)v;
}

static inline int32_t clamp32(int64_t v) {
    if (v < -0x7fffffff - 1) {
        return -0x7fffffff - 1;
    } else if (v > 0x7fffffff) {
        return 0x7fffffff;
    }
    return (int32_t)v;
}

#if HAS_MMI
/*
 * Four resampler outputs. w[k] is the 4-tap input window (any 2-byte
 * alignment), t[k] the 8-byte aligned filter row. Bit-exact with the scalar
 * loop: every product is rounded and shifted on its own (PMULTH keeps all
 * eight 32-bit products), then the four taps are summed and clamped.
 */
static inline void mmiResample4(int16_t *out, const int16_t *w0,
    const int16_t *w1, const int16_t *w2, const int16_t *w3,
    const int16_t *t0, const int16_t *t1, const int16_t *t2,
    const int16_t *t3)
{
    __asm__ __volatile__(
        "ldl     $8, 7(%1)\n\t"
        "ldr     $8, 0(%1)\n\t"
        "ldl     $9, 7(%2)\n\t"
        "ldr     $9, 0(%2)\n\t"
        "ldl     $10, 7(%3)\n\t"
        "ldr     $10, 0(%3)\n\t"
        "ldl     $11, 7(%4)\n\t"
        "ldr     $11, 0(%4)\n\t"
        "ld      $12, 0(%5)\n\t"
        "ld      $13, 0(%6)\n\t"
        "ld      $14, 0(%7)\n\t"
        "ld      $15, 0(%8)\n\t"
        "pcpyld  $8, $9, $8\n\t"      /* {w0, w1} */
        "pcpyld  $12, $13, $12\n\t"   /* {t0, t1} */
        "pcpyld  $10, $11, $10\n\t"   /* {w2, w3} */
        "pcpyld  $14, $15, $14\n\t"   /* {t2, t3} */
        "lq      $15, 0(%9)\n\t"      /* 0x4000 words */
        "pmulth  $0, $8, $12\n\t"
        "pmflo   $8\n\t"              /* {a0 a1 b0 b1} */
        "pmfhi   $9\n\t"              /* {a2 a3 b2 b3} */
        "pmulth  $0, $10, $14\n\t"
        "paddw   $8, $8, $15\n\t"
        "paddw   $9, $9, $15\n\t"
        "psraw   $8, $8, 15\n\t"
        "psraw   $9, $9, 15\n\t"
        "pmflo   $10\n\t"             /* {c0 c1 d0 d1} */
        "pmfhi   $11\n\t"             /* {c2 c3 d2 d3} */
        "paddw   $8, $8, $9\n\t"      /* {a02 a13 b02 b13} */
        "paddw   $10, $10, $15\n\t"
        "paddw   $11, $11, $15\n\t"
        "psraw   $10, $10, 15\n\t"
        "psraw   $11, $11, 15\n\t"
        "paddw   $10, $10, $11\n\t"   /* {c02 c13 d02 d13} */
        "pextlw  $12, $10, $8\n\t"    /* {a02 c02 a13 c13} */
        "pextuw  $13, $10, $8\n\t"    /* {b02 d02 b13 d13} */
        "pextlw  $8, $13, $12\n\t"    /* {a02 b02 c02 d02} */
        "pextuw  $9, $13, $12\n\t"    /* {a13 b13 c13 d13} */
        "lq      $14, 0(%10)\n\t"
        "lq      $15, 0(%11)\n\t"
        "paddw   $8, $8, $9\n\t"
        "pminw   $8, $8, $14\n\t"
        "pmaxw   $8, $8, $15\n\t"
        "ppach   $8, $0, $8\n\t"
        "sd      $8, 0(%0)\n\t"
        :
        : "r"(out), "r"(w0), "r"(w1), "r"(w2), "r"(w3),
          "r"(t0), "r"(t1), "r"(t2), "r"(t3),
          "r"(&k_mmi_round), "r"(&k_mmi_max16), "r"(&k_mmi_min16)
        : "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15",
          "hi", "lo", "memory");
}
#endif

void aClearBufferImpl(uint16_t addr, int nbytes) {
    nbytes = ROUND_UP_16(nbytes);
    memset(BUF_U8(addr), 0, nbytes);
}

void aLoadBufferImpl(const void *source_addr, uint16_t dest_addr, uint16_t nbytes) {
    memcpy(BUF_U8(dest_addr), source_addr, ROUND_UP_8(nbytes));
}

void aSaveBufferImpl(uint16_t source_addr, int16_t *dest_addr, uint16_t nbytes) {
    memcpy(dest_addr, BUF_S16(source_addr), ROUND_UP_8(nbytes));
}

void aLoadADPCMImpl(int num_entries_times_16, const int16_t *book_source_addr) {
    memcpy(rspa.adpcm_table, book_source_addr, num_entries_times_16);
}

void aInterleaveImpl(void) {
    const int16_t *l = BUF_S16(OFS_MAIN_L);
    const int16_t *r = BUF_S16(OFS_MAIN_R);
    int count = ROUND_UP_16(NUM_SAMPLES) / 8;
    int16_t *d = BUF_S16(OFS_BASE);
    while (count > 0) {
        int16_t l0 = *l++;
        int16_t l1 = *l++;
        int16_t l2 = *l++;
        int16_t l3 = *l++;
        int16_t l4 = *l++;
        int16_t l5 = *l++;
        int16_t l6 = *l++;
        int16_t l7 = *l++;
        int16_t r0 = *r++;
        int16_t r1 = *r++;
        int16_t r2 = *r++;
        int16_t r3 = *r++;
        int16_t r4 = *r++;
        int16_t r5 = *r++;
        int16_t r6 = *r++;
        int16_t r7 = *r++;
        *d++ = l0;
        *d++ = r0;
        *d++ = l1;
        *d++ = r1;
        *d++ = l2;
        *d++ = r2;
        *d++ = l3;
        *d++ = r3;
        *d++ = l4;
        *d++ = r4;
        *d++ = l5;
        *d++ = r5;
        *d++ = l6;
        *d++ = r6;
        *d++ = l7;
        *d++ = r7;
        --count;
    }
}

void aDMEMMoveImpl(uint16_t in_addr, uint16_t out_addr, int nbytes) {
    memmove(BUF_U8(out_addr), BUF_U8(in_addr), ROUND_UP_16(nbytes));
}

void aSetLoopImpl(ADPCM_STATE *adpcm_loop_state) {
    rspa.adpcm_loop_state = adpcm_loop_state;
}

void aADPCMdecImpl(uint8_t flags, ADPCM_STATE state, int nbytes, uint16_t inofs, uint16_t outofs) {
#if HAS_SSE41
    const __m128i tblrev = _mm_setr_epi8(12, 13, 10, 11, 8, 9, 6, 7, 4, 5, 2, 3, 0, 1, -1, -1);
    const __m128i pos0 = _mm_set_epi8(3, -1, 3, -1, 2, -1, 2, -1, 1, -1, 1, -1, 0, -1, 0, -1);
    const __m128i pos1 = _mm_set_epi8(7, -1, 7, -1, 6, -1, 6, -1, 5, -1, 5, -1, 4, -1, 4, -1);
    const __m128i mult = _mm_set_epi16(0x10, 0x01, 0x10, 0x01, 0x10, 0x01, 0x10, 0x01);
    const __m128i mask = _mm_set1_epi16((int16_t)0xf000);
#elif HAS_NEON
    static const int8_t pos0_data[] = {-1, 0, -1, 0, -1, 1, -1, 1, -1, 2, -1, 2, -1, 3, -1, 3};
    static const int8_t pos1_data[] = {-1, 4, -1, 4, -1, 5, -1, 5, -1, 6, -1, 6, -1, 7, -1, 7};
    static const int16_t mult_data[] = {0x01, 0x10, 0x01, 0x10, 0x01, 0x10, 0x01, 0x10};
    static const int16_t table_prefix_data[] = {0, 0, 0, 0, 0, 0, 0, 1 << 11};
    const int8x16_t pos0 = vld1q_s8(pos0_data);
    const int8x16_t pos1 = vld1q_s8(pos1_data);
    const int16x8_t mult = vld1q_s16(mult_data);
    const int16x8_t mask = vdupq_n_s16((int16_t)0xf000);
    const int16x8_t table_prefix = vld1q_s16(table_prefix_data);
#endif
    uint8_t *in = BUF_U8(inofs);
    int16_t *out = BUF_S16(outofs);
    nbytes = ROUND_UP_32(nbytes);
    if (flags & A_INIT) {
        memset(out, 0, 16 * sizeof(int16_t));
    } else if (flags & A_LOOP) {
        memcpy(out, rspa.adpcm_loop_state, 16 * sizeof(int16_t));
    } else {
        memcpy(out, state, 16 * sizeof(int16_t));
    }
    out += 16;
#if HAS_SSE41
    __m128i prev_interleaved = _mm_set1_epi32((uint16_t)out[-2] | ((uint16_t)out[-1] << 16));
    //__m128i prev_interleaved = _mm_shuffle_epi32(_mm_loadu_si32(out - 2), 0); // GCC misses this?
#elif HAS_NEON
    int16x8_t result = vld1q_s16(out - 8);
#endif
    while (nbytes > 0) {
        int shift = *in >> 4; // should be in 0..12
        int table_index = *in++ & 0xf; // should be in 0..7
        int16_t (*tbl)[8] = rspa.adpcm_table[table_index];
        int i;
#if HAS_SSE41
        // The _mm_loadu_si64 instruction was added in GCC 9, and results in the same
        // asm as the following instructions, so better be compatible with old GCC.
        //__m128i inv = _mm_loadu_si64(in);
        uint64_t v; memcpy(&v, in, 8);
        __m128i inv = _mm_set_epi64x(0, v);
        __m128i invec[2] = {_mm_shuffle_epi8(inv, pos0), _mm_shuffle_epi8(inv, pos1)};
        __m128i tblvec0 = _mm_loadu_si128((const __m128i *)tbl[0]);
        __m128i tblvec1 = _mm_loadu_si128((const __m128i *)(tbl[1]));
        __m128i tbllo = _mm_unpacklo_epi16(tblvec0, tblvec1);
        __m128i tblhi = _mm_unpackhi_epi16(tblvec0, tblvec1);
        __m128i shiftcount = _mm_set_epi64x(0, 12 - shift); // _mm_cvtsi64_si128 does not exist on 32-bit x86
        __m128i tblvec1_rev[8];

        tblvec1_rev[0] = _mm_insert_epi16(_mm_shuffle_epi8(tblvec1, tblrev), 1 << 11, 7);
        tblvec1_rev[1] = _mm_bsrli_si128(tblvec1_rev[0], 2);
        tblvec1_rev[2] = _mm_bsrli_si128(tblvec1_rev[0], 4);
        tblvec1_rev[3] = _mm_bsrli_si128(tblvec1_rev[0], 6);
        tblvec1_rev[4] = _mm_bsrli_si128(tblvec1_rev[0], 8);
        tblvec1_rev[5] = _mm_bsrli_si128(tblvec1_rev[0], 10);
        tblvec1_rev[6] = _mm_bsrli_si128(tblvec1_rev[0], 12);
        tblvec1_rev[7] = _mm_bsrli_si128(tblvec1_rev[0], 14);
        in += 8;
        for (i = 0; i < 2; i++) {
            __m128i acc0 = _mm_madd_epi16(prev_interleaved, tbllo);
            __m128i acc1 = _mm_madd_epi16(prev_interleaved, tblhi);
            __m128i muls[8];
            __m128i result;
            invec[i] = _mm_sra_epi16(_mm_and_si128(_mm_mullo_epi16(invec[i], mult), mask), shiftcount);

            muls[7] = _mm_madd_epi16(tblvec1_rev[0], invec[i]);
            muls[6] = _mm_madd_epi16(tblvec1_rev[1], invec[i]);
            muls[5] = _mm_madd_epi16(tblvec1_rev[2], invec[i]);
            muls[4] = _mm_madd_epi16(tblvec1_rev[3], invec[i]);
            muls[3] = _mm_madd_epi16(tblvec1_rev[4], invec[i]);
            muls[2] = _mm_madd_epi16(tblvec1_rev[5], invec[i]);
            muls[1] = _mm_madd_epi16(tblvec1_rev[6], invec[i]);
            muls[0] = _mm_madd_epi16(tblvec1_rev[7], invec[i]);

            acc0 = _mm_add_epi32(acc0, _mm_hadd_epi32(_mm_hadd_epi32(muls[0], muls[1]), _mm_hadd_epi32(muls[2], muls[3])));
            acc1 = _mm_add_epi32(acc1, _mm_hadd_epi32(_mm_hadd_epi32(muls[4], muls[5]), _mm_hadd_epi32(muls[6], muls[7])));

            acc0 = _mm_srai_epi32(acc0, 11);
            acc1 = _mm_srai_epi32(acc1, 11);

            result = _mm_packs_epi32(acc0, acc1);
            _mm_storeu_si128((__m128i *)out, result);
            out += 8;

            prev_interleaved = _mm_shuffle_epi32(result, _MM_SHUFFLE(3, 3, 3, 3));
        }
#elif HAS_NEON
        int8x8_t inv = vld1_s8((int8_t *)in);
        int16x8_t tblvec[2] = {vld1q_s16(tbl[0]), vld1q_s16(tbl[1])};
        int16x8_t invec[2] = {vreinterpretq_s16_s8(vcombine_s8(vtbl1_s8(inv, vget_low_s8(pos0)),
                                                               vtbl1_s8(inv, vget_high_s8(pos0)))),
                              vreinterpretq_s16_s8(vcombine_s8(vtbl1_s8(inv, vget_low_s8(pos1)),
                                                               vtbl1_s8(inv, vget_high_s8(pos1))))};
        int16x8_t shiftcount = vdupq_n_s16(shift - 12); // negative means right shift
        int16x8_t tblvec1[8];

        in += 8;
        tblvec1[0] = vextq_s16(table_prefix, tblvec[1], 7);
        invec[0] = vmulq_s16(invec[0], mult);
        tblvec1[1] = vextq_s16(table_prefix, tblvec[1], 6);
        invec[1] = vmulq_s16(invec[1], mult);
        tblvec1[2] = vextq_s16(table_prefix, tblvec[1], 5);
        tblvec1[3] = vextq_s16(table_prefix, tblvec[1], 4);
        invec[0] = vandq_s16(invec[0], mask);
        tblvec1[4] = vextq_s16(table_prefix, tblvec[1], 3);
        invec[1] = vandq_s16(invec[1], mask);
        tblvec1[5] = vextq_s16(table_prefix, tblvec[1], 2);
        tblvec1[6] = vextq_s16(table_prefix, tblvec[1], 1);
        invec[0] = vqshlq_s16(invec[0], shiftcount);
        invec[1] = vqshlq_s16(invec[1], shiftcount);
        tblvec1[7] = table_prefix;
        for (i = 0; i < 2; i++) {
            int32x4_t acc0;
            int32x4_t acc1;

            acc1 = vmull_lane_s16(vget_high_s16(tblvec[0]), vget_high_s16(result), 2);
            acc1 = vmlal_lane_s16(acc1, vget_high_s16(tblvec[1]), vget_high_s16(result), 3);
            acc0 = vmull_lane_s16(vget_low_s16(tblvec[0]), vget_high_s16(result), 2);
            acc0 = vmlal_lane_s16(acc0, vget_low_s16(tblvec[1]), vget_high_s16(result), 3);

            acc0 = vmlal_lane_s16(acc0, vget_low_s16(tblvec1[0]), vget_low_s16(invec[i]), 0);
            acc0 = vmlal_lane_s16(acc0, vget_low_s16(tblvec1[1]), vget_low_s16(invec[i]), 1);
            acc0 = vmlal_lane_s16(acc0, vget_low_s16(tblvec1[2]), vget_low_s16(invec[i]), 2);
            acc0 = vmlal_lane_s16(acc0, vget_low_s16(tblvec1[3]), vget_low_s16(invec[i]), 3);

            acc1 = vmlal_lane_s16(acc1, vget_high_s16(tblvec1[0]), vget_low_s16(invec[i]), 0);
            acc1 = vmlal_lane_s16(acc1, vget_high_s16(tblvec1[1]), vget_low_s16(invec[i]), 1);
            acc1 = vmlal_lane_s16(acc1, vget_high_s16(tblvec1[2]), vget_low_s16(invec[i]), 2);
            acc1 = vmlal_lane_s16(acc1, vget_high_s16(tblvec1[3]), vget_low_s16(invec[i]), 3);
            acc1 = vmlal_lane_s16(acc1, vget_high_s16(tblvec1[4]), vget_high_s16(invec[i]), 0);
            acc1 = vmlal_lane_s16(acc1, vget_high_s16(tblvec1[5]), vget_high_s16(invec[i]), 1);
            acc1 = vmlal_lane_s16(acc1, vget_high_s16(tblvec1[6]), vget_high_s16(invec[i]), 2);
            acc1 = vmlal_lane_s16(acc1, vget_high_s16(tblvec1[7]), vget_high_s16(invec[i]), 3);

            result = vcombine_s16(vqshrn_n_s32(acc0, 11), vqshrn_n_s32(acc1, 11));
            vst1q_s16(out, result);
            out += 8;
        }
#else
        for (i = 0; i < 2; i++) {
            int16_t ins[8];
            int16_t prev1 = out[-1];
            int16_t prev2 = out[-2];
            int j, k;
            for (j = 0; j < 4; j++) {
                ins[j * 2] = (((*in >> 4) << 28) >> 28) << shift;
                ins[j * 2 + 1] = (((*in++ & 0xf) << 28) >> 28) << shift;
            }
            for (j = 0; j < 8; j++) {
                int32_t acc = tbl[0][j] * prev2 + tbl[1][j] * prev1 + (ins[j] << 11);
                for (k = 0; k < j; k++) {
                    acc += tbl[1][((j - k) - 1)] * ins[k];
                }
                acc >>= 11;
                *out++ = clamp16(acc);
            }
        }
#endif
        nbytes -= 16 * sizeof(int16_t);
    }
    memcpy(state, out - 16, 16 * sizeof(int16_t));
}

void aResampleImpl(uint8_t flags, uint16_t pitch, RESAMPLE_STATE state, uint16_t inofs, uint8_t outflag) {
    int16_t tmp[16];
    int16_t *in_initial = BUF_S16(inofs);
    int16_t *in = in_initial;
    int16_t *out = BUF_S16(OFS_BASE + ((outflag & 3) != 0) * NUM_BYTES);
    int nbytes = ROUND_UP_16(NUM_BYTES);
    uint32_t pitch_accumulator;
    int i;
#if !HAS_SSE41 && !HAS_NEON
    int16_t *tbl = NULL;
    int32_t sample = 0;
#endif
    if (flags & A_INIT) {
        memset(tmp, 0, 5 * sizeof(int16_t));
    } else {
        memcpy(tmp, state, 16 * sizeof(int16_t));
    }
    if (flags & 2) {
        memcpy(in - 8, tmp + 8, 8 * sizeof(int16_t));
        in -= tmp[5] / sizeof(int16_t);
    }
    in -= 4;
    pitch_accumulator = (uint16_t)tmp[4];
    memcpy(in, tmp, 4 * sizeof(int16_t));

#if HAS_SSE41
    __m128i multiples = _mm_setr_epi16(0, 2, 4, 6, 8, 10, 12, 14);
    __m128i pitchvec = _mm_set1_epi16((int16_t)pitch);
    __m128i pitchvec_8_steps = _mm_set1_epi32((pitch << 1) * 8);
    __m128i pitchacclo_vec = _mm_set1_epi32((uint16_t)pitch_accumulator);
    __m128i pl = _mm_mullo_epi16(multiples, pitchvec);
    __m128i ph = _mm_mulhi_epu16(multiples, pitchvec);
    __m128i acc_a = _mm_add_epi32(_mm_unpacklo_epi16(pl, ph), pitchacclo_vec);
    __m128i acc_b = _mm_add_epi32(_mm_unpackhi_epi16(pl, ph), pitchacclo_vec);

    do {
        __m128i tbl_positions = _mm_srli_epi16(_mm_packus_epi32(
            _mm_and_si128(acc_a, _mm_set1_epi32(0xffff)),
            _mm_and_si128(acc_b, _mm_set1_epi32(0xffff))), 10);

        __m128i in_positions = _mm_packus_epi32(_mm_srli_epi32(acc_a, 16), _mm_srli_epi32(acc_b, 16));
        __m128i tbl_entries[4];
        __m128i samples[4];

        /*for (i = 0; i < 4; i++) {
            tbl_entries[i] = _mm_castpd_si128(_mm_loadh_pd(_mm_load_sd(
                (const double *)resample_table[_mm_extract_epi16(tbl_positions, 2 * i)]),
                (const double *)resample_table[_mm_extract_epi16(tbl_positions, 2 * i + 1)]));
            samples[i] = _mm_castpd_si128(_mm_loadh_pd(_mm_load_sd(
                (const double *)&in[_mm_extract_epi16(in_positions, 2 * i)]),
                (const double *)&in[_mm_extract_epi16(in_positions, 2 * i + 1)]));
            samples[i] = _mm_mulhrs_epi16(samples[i], tbl_entries[i]);
        }*/
        tbl_entries[0] = LOADLH(resample_table[_mm_extract_epi16(tbl_positions, 0)], resample_table[_mm_extract_epi16(tbl_positions, 1)]);
        tbl_entries[1] = LOADLH(resample_table[_mm_extract_epi16(tbl_positions, 2)], resample_table[_mm_extract_epi16(tbl_positions, 3)]);
        tbl_entries[2] = LOADLH(resample_table[_mm_extract_epi16(tbl_positions, 4)], resample_table[_mm_extract_epi16(tbl_positions, 5)]);
        tbl_entries[3] = LOADLH(resample_table[_mm_extract_epi16(tbl_positions, 6)], resample_table[_mm_extract_epi16(tbl_positions, 7)]);
        samples[0] = LOADLH(&in[_mm_extract_epi16(in_positions, 0)], &in[_mm_extract_epi16(in_positions, 1)]);
        samples[1] = LOADLH(&in[_mm_extract_epi16(in_positions, 2)], &in[_mm_extract_epi16(in_positions, 3)]);
        samples[2] = LOADLH(&in[_mm_extract_epi16(in_positions, 4)], &in[_mm_extract_epi16(in_positions, 5)]);
        samples[3] = LOADLH(&in[_mm_extract_epi16(in_positions, 6)], &in[_mm_extract_epi16(in_positions, 7)]);
        samples[0] = _mm_mulhrs_epi16(samples[0], tbl_entries[0]);
        samples[1] = _mm_mulhrs_epi16(samples[1], tbl_entries[1]);
        samples[2] = _mm_mulhrs_epi16(samples[2], tbl_entries[2]);
        samples[3] = _mm_mulhrs_epi16(samples[3], tbl_entries[3]);

        _mm_storeu_si128((__m128i *)out, _mm_hadds_epi16(_mm_hadds_epi16(samples[0], samples[1]), _mm_hadds_epi16(samples[2], samples[3])));

        acc_a = _mm_add_epi32(acc_a, pitchvec_8_steps);
        acc_b = _mm_add_epi32(acc_b, pitchvec_8_steps);
        out += 8;
        nbytes -= 8 * sizeof(int16_t);
    } while (nbytes > 0);
    in += (uint16_t)_mm_extract_epi16(acc_a, 1);
    pitch_accumulator = (uint16_t)_mm_extract_epi16(acc_a, 0);
#elif HAS_NEON
    static const uint16_t multiples_data[8] = {0, 2, 4, 6, 8, 10, 12, 14};
    uint16x8_t multiples = vld1q_u16(multiples_data);
    uint32x4_t pitchvec_8_steps = vdupq_n_u32((pitch << 1) * 8);
    uint32x4_t pitchacclo_vec = vdupq_n_u32((uint16_t)pitch_accumulator);
    uint32x4_t acc_a = vmlal_n_u16(pitchacclo_vec, vget_low_u16(multiples), pitch);
    uint32x4_t acc_b = vmlal_n_u16(pitchacclo_vec, vget_high_u16(multiples), pitch);

    do {
        uint16x8x2_t unzipped = vuzpq_u16(vreinterpretq_u16_u32(acc_a), vreinterpretq_u16_u32(acc_b));
        uint16x8_t tbl_positions = vshrq_n_u16(unzipped.val[0], 10);
        uint16x8_t in_positions = unzipped.val[1];
        int16x8_t tbl_entries[4];
        int16x8_t samples[4];
        int16x8x2_t unzipped1;
        int16x8x2_t unzipped2;

        tbl_entries[0] = vcombine_s16(vld1_s16(resample_table[vgetq_lane_u16(tbl_positions, 0)]), vld1_s16(resample_table[vgetq_lane_u16(tbl_positions, 1)]));
        tbl_entries[1] = vcombine_s16(vld1_s16(resample_table[vgetq_lane_u16(tbl_positions, 2)]), vld1_s16(resample_table[vgetq_lane_u16(tbl_positions, 3)]));
        tbl_entries[2] = vcombine_s16(vld1_s16(resample_table[vgetq_lane_u16(tbl_positions, 4)]), vld1_s16(resample_table[vgetq_lane_u16(tbl_positions, 5)]));
        tbl_entries[3] = vcombine_s16(vld1_s16(resample_table[vgetq_lane_u16(tbl_positions, 6)]), vld1_s16(resample_table[vgetq_lane_u16(tbl_positions, 7)]));
        samples[0] = vcombine_s16(vld1_s16(&in[vgetq_lane_u16(in_positions, 0)]), vld1_s16(&in[vgetq_lane_u16(in_positions, 1)]));
        samples[1] = vcombine_s16(vld1_s16(&in[vgetq_lane_u16(in_positions, 2)]), vld1_s16(&in[vgetq_lane_u16(in_positions, 3)]));
        samples[2] = vcombine_s16(vld1_s16(&in[vgetq_lane_u16(in_positions, 4)]), vld1_s16(&in[vgetq_lane_u16(in_positions, 5)]));
        samples[3] = vcombine_s16(vld1_s16(&in[vgetq_lane_u16(in_positions, 6)]), vld1_s16(&in[vgetq_lane_u16(in_positions, 7)]));
        samples[0] = vqrdmulhq_s16(samples[0], tbl_entries[0]);
        samples[1] = vqrdmulhq_s16(samples[1], tbl_entries[1]);
        samples[2] = vqrdmulhq_s16(samples[2], tbl_entries[2]);
        samples[3] = vqrdmulhq_s16(samples[3], tbl_entries[3]);

        unzipped1 = vuzpq_s16(samples[0], samples[1]);
        unzipped2 = vuzpq_s16(samples[2], samples[3]);
        samples[0] = vqaddq_s16(unzipped1.val[0], unzipped1.val[1]);
        samples[1] = vqaddq_s16(unzipped2.val[0], unzipped2.val[1]);
        unzipped1 = vuzpq_s16(samples[0], samples[1]);
        samples[0] = vqaddq_s16(unzipped1.val[0], unzipped1.val[1]);

        vst1q_s16(out, samples[0]);

        acc_a = vaddq_u32(acc_a, pitchvec_8_steps);
        acc_b = vaddq_u32(acc_b, pitchvec_8_steps);
        out += 8;
        nbytes -= 8 * sizeof(int16_t);
    } while (nbytes > 0);
    in += vgetq_lane_u16(vreinterpretq_u16_u32(acc_a), 1);
    pitch_accumulator = vgetq_lane_u16(vreinterpretq_u16_u32(acc_a), 0);
#elif HAS_MMI
    {
        const uint32_t step = (uint32_t)pitch << 1;
        const int16_t *w[4];
        const int16_t *t[4];
        (void)tbl;
        (void)sample;
        do {
            /* out is 16-byte aligned; each kernel call writes 8 bytes. */
            for (int half = 0; half < 2; ++half) {
                for (i = 0; i < 4; i++) {
                    w[i] = in;
                    t[i] = resample_table[pitch_accumulator >> 10];
                    pitch_accumulator += step;
                    in += pitch_accumulator >> 16;
                    pitch_accumulator &= 0xffffu;
                }
                mmiResample4(out, w[0], w[1], w[2], w[3],
                    t[0], t[1], t[2], t[3]);
                out += 4;
            }
            nbytes -= 8 * sizeof(int16_t);
        } while (nbytes > 0);
    }
#else
    do {
        for (i = 0; i < 8; i++) {
            tbl = resample_table[pitch_accumulator * 64 >> 16];
            sample = ((in[0] * tbl[0] + 0x4000) >> 15) +
                     ((in[1] * tbl[1] + 0x4000) >> 15) +
                     ((in[2] * tbl[2] + 0x4000) >> 15) +
                     ((in[3] * tbl[3] + 0x4000) >> 15);
            *out++ = clamp16(sample);

            pitch_accumulator += (pitch << 1);
            in += pitch_accumulator >> 16;
            pitch_accumulator %= 0x10000;
        }
        nbytes -= 8 * sizeof(int16_t);
    } while (nbytes > 0);
#endif

    state[4] = (int16_t)pitch_accumulator;
    memcpy(state, in, 4 * sizeof(int16_t));
    i = (in - in_initial + 4) & 7;
    in -= i;
    if (i != 0) {
        i = -8 - i;
    }
    state[5] = i;
    memcpy(state + 8, in, 8 * sizeof(int16_t));
}

void aEnvMixerImpl(uint8_t flags, ENVMIX_STATE state, int16_t rvol) {
    int16_t *in = BUF_S16(OFS_BASE);
    int16_t *dry[2] = {BUF_S16(OFS_MAIN_L), BUF_S16(OFS_MAIN_R)};
    int16_t *wet[2] = {BUF_S16(OFS_AUX_L), BUF_S16(OFS_AUX_R)};
    int nsamples = NUM_SAMPLES;

    struct {
        int32_t t[2];
        int32_t rate[2];
        int16_t tgt[2];
        int16_t voldry;
        int16_t volwet;
    } *savedstate = (void *)state;

    rspa.vol[1] = rvol; // why the fuck is this here?

    // naudio uses a linear envelope
    // TODO: sse/neon

    int32_t t[2], tgt[2], rate[2];
    int16_t voldry, volwet;

    if (flags & A_INIT) {
        for (int i = 0; i < 2; ++i) {
           t[i] = rspa.vol[i] << 16;
            rate[i] = rspa.rate[i] >> 3;
            tgt[i] = rspa.target[i] << 16;
        }
        voldry = rspa.vol_dry;
        volwet = rspa.vol_wet;
    } else {
        for (int i = 0; i < 2; ++i) {
            t[i] = savedstate->t[i];
            rate[i] = savedstate->rate[i];
            tgt[i] = savedstate->tgt[i] << 16;
        }
        voldry = savedstate->voldry;
        volwet = savedstate->volwet;
    }

    #ifdef PLATFORM_BIG_ENDIAN
    #define XOR 0
    #else
    #define XOR 1
    #endif

    int16_t *outs[4] = {dry[0], dry[1], wet[0], wet[1]};
    int16_t gain[4] = {0, 0, 0, 0};
    int i = 0;

    // Per-sample envelope while either side is still ramping. Once both rates
    // are zero the envelope is frozen (t is either at its target or above a
    // target it can never be clamped down to), so the remaining samples share
    // one set of gains and can be mixed linearly. The switch happens on an
    // even sample so the XOR-swizzled index set stays identical.
    for (; i < nsamples; ++i) {
        int16_t vol[2];

        for (int j = 0; j < 2; ++j) {
            t[j] += rate[j];
            if ((rate[j] <= 0 && t[j] <= tgt[j]) || (rate[j] > 0 && t[j] >= tgt[j])) {
                t[j] = tgt[j];
                rate[j] = 0;
            }
            vol[j] = t[j] >> 16;
        }

        gain[0] = clamp16((vol[0] * voldry + 0x4000) >> 15);
        gain[1] = clamp16((vol[1] * voldry + 0x4000) >> 15);
        gain[2] = clamp16((vol[0] * volwet + 0x4000) >> 15);
        gain[3] = clamp16((vol[1] * volwet + 0x4000) >> 15);

        const int idx = i ^ XOR;
        const int32_t insamp = in[idx];
        for (int j = 0; j < 4; ++j) {
            // a zero gain adds exactly zero to an already clamped sample
            if (gain[j] != 0) {
                outs[j][idx] = clamp16(outs[j][idx] + ((insamp * gain[j]) >> 15));
            }
        }

        if (rate[0] == 0 && rate[1] == 0 && ((i + 1) & 1) == 0) {
            ++i;
            break;
        }
    }

    if (i < nsamples) {
        for (int j = 0; j < 4; ++j) {
            const int32_t g = gain[j];
            if (g != 0) {
                int16_t *out = outs[j];
                int k = i;
#if HAS_MMI
                if (g != -0x8000) {
                    for (; k < nsamples && (k & 7) != 0; ++k) {
                        out[k] = clamp16(out[k] + ((in[k] * g) >> 15));
                    }
                    if (mmiAligned(out + k, in + k)) {
                        const int blocks = (nsamples - k) >> 3;
                        mmiMixScaledAdd(out + k, in + k, (int16_t)g, blocks);
                        k += blocks << 3;
                    }
                }
#endif
                for (; k < nsamples; ++k) {
                    out[k] = clamp16(out[k] + ((in[k] * g) >> 15));
                }
            }
        }
    }

    #undef XOR

    for (int i = 0; i < 2; ++i) {
        savedstate->t[i] = t[i];
        savedstate->rate[i] = rate[i];
        savedstate->tgt[i] = tgt[i] >> 16;
    }
    savedstate->voldry = voldry;
    savedstate->volwet = volwet;
}

// flags is always 0 in PD
void aMixImpl(uint8_t flags, int16_t gain, uint16_t in_addr, uint16_t out_addr) {
    int nbytes = ROUND_UP_16(NUM_BYTES); // can't round up to 32 here because 0x170 ain't gonna do that
    int16_t *in = BUF_S16(in_addr);
    int16_t *out = BUF_S16(out_addr);
#if HAS_SSE41
    __m128i gain_vec = _mm_set1_epi16(gain);
#elif !HAS_NEON
    int i;
    int32_t sample;
#endif

#if HAS_MMI
    if (mmiAligned(out, in)) {
        if (gain == -0x8000) {
            mmiMixSubtract(out, in, nbytes >> 4);
        } else {
            mmiMixGeneral(out, in, gain, nbytes >> 4);
        }
        return;
    }
#endif

#if !HAS_NEON
    if (gain == -0x8000) {
        while (nbytes > 0) {
#if HAS_SSE41
            __m128i out1, in1;
            out1 = _mm_loadu_si128((const __m128i *)out);
            in1 = _mm_loadu_si128((const __m128i *)in);

            out1 = _mm_subs_epi16(out1, in1);

            _mm_storeu_si128((__m128i *)out, out1);

            out += 8;
            in += 8;
#else
            for (i = 0; i < 8; i++) {
                sample = *out - *in++;
                *out++ = clamp16(sample);
            }
#endif

            nbytes -= 8 * sizeof(int16_t);
        }
    }
#endif

    while (nbytes > 0) {
#if HAS_SSE41
        __m128i out1, in1;
        out1 = _mm_loadu_si128((const __m128i *)out);
        in1 = _mm_loadu_si128((const __m128i *)in);

        out1 = _mm_adds_epi16(out1, _mm_mulhrs_epi16(in1, gain_vec));

        _mm_storeu_si128((__m128i *)out, out1);

        out += 8;
        in += 8;
#elif HAS_NEON
        int16x8_t out1, in1;
        out1 = vld1q_s16(out);
        in1 = vld1q_s16(in);

        out1 = vqaddq_s16(out1, vqrdmulhq_n_s16(in1, gain));

        vst1q_s16(out, out1);

        out += 8;
        in += 8;
#else
        for (i = 0; i < 8; i++) {
            sample = ((*out * 0x7fff + *in++ * gain) + 0x4000) >> 15;
            *out++ = clamp16(sample);
        }
#endif

        nbytes -= 8 * sizeof(int16_t);
    }
}

void aSetVolumeImpl(uint8_t flags, int16_t v, int16_t t, int16_t r) {
    // the flags are not really very intuitive here
    if (flags & A_VOL) {
        if (flags & A_LEFT) {
            rspa.vol[0] = v;
            rspa.vol_dry = t;
            rspa.vol_wet = r;
        } else {
            rspa.target[1] = v;
            rspa.rate[1] = (int32_t)((uint16_t)t << 16 | ((uint16_t)r));
        }
    } else /* A_RATE */ {
        rspa.target[0] = v;
        rspa.rate[0] = (int32_t)((uint16_t)t << 16 | ((uint16_t)r));
    }
}

void aPlayMP3Impl(const void *mp3file, u32 mp3size, void *out, int reset) {
    static mp3dec_t mp3d;
    static const u8 *curdata = NULL; // pointer to the mp3 we're currently processing
    static s32 dataptr = 0; // byte index into curdata

    if (mp3file != curdata || reset) {
        // new mp3, reinit decoder
        mp3dec_init(&mp3d);
        curdata = mp3file;
        dataptr = 0;
    }

    // this command is supposed to write one full frame to out
    // but which frame? we'll just decode sequentially, it'll probably work
    if (dataptr < mp3size) {
        // FIXME: decoding straight to out might bite us in the ass because it's only 1160 bytes
        mp3dec_frame_info_t info;
        const s32 samples = mp3dec_decode_frame(&mp3d, curdata + dataptr, mp3size - dataptr, out, &info);
        // fill in the rest of the buffer if frame is smaller
        const s32 diff = 580 - samples;
        if (diff > 0) {
            memset((s16 *)out + samples, 0, diff * 2);
        } else {
            assert(diff == 0);
        }
        dataptr += info.frame_bytes;
    } else {
        // empty frame
        memset(out, 0, 580 * 2);
    }
}

void aPoleFilterImpl(uint8_t flags, int16_t gain, uint32_t t, uint32_t addr) {
    // this never gets called?
}

void aDisableImpl(uint16_t outp, uint32_t b, uint32_t c) {
    // this never gets called?
}

#if HAS_MMI && defined(PD_MIXER_MMI_SELFTEST)
#include "system.h"
// On-target equivalence check of the MMI kernels against the scalar
// formulas. Debug builds only (needs real R5900 or PCSX2).
void mixerMmiSelfTest(void)
{
    static mmi_h8 outa[24], outb[24], in[24];
    uint32_t seed = 0x1234567u;
    unsigned fails = 0, cases = 0;
    for (int it = 0; it < 4000; ++it) {
        for (int k = 0; k < 24 * 8; ++k) {
            seed = seed * 1664525u + 1013904223u;
            int16_t v = (int16_t)(seed >> 8);
            if ((seed & 0x70) == 0) v = (seed & 0x80) ? 0x7fff : -0x8000;
            ((int16_t *)in)[k] = v;
            seed = seed * 1664525u + 1013904223u;
            v = (int16_t)(seed >> 8);
            if ((seed & 0x70) == 0) v = (seed & 0x80) ? 0x7fff : -0x8000;
            ((int16_t *)outa)[k] = ((int16_t *)outb)[k] = v;
        }
        seed = seed * 1664525u + 1013904223u;
        int16_t g = (int16_t)(seed >> 8);
        if (it % 7 == 0) g = 0x7fff;
        if (it % 11 == 0) g = -0x7fff;
        if (it % 13 == 0) g = -0x8000;
        int16_t *a = (int16_t *)outa, *b = (int16_t *)outb, *x = (int16_t *)in;
        int mode = it % 3;
        if (mode == 0) {
            if (g == -0x8000) mmiMixSubtract(a, x, 23); else mmiMixGeneral(a, x, g, 23);
            for (int k = 0; k < 184; ++k)
                b[k] = g == -0x8000 ? clamp16(b[k] - x[k])
                    : clamp16(((b[k] * 0x7fff + x[k] * g) + 0x4000) >> 15);
        } else {
            if (g == -0x8000) g = -0x7fff;
            mmiMixScaledAdd(a, x, g, 23);
            for (int k = 0; k < 184; ++k)
                b[k] = clamp16(b[k] + ((x[k] * g) >> 15));
        }
        ++cases;
        if (memcmp(a, b, 184 * 2) != 0) {
            if (fails++ < 4) {
                int k = 0;
                while (a[k] == b[k]) ++k;
                sysLogPrintf(LOG_ERROR, "MMI selftest mismatch mode=%d g=%d k=%d mmi=%d ref=%d in=%d",
                    mode, g, k, a[k], b[k], x[k]);
            }
        }
    }
    /* Resampler kernel: random windows at every alignment, random rows. */
    {
        static int16_t src[64] __attribute__((aligned(16)));
        static int16_t got[4] __attribute__((aligned(16)));
        for (int it = 0; it < 4000; ++it) {
            for (int k = 0; k < 64; ++k) {
                seed = seed * 1664525u + 1013904223u;
                int16_t v = (int16_t)(seed >> 8);
                if ((seed & 0x70) == 0) v = (seed & 0x80) ? 0x7fff : -0x8000;
                src[k] = v;
            }
            const int16_t *w[4];
            const int16_t *t[4];
            for (int k = 0; k < 4; ++k) {
                seed = seed * 1664525u + 1013904223u;
                w[k] = &src[(seed >> 8) % 60u];
                t[k] = resample_table[(seed >> 20) & 63u];
            }
            mmiResample4(got, w[0], w[1], w[2], w[3], t[0], t[1], t[2], t[3]);
            ++cases;
            for (int k = 0; k < 4; ++k) {
                const int32_t ref = ((w[k][0] * t[k][0] + 0x4000) >> 15) +
                    ((w[k][1] * t[k][1] + 0x4000) >> 15) +
                    ((w[k][2] * t[k][2] + 0x4000) >> 15) +
                    ((w[k][3] * t[k][3] + 0x4000) >> 15);
                if (got[k] != clamp16(ref)) {
                    if (fails++ < 8) {
                        sysLogPrintf(LOG_ERROR, "MMI resample mismatch lane=%d mmi=%d ref=%d",
                            k, got[k], clamp16(ref));
                    }
                    break;
                }
            }
        }
    }
    sysLogPrintf(fails ? LOG_ERROR : LOG_NOTE, "MMI mixer selftest: %u/%u cases failed", fails, cases);
}
#endif
