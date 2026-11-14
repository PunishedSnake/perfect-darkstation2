/*
 * pd_spu2bank: transcode Perfect Dark's audio sample banks into a PS-ADPCM
 * bank for the PS2 SPU2 hardware-voice path (docs/ps2-spu2-audio.md).
 *
 *   pd_spu2bank <rom.z64> <out.pssb> [--verify]
 *
 * Walks sfx.ctl and seq.ctl in the (big-endian, byte-order .z64) ROM, decodes
 * every referenced wavetable (VADPCM or raw PCM16) and encodes it as
 * PS-ADPCM. Loops are made block-exact for the SPU2:
 *   - silence is prepended so the loop start falls on a 28-sample block;
 *   - the loop body is repeated until its length is a multiple of 28;
 *   - finite loop counts are unrolled into a one-shot sample.
 * The SPU2 then loops with END|REPEAT at exactly the N64's loop points.
 *
 * Build: cc -O2 -Iport/ps2 tools/ps2/pd_spu2bank.c port/ps2/spu2_adpcm.c -lm
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spu2_adpcm.h"
#include "spu2_bank.h"

struct RomLayout {
    const char *name;
    uint32_t sfxctl, sfxctl_size, sfxtbl, sfxtbl_size;
    uint32_t seqctl, seqctl_size, seqtbl, seqtbl_size;
};

/* Offsets from port/src/romdata.c (NTSC final, PAL final, JPN final). */
static const struct RomLayout s_layouts[] = {
    { "ntsc-final", 0x80a250u, 0x2fb80u, 0x839dd0u, 0x4c2160u,
      0xcfbf30u, 0xa060u, 0xd05f90u, 0x17c070u },
    { "pal-final", 0x7f87e0u, 0x2fb80u, 0x828360u, 0x4c2160u,
      0xcea4c0u, 0xa060u, 0xcf4520u, 0x17c070u },
    { "jpn-final", 0x7fc670u, 0x2fb80u, 0x82c1f0u, 0x4c2160u,
      0xcee350u, 0xa060u, 0xcf83b0u, 0x17c070u },
};

static uint8_t *s_rom;
static size_t s_rom_size;

static uint32_t be32(uint32_t off)
{
    if (off + 4 > s_rom_size) {
        fprintf(stderr, "read past ROM end at 0x%x\n", off);
        exit(1);
    }
    const uint8_t *p = s_rom + off;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t be16(uint32_t off)
{
    if (off + 2 > s_rom_size) {
        fprintf(stderr, "read past ROM end at 0x%x\n", off);
        exit(1);
    }
    return (uint16_t)((s_rom[off] << 8) | s_rom[off + 1]);
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; ++k) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

struct Wave {
    uint16_t source;
    uint32_t base, len;
    uint8_t type;
    uint32_t loop_off, book_off; /* ctl-relative, 0 = none */
    uint32_t ctl;                /* ROM offset of the ctl segment */
    uint32_t tbl;
};

static struct Wave *s_waves;
static size_t s_wave_count, s_wave_cap;

static void add_wave(uint16_t source, uint32_t ctl, uint32_t ctl_size,
    uint32_t tbl, uint32_t tbl_size, uint32_t wt)
{
    if (wt == 0 || wt + 20 > ctl_size) {
        return;
    }
    struct Wave w;
    w.source = source;
    w.ctl = ctl;
    w.tbl = tbl;
    w.base = be32(ctl + wt + 0);
    w.len = be32(ctl + wt + 4);
    w.type = s_rom[ctl + wt + 8];
    w.loop_off = be32(ctl + wt + 12);
    w.book_off = w.type == 0 ? be32(ctl + wt + 16) : 0u;
    if (w.len == 0 || w.base + w.len > tbl_size) {
        fprintf(stderr, "warning: wavetable at ctl+0x%x out of range (base 0x%x len 0x%x)\n",
            wt, w.base, w.len);
        return;
    }
    for (size_t i = 0; i < s_wave_count; ++i) {
        if (s_waves[i].source == source && s_waves[i].base == w.base &&
            s_waves[i].len == w.len) {
            return;
        }
    }
    if (s_wave_count == s_wave_cap) {
        s_wave_cap = s_wave_cap ? s_wave_cap * 2 : 256;
        s_waves = realloc(s_waves, s_wave_cap * sizeof(*s_waves));
    }
    s_waves[s_wave_count++] = w;
}

static void walk_instrument(uint16_t source, uint32_t ctl, uint32_t ctl_size,
    uint32_t tbl, uint32_t tbl_size, uint32_t inst)
{
    if (inst == 0 || inst + 0x10 > ctl_size) {
        return;
    }
    const int16_t count = (int16_t)be16(ctl + inst + 0x0e);
    for (int s = 0; s < count; ++s) {
        const uint32_t snd = be32(ctl + inst + 0x10 + 4u * (uint32_t)s);
        if (snd == 0 || snd + 12 > ctl_size) {
            continue;
        }
        add_wave(source, ctl, ctl_size, tbl, tbl_size, be32(ctl + snd + 8));
    }
}

static void walk_bankfile(uint16_t source, uint32_t ctl, uint32_t ctl_size,
    uint32_t tbl, uint32_t tbl_size)
{
    const int16_t banks = (int16_t)be16(ctl + 2);
    for (int b = 0; b < banks; ++b) {
        const uint32_t bank = be32(ctl + 4 + 4u * (uint32_t)b);
        if (bank == 0 || bank + 12 > ctl_size) {
            continue;
        }
        const int16_t insts = (int16_t)be16(ctl + bank);
        walk_instrument(source, ctl, ctl_size, tbl, tbl_size, be32(ctl + bank + 8));
        for (int i = 0; i < insts; ++i) {
            walk_instrument(source, ctl, ctl_size, tbl, tbl_size,
                be32(ctl + bank + 12 + 4u * (uint32_t)i));
        }
    }
}

static int wave_cmp(const void *a, const void *b)
{
    const struct Wave *x = a, *y = b;
    if (x->source != y->source) return x->source < y->source ? -1 : 1;
    if (x->base != y->base) return x->base < y->base ? -1 : 1;
    if (x->len != y->len) return x->len < y->len ? -1 : 1;
    return 0;
}

/* Decode a wavetable to PCM16; returns sample count. */
static uint32_t decode_wave(const struct Wave *w, int16_t **out_pcm)
{
    const uint8_t *src = s_rom + w->tbl + w->base;
    if (w->type == 1) {
        const uint32_t n = w->len / 2u;
        int16_t *pcm = malloc((n + 1) * sizeof(int16_t));
        for (uint32_t i = 0; i < n; ++i) {
            pcm[i] = (int16_t)((src[2 * i] << 8) | src[2 * i + 1]);
        }
        *out_pcm = pcm;
        return n;
    }
    if (w->type != 0 || w->book_off == 0) {
        *out_pcm = NULL;
        return 0;
    }
    const int32_t order = (int32_t)be32(w->ctl + w->book_off);
    const int32_t npred = (int32_t)be32(w->ctl + w->book_off + 4);
    if (order != 2 || npred <= 0 || npred > 8) {
        fprintf(stderr, "warning: unsupported book order %d npred %d\n", order, npred);
        *out_pcm = NULL;
        return 0;
    }
    int16_t book[8 * 16];
    for (int i = 0; i < npred * 16; ++i) {
        book[i] = (int16_t)be16(w->ctl + w->book_off + 8 + 2u * (uint32_t)i);
    }
    const uint32_t frames = w->len / SPU2_VADPCM_FRAME_BYTES;
    int16_t *pcm = malloc((frames * 16u + 1) * sizeof(int16_t));
    int16_t hist[2] = { 0, 0 };
    *out_pcm = pcm;
    return spu2VadpcmDecode(src, frames, book, order, npred, hist, pcm);
}

static uint32_t gcd32(uint32_t a, uint32_t b)
{
    while (b) {
        const uint32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

struct Built {
    int16_t *pcm;
    uint32_t count;
    bool loop;
    uint32_t loop_start;
    uint32_t lead;
    uint16_t flags;
};

static void push(struct Built *b, uint32_t *cap, const int16_t *src, uint32_t n)
{
    if (b->count + n > *cap) {
        while (b->count + n > *cap) {
            *cap = *cap ? *cap * 2 : 4096;
        }
        b->pcm = realloc(b->pcm, *cap * sizeof(int16_t));
    }
    memcpy(b->pcm + b->count, src, n * sizeof(int16_t));
    b->count += n;
}

/* Max repetitions of a loop body used to reach a whole number of blocks. */
#define MAX_LOOP_BYTES (64u * 1024u)

static void build_sample(const struct Wave *w, const int16_t *pcm, uint32_t n,
    struct Built *out)
{
    uint32_t cap = 0;
    memset(out, 0, sizeof(*out));
    uint32_t start = 0, end = 0, count = 0;
    if (w->loop_off != 0) {
        start = be32(w->ctl + w->loop_off + 0);
        end = be32(w->ctl + w->loop_off + 4);
        count = be32(w->ctl + w->loop_off + 8);
    }
    if (w->type == 1) {
        out->flags |= SPU2_BANK_ENTRY_RAW16;
    }
    if (w->loop_off == 0 || count == 0 || end <= start || end > n) {
        push(out, &cap, pcm, n);
        return;
    }
    const uint32_t body = end - start;
    if (count != 0xffffffffu) {
        /* Finite loop: play [0,end), repeat [start,end) count times, then
         * the tail [end,n). */
        push(out, &cap, pcm, end);
        for (uint32_t i = 0; i < count && i < 64u; ++i) {
            push(out, &cap, pcm + start, body);
        }
        push(out, &cap, pcm + end, n - end);
        out->flags |= SPU2_BANK_ENTRY_UNROLLED;
        return;
    }
    /* Infinite loop: block-align the start, then whole blocks of body. */
    const uint32_t lead = (SPU2_PSADPCM_BLOCK_SAMPLES -
        start % SPU2_PSADPCM_BLOCK_SAMPLES) % SPU2_PSADPCM_BLOCK_SAMPLES;
    uint32_t reps = SPU2_PSADPCM_BLOCK_SAMPLES / gcd32(body, SPU2_PSADPCM_BLOCK_SAMPLES);
    if (reps > 1u && (uint64_t)body * reps * 16u / 28u > MAX_LOOP_BYTES) {
        reps = 1u; /* long body: accept < 1 block of loop-length error */
    }
    if (reps > 1u) {
        out->flags |= SPU2_BANK_ENTRY_RESHAPED;
    }
    const int16_t zero[SPU2_PSADPCM_BLOCK_SAMPLES] = { 0 };
    push(out, &cap, zero, lead);
    push(out, &cap, pcm, end);
    for (uint32_t i = 1; i < reps; ++i) {
        push(out, &cap, pcm + start, body);
    }
    /* Pad the final block from the loop's own start so the wrap is smooth. */
    uint32_t rem = out->count % SPU2_PSADPCM_BLOCK_SAMPLES;
    uint32_t k = 0;
    while (rem != 0) {
        push(out, &cap, pcm + start + (k % body), 1);
        ++k;
        rem = out->count % SPU2_PSADPCM_BLOCK_SAMPLES;
    }
    out->loop = true;
    out->loop_start = lead + start;
    out->lead = lead;
    out->flags |= SPU2_BANK_ENTRY_LOOPED;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <rom.z64> <out.pssb> [--verify]\n", argv[0]);
        return 2;
    }
    const bool verify = argc > 3 && strcmp(argv[3], "--verify") == 0;
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    s_rom_size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    s_rom = malloc(s_rom_size);
    if (fread(s_rom, 1, s_rom_size, f) != s_rom_size) {
        fprintf(stderr, "short read\n");
        return 1;
    }
    fclose(f);
    if (s_rom_size < 0x40 || be32(0) != 0x80371240u) {
        fprintf(stderr, "%s: not a big-endian .z64 ROM\n", argv[1]);
        return 1;
    }

    /* Identify the version by the bank file headers (revision 'B1'). */
    int ver = -1;
    for (int v = 0; v < 3; ++v) {
        const struct RomLayout *l = &s_layouts[v];
        if (l->seqtbl + l->seqtbl_size <= s_rom_size &&
            be16(l->sfxctl) == 0x4231u && be16(l->seqctl) == 0x4231u) {
            ver = v;
            break;
        }
    }
    if (ver < 0) {
        fprintf(stderr, "unrecognised ROM (no B1 bank files at known offsets)\n");
        return 1;
    }
    const struct RomLayout *l = &s_layouts[ver];
    walk_bankfile(SPU2_BANK_SOURCE_SFX, l->sfxctl, l->sfxctl_size, l->sfxtbl, l->sfxtbl_size);
    walk_bankfile(SPU2_BANK_SOURCE_SEQ, l->seqctl, l->seqctl_size, l->seqtbl, l->seqtbl_size);
    qsort(s_waves, s_wave_count, sizeof(*s_waves), wave_cmp);

    uint32_t crc = 0;
    crc = crc32_update(crc, s_rom + l->sfxctl, l->sfxctl_size);
    crc = crc32_update(crc, s_rom + l->sfxtbl, l->sfxtbl_size);
    crc = crc32_update(crc, s_rom + l->seqctl, l->seqctl_size);
    crc = crc32_update(crc, s_rom + l->seqtbl, l->seqtbl_size);

    const size_t header_size = 32u;
    const size_t entry_size = 32u;
    size_t data_off = header_size + entry_size * s_wave_count;
    data_off = (data_off + 15u) & ~(size_t)15u;
    uint8_t *entries = calloc(s_wave_count, entry_size);
    size_t data_cap = 1u << 20, data_len = 0;
    uint8_t *data = malloc(data_cap);
    uint32_t looped = 0, unrolled = 0, reshaped = 0, raw = 0, skipped = 0;
    double worst_snr = 1e9;
    uint64_t pcm_samples = 0;

    for (size_t i = 0; i < s_wave_count; ++i) {
        const struct Wave *w = &s_waves[i];
        int16_t *pcm = NULL;
        const uint32_t n = decode_wave(w, &pcm);
        uint8_t *e = entries + i * entry_size;
        put16(e + 0, w->source);
        put32(e + 4, w->base);
        put32(e + 8, w->len);
        put32(e + 20, SPU2_BANK_NO_LOOP);
        put32(e + 28, n);
        if (n == 0) {
            ++skipped;
            free(pcm);
            continue;
        }
        struct Built b;
        build_sample(w, pcm, n, &b);
        const uint32_t bytes = spu2PsAdpcmEncodedSize(b.count);
        while (data_len + bytes > data_cap) {
            data_cap *= 2;
            data = realloc(data, data_cap);
        }
        const uint32_t written = spu2PsAdpcmEncode(b.pcm, b.count, b.loop,
            b.loop_start, data + data_len);
        if (verify) {
            const uint32_t blocks = written / SPU2_PSADPCM_BLOCK_BYTES;
            int16_t *dec = malloc(blocks * SPU2_PSADPCM_BLOCK_SAMPLES * sizeof(int16_t));
            int16_t hist[2] = { 0, 0 };
            spu2PsAdpcmDecode(data + data_len, blocks, hist, dec);
            double sig = 0, err = 0;
            for (uint32_t s = 0; s < b.count; ++s) {
                const double d = (double)dec[s] - b.pcm[s];
                sig += (double)b.pcm[s] * b.pcm[s];
                err += d * d;
            }
            if (sig > 1e6 * b.count / 1000.0 && err > 0) {
                const double snr = 10.0 * log10(sig / err);
                if (snr < worst_snr) worst_snr = snr;
            }
            free(dec);
        }
        put16(e + 2, b.flags);
        put32(e + 12, (uint32_t)data_len);
        put32(e + 16, written);
        if (b.loop) {
            put32(e + 20, b.loop_start / SPU2_PSADPCM_BLOCK_SAMPLES);
        }
        put32(e + 24, b.lead);
        data_len += written;
        pcm_samples += b.count;
        looped += (b.flags & SPU2_BANK_ENTRY_LOOPED) != 0;
        unrolled += (b.flags & SPU2_BANK_ENTRY_UNROLLED) != 0;
        reshaped += (b.flags & SPU2_BANK_ENTRY_RESHAPED) != 0;
        raw += (b.flags & SPU2_BANK_ENTRY_RAW16) != 0;
        free(b.pcm);
        free(pcm);
    }

    uint8_t header[32] = { 0 };
    put32(header + 0, SPU2_BANK_MAGIC);
    put16(header + 4, SPU2_BANK_VERSION);
    put16(header + 6, (uint16_t)ver);
    put32(header + 8, (uint32_t)s_wave_count);
    put32(header + 12, (uint32_t)data_off);
    put32(header + 16, (uint32_t)data_len);
    put32(header + 20, crc);

    FILE *o = fopen(argv[2], "wb");
    if (!o) {
        perror(argv[2]);
        return 1;
    }
    static const uint8_t pad[16];
    fwrite(header, 1, sizeof(header), o);
    fwrite(entries, entry_size, s_wave_count, o);
    fwrite(pad, 1, data_off - header_size - entry_size * s_wave_count, o);
    fwrite(data, 1, data_len, o);
    fclose(o);

    printf("%s: %zu samples (%u looped, %u reshaped, %u unrolled, %u raw16, %u skipped)\n",
        l->name, s_wave_count, looped, reshaped, unrolled, raw, skipped);
    printf("PS-ADPCM data %.2f MiB, %.1f s of audio at 22.05 kHz, crc %08x\n",
        data_len / 1048576.0, pcm_samples / 22050.0, crc);
    if (verify) {
        printf("worst round-trip SNR %.1f dB\n", worst_snr);
    }
    free(entries);
    free(data);
    free(s_rom);
    free(s_waves);
    return 0;
}
