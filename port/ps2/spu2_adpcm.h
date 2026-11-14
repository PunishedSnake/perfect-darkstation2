#ifndef PD_PS2_SPU2_ADPCM_H
#define PD_PS2_SPU2_ADPCM_H

/*
 * Sample codecs for the SPU2 hardware-voice audio path (see
 * docs/ps2-spu2-audio.md). Portable C, no PS2 dependencies, so the host
 * tests and an offline/first-boot transcoder can share them.
 *
 *  - N64 VADPCM (9-byte frames of 16 samples, order-2 predictor books), as
 *    decoded by the RSP ADPCM microcode / aADPCMdec.
 *  - PS-ADPCM (16-byte blocks of 28 samples, five fixed filters) as played
 *    by SPU2 voices, with its loop flags.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPU2_VADPCM_FRAME_BYTES 9u
#define SPU2_VADPCM_FRAME_SAMPLES 16u
#define SPU2_PSADPCM_BLOCK_BYTES 16u
#define SPU2_PSADPCM_BLOCK_SAMPLES 28u

/* PS-ADPCM block flag bits (byte 1 of each block). */
#define SPU2_PSADPCM_FLAG_LOOP_END 0x01u   /* jump to the loop address */
#define SPU2_PSADPCM_FLAG_REPEAT 0x02u     /* with END: keep playing (loop) */
#define SPU2_PSADPCM_FLAG_LOOP_START 0x04u /* this block is the loop address */

/*
 * Decode whole VADPCM frames. book holds npredictors tables of order (=2)
 * x 8 coefficients, laid out as in ALADPCMBook. history[2] carries the last
 * two output samples between calls (zero for a fresh sample). Returns the
 * number of samples written (frames * 16), or 0 on bad arguments.
 */
uint32_t spu2VadpcmDecode(const uint8_t *src, uint32_t frames,
    const int16_t *book, int32_t order, int32_t npredictors,
    int16_t history[2], int16_t *out);

/* Bytes needed to encode sample_count samples (whole blocks). */
uint32_t spu2PsAdpcmEncodedSize(uint32_t sample_count);

/*
 * Encode PCM16 as PS-ADPCM. Non-looping samples end with the END flag on
 * their last block (the voice stops). Looping samples mark the block that
 * contains loop_start with LOOP_START and the last block with END|REPEAT;
 * the loop start is rounded down to its block (28 samples). Returns bytes
 * written.
 */
uint32_t spu2PsAdpcmEncode(const int16_t *pcm, uint32_t sample_count,
    bool loop, uint32_t loop_start, uint8_t *out);

/* Reference decoder for whole blocks (tests, verification). */
uint32_t spu2PsAdpcmDecode(const uint8_t *src, uint32_t blocks,
    int16_t history[2], int16_t *out);

#ifdef __cplusplus
}
#endif

#endif
