#ifndef PD_PS2_SPU2_BANK_H
#define PD_PS2_SPU2_BANK_H

/*
 * PS-ADPCM sample bank for the SPU2 hardware-voice audio path
 * (docs/ps2-spu2-audio.md, stage 2).
 *
 * Written once per ROM by tools/ps2/pd_spu2bank (host) and read by the IOP
 * voice module, which streams samples from it into SPU2 RAM. Every
 * multi-byte field is little-endian.
 *
 *   Spu2BankHeader
 *   Spu2BankEntry[entry_count]   sorted by (source, tbl_base, tbl_len)
 *   sample data                  16-byte aligned PS-ADPCM, each entry's
 *                                blocks carry their own loop flags
 *
 * Entries are keyed by the wavetable's location in its .tbl segment, which
 * is what the game's ALWaveTable holds at run time (base/len), so the EE can
 * find a sample without any extra mapping table.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPU2_BANK_MAGIC 0x42535350u /* "PSSB" */
#define SPU2_BANK_VERSION 1u

enum Spu2BankSource {
    SPU2_BANK_SOURCE_SFX = 0, /* sfx.ctl / sfx.tbl */
    SPU2_BANK_SOURCE_SEQ = 1, /* seq.ctl / seq.tbl (instruments) */
};

#define SPU2_BANK_NO_LOOP 0xffffffffu

typedef struct Spu2BankHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t rom_version;   /* 0 = NTSC final, 1 = PAL final, 2 = JPN final */
    uint32_t entry_count;
    uint32_t data_offset;   /* file offset of the sample data */
    uint32_t data_size;
    uint32_t rom_crc;       /* CRC-32 of the source ctl+tbl segments */
    uint32_t reserved[2];
} Spu2BankHeader;

typedef struct Spu2BankEntry {
    uint16_t source;        /* enum Spu2BankSource */
    uint16_t flags;         /* SPU2_BANK_ENTRY_* */
    uint32_t tbl_base;      /* ALWaveTable.base (offset in the .tbl) */
    uint32_t tbl_len;       /* ALWaveTable.len (bytes of VADPCM/PCM) */
    uint32_t data_offset;   /* relative to Spu2BankHeader.data_offset */
    uint32_t data_size;     /* bytes, multiple of 16 */
    uint32_t loop_block;    /* block index of LOOP_START or SPU2_BANK_NO_LOOP */
    uint32_t lead_samples;  /* silence prepended to block-align the loop */
    uint32_t source_samples;/* samples in the original (decoded) wavetable */
} Spu2BankEntry;

#define SPU2_BANK_ENTRY_LOOPED 0x0001u    /* infinite loop (END|REPEAT) */
#define SPU2_BANK_ENTRY_UNROLLED 0x0002u  /* finite loop expanded inline */
#define SPU2_BANK_ENTRY_RAW16 0x0004u     /* source was raw PCM16 */
#define SPU2_BANK_ENTRY_RESHAPED 0x0008u  /* loop body repeated to fit 28-sample blocks */

/*
 * Binary search by (source, tbl_base); bases are unique within a .tbl. The
 * length is not part of the key: the runtime ALWaveTable.len of some
 * instrument samples is one less than the ROM's (rounded down to whole
 * whole 9-byte VADPCM frames), so it is only a sanity check against the stored length.
 */
static inline const Spu2BankEntry *spu2BankFind(const Spu2BankEntry *entries,
    uint32_t count, uint16_t source, uint32_t tbl_base, uint32_t tbl_len)
{
    uint32_t lo = 0u, hi = count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2u;
        const Spu2BankEntry *e = &entries[mid];
        int cmp;
        if (e->source != source) {
            cmp = e->source < source ? -1 : 1;
        } else if (e->tbl_base != tbl_base) {
            cmp = e->tbl_base < tbl_base ? -1 : 1;
        } else {
            return tbl_len <= e->tbl_len && e->tbl_len - tbl_len <= 1u ? e : 0;
        }
        if (cmp < 0) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif
