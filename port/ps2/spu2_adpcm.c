#include "spu2_adpcm.h"

#include <string.h>

static int16_t spu2Clamp16(int32_t v)
{
    return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v;
}

uint32_t spu2VadpcmDecode(const uint8_t *src, uint32_t frames,
    const int16_t *book, int32_t order, int32_t npredictors,
    int16_t history[2], int16_t *out)
{
    if (!src || !book || !history || !out || order != 2 || npredictors <= 0) {
        return 0u;
    }
    int16_t prev2 = history[0];
    int16_t prev1 = history[1];
    for (uint32_t f = 0u; f < frames; ++f, src += SPU2_VADPCM_FRAME_BYTES) {
        const int32_t shift = src[0] >> 4;
        const int32_t index = src[0] & 0xf;
        if (index >= npredictors) {
            return 0u;
        }
        /* tbl[0] multiplies prev2, tbl[1] prev1 and the in-frame taps. */
        const int16_t *tbl0 = &book[index * 16];
        const int16_t *tbl1 = tbl0 + 8;
        for (int32_t half = 0; half < 2; ++half) {
            int32_t ins[8];
            for (int32_t j = 0; j < 4; ++j) {
                const uint8_t b = src[1 + half * 4 + j];
                ins[j * 2] = (int16_t)(((int32_t)((uint32_t)(b >> 4) << 28) >> 28) << shift);
                ins[j * 2 + 1] = (int16_t)(((int32_t)((uint32_t)(b & 0xf) << 28) >> 28) << shift);
            }
            for (int32_t j = 0; j < 8; ++j) {
                int32_t acc = tbl0[j] * prev2 + tbl1[j] * prev1 + (ins[j] << 11);
                for (int32_t k = 0; k < j; ++k) {
                    acc += tbl1[(j - k) - 1] * ins[k];
                }
                out[half * 8 + j] = spu2Clamp16(acc >> 11);
            }
            prev2 = out[half * 8 + 6];
            prev1 = out[half * 8 + 7];
        }
        out += SPU2_VADPCM_FRAME_SAMPLES;
    }
    history[0] = prev2;
    history[1] = prev1;
    return frames * SPU2_VADPCM_FRAME_SAMPLES;
}

/* SPU2 filter coefficients, scaled by 64. */
static const int32_t k_filters[5][2] = {
    { 0, 0 }, { 60, 0 }, { 115, -52 }, { 98, -55 }, { 122, -60 },
};

static int32_t spu2PsPredict(int32_t filter, int32_t s1, int32_t s2)
{
    return (s1 * k_filters[filter][0] + s2 * k_filters[filter][1] + 32) >> 6;
}

uint32_t spu2PsAdpcmEncodedSize(uint32_t sample_count)
{
    const uint32_t blocks = (sample_count + SPU2_PSADPCM_BLOCK_SAMPLES - 1u) /
        SPU2_PSADPCM_BLOCK_SAMPLES;
    return (blocks == 0u ? 1u : blocks) * SPU2_PSADPCM_BLOCK_BYTES;
}

/*
 * Encode one block with a given filter and shift, tracking the decoder's own
 * history so quantisation error does not drift. Returns the squared error.
 */
static uint64_t spu2PsEncodeBlockWith(const int16_t *pcm, int32_t filter,
    int32_t shift, int32_t s1, int32_t s2, uint8_t nibbles[28],
    int32_t *out_s1, int32_t *out_s2)
{
    uint64_t err = 0u;
    for (int32_t i = 0; i < 28; ++i) {
        const int32_t pred = spu2PsPredict(filter, s1, s2);
        const int32_t residual = pcm[i] - pred;
        /* value = (nibble << 12) >> shift  =>  nibble = residual << shift >> 12 */
        int32_t q;
        const int32_t scaled = residual * (1 << shift);
        q = (scaled + (scaled >= 0 ? 2048 : -2048)) / 4096;
        if (q > 7) q = 7;
        if (q < -8) q = -8;
        nibbles[i] = (uint8_t)(q & 0xf);
        const int32_t decoded = spu2Clamp16(((q * 4096) >> shift) + pred);
        const int32_t d = decoded - pcm[i];
        err += (uint64_t)((int64_t)d * d);
        s2 = s1;
        s1 = decoded;
    }
    *out_s1 = s1;
    *out_s2 = s2;
    return err;
}

uint32_t spu2PsAdpcmEncode(const int16_t *pcm, uint32_t sample_count,
    bool loop, uint32_t loop_start, uint8_t *out)
{
    if (!out || (!pcm && sample_count != 0u)) {
        return 0u;
    }
    const uint32_t bytes = spu2PsAdpcmEncodedSize(sample_count);
    const uint32_t blocks = bytes / SPU2_PSADPCM_BLOCK_BYTES;
    const uint32_t loop_block = loop ? loop_start / SPU2_PSADPCM_BLOCK_SAMPLES : 0u;
    int32_t s1 = 0;
    int32_t s2 = 0;
    for (uint32_t b = 0u; b < blocks; ++b) {
        int16_t block[28];
        const uint32_t first = b * SPU2_PSADPCM_BLOCK_SAMPLES;
        for (uint32_t i = 0u; i < 28u; ++i) {
            block[i] = first + i < sample_count ? pcm[first + i] : 0;
        }
        uint64_t best_err = UINT64_MAX;
        int32_t best_filter = 0;
        int32_t best_shift = 0;
        uint8_t best_nibbles[28];
        int32_t best_s1 = 0;
        int32_t best_s2 = 0;
        for (int32_t filter = 0; filter < 5; ++filter) {
            /* Smallest shift loss for this filter's worst residual, then try
             * one step either side (rounding can favour a neighbour). */
            int32_t ts1 = s1, ts2 = s2, maxres = 0;
            for (uint32_t i = 0u; i < 28u; ++i) {
                const int32_t pred = spu2PsPredict(filter, ts1, ts2);
                int32_t r = block[i] - pred;
                if (r < 0) r = -r;
                if (r > maxres) maxres = r;
                ts2 = ts1;
                ts1 = block[i];
            }
            int32_t shift = 12;
            while (shift > 0 && ((7 * 4096) >> shift) < maxres) {
                --shift;
            }
            for (int32_t cand = shift - 1; cand <= shift + 1; ++cand) {
                if (cand < 0 || cand > 12) {
                    continue;
                }
                uint8_t nib[28];
                int32_t ns1, ns2;
                const uint64_t err = spu2PsEncodeBlockWith(block, filter, cand,
                    s1, s2, nib, &ns1, &ns2);
                if (err < best_err) {
                    best_err = err;
                    best_filter = filter;
                    best_shift = cand;
                    memcpy(best_nibbles, nib, sizeof(nib));
                    best_s1 = ns1;
                    best_s2 = ns2;
                }
            }
        }
        uint8_t *o = &out[b * SPU2_PSADPCM_BLOCK_BYTES];
        o[0] = (uint8_t)((best_filter << 4) | best_shift);
        uint8_t flags = 0u;
        if (loop && b == loop_block) {
            flags |= SPU2_PSADPCM_FLAG_LOOP_START;
        }
        if (b + 1u == blocks) {
            flags |= loop ? (SPU2_PSADPCM_FLAG_LOOP_END | SPU2_PSADPCM_FLAG_REPEAT)
                          : SPU2_PSADPCM_FLAG_LOOP_END;
        }
        o[1] = flags;
        for (uint32_t i = 0u; i < 14u; ++i) {
            o[2 + i] = (uint8_t)(best_nibbles[i * 2] | (best_nibbles[i * 2 + 1] << 4));
        }
        s1 = best_s1;
        s2 = best_s2;
    }
    return bytes;
}

uint32_t spu2PsAdpcmDecode(const uint8_t *src, uint32_t blocks,
    int16_t history[2], int16_t *out)
{
    if (!src || !history || !out) {
        return 0u;
    }
    int32_t s2 = history[0];
    int32_t s1 = history[1];
    for (uint32_t b = 0u; b < blocks; ++b, src += SPU2_PSADPCM_BLOCK_BYTES) {
        const int32_t shift = src[0] & 0xf;
        int32_t filter = (src[0] >> 4) & 0x7;
        if (filter > 4) filter = 0;
        for (uint32_t i = 0u; i < 28u; ++i) {
            const uint8_t byte = src[2 + i / 2];
            const int32_t nib = (i & 1u) ? (byte >> 4) : (byte & 0xf);
            const int32_t q = (int32_t)((uint32_t)nib << 28) >> 28;
            const int32_t sample = spu2Clamp16(((q * 4096) >> shift) +
                spu2PsPredict(filter, s1, s2));
            *out++ = (int16_t)sample;
            s2 = s1;
            s1 = sample;
        }
    }
    history[0] = (int16_t)s2;
    history[1] = (int16_t)s1;
    return blocks * SPU2_PSADPCM_BLOCK_SAMPLES;
}
