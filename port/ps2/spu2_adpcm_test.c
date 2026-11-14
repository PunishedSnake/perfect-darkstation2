#include "spu2_adpcm.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static double snr_db(const int16_t *ref, const int16_t *got, uint32_t n)
{
    double sig = 0.0, err = 0.0;
    for (uint32_t i = 0; i < n; ++i) {
        sig += (double)ref[i] * ref[i];
        const double d = (double)ref[i] - got[i];
        err += d * d;
    }
    return err == 0.0 ? 200.0 : 10.0 * log10(sig / err);
}

static void test_psadpcm_round_trip(void)
{
    enum { N = 22050 };
    static int16_t pcm[N], dec[N + 28];
    static uint8_t enc[(N / 28 + 2) * 16];
    /* Sine sweep plus a little noise, like a typical effect. */
    uint32_t seed = 12345u;
    double phase = 0.0;
    for (uint32_t i = 0; i < N; ++i) {
        phase += 2.0 * 3.14159265 * (100.0 + 4000.0 * i / N) / 22050.0;
        seed = seed * 1664525u + 1013904223u;
        pcm[i] = (int16_t)(12000.0 * sin(phase) + (int32_t)(seed >> 22) - 512);
    }
    const uint32_t bytes = spu2PsAdpcmEncode(pcm, N, false, 0u, enc);
    assert(bytes == spu2PsAdpcmEncodedSize(N));
    int16_t hist[2] = { 0, 0 };
    const uint32_t samples = spu2PsAdpcmDecode(enc, bytes / 16u, hist, dec);
    assert(samples >= N);
    const double snr = snr_db(pcm, dec, N);
    printf("PS-ADPCM sweep SNR %.1f dB\n", snr);
    assert(snr > 25.0);
    /* only the last block ends the voice */
    for (uint32_t b = 0; b + 1 < bytes / 16u; ++b) {
        assert(enc[b * 16 + 1] == 0u);
    }
    assert(enc[bytes - 16 + 1] == SPU2_PSADPCM_FLAG_LOOP_END);
}

static void test_psadpcm_loop_flags(void)
{
    int16_t pcm[28 * 5];
    for (uint32_t i = 0; i < 28 * 5; ++i) {
        pcm[i] = (int16_t)((i * 977) & 0x3fff);
    }
    uint8_t enc[16 * 5];
    const uint32_t bytes = spu2PsAdpcmEncode(pcm, 28 * 5, true, 60u, enc);
    assert(bytes == 16 * 5);
    assert(enc[2 * 16 + 1] == SPU2_PSADPCM_FLAG_LOOP_START); /* 60 / 28 = 2 */
    assert(enc[4 * 16 + 1] == (SPU2_PSADPCM_FLAG_LOOP_END | SPU2_PSADPCM_FLAG_REPEAT));
}

static void test_silence_is_exact(void)
{
    int16_t pcm[56] = { 0 }, dec[56];
    uint8_t enc[32];
    spu2PsAdpcmEncode(pcm, 56, false, 0u, enc);
    int16_t hist[2] = { 0, 0 };
    spu2PsAdpcmDecode(enc, 2, hist, dec);
    assert(memcmp(pcm, dec, sizeof(pcm)) == 0);
}

static void test_vadpcm_decode_zero_book(void)
{
    /* With an all-zero predictor book a sample is just nibble << shift. */
    int16_t book[16] = { 0 };
    uint8_t frame[9] = { 0x30, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef };
    int16_t out[16];
    int16_t hist[2] = { 0, 0 };
    assert(spu2VadpcmDecode(frame, 1, book, 2, 1, hist, out) == 16u);
    for (int i = 0; i < 16; ++i) {
        const int nib = (i & 1) ? (frame[1 + i / 2] & 0xf) : (frame[1 + i / 2] >> 4);
        const int q = (int)((unsigned)nib << 28) >> 28;
        assert(out[i] == (int16_t)(q << 3));
    }
    assert(hist[0] == out[14] && hist[1] == out[15]);
}

static void test_vadpcm_decode_predictor(void)
{
    /* First-order hold: tbl1[0] = 2048 (0.5 in 4.11) feeds prev1 forward. */
    int16_t book[16] = { 0 };
    book[8] = 2048;
    uint8_t frame[9] = { 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0 };
    int16_t out[16];
    int16_t hist[2] = { 0, 100 };
    spu2VadpcmDecode(frame, 1, book, 2, 1, hist, out);
    /* out[0] = (2048*100 + (0 << 11)) >> 11 = 100 */
    assert(out[0] == 100);
}

int main(void)
{
    test_psadpcm_round_trip();
    test_psadpcm_loop_flags();
    test_silence_is_exact();
    test_vadpcm_decode_zero_book();
    test_vadpcm_decode_predictor();
    printf("spu2_adpcm tests passed\n");
    return 0;
}
