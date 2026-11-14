/*
 * EE side of the SPU2 hardware-voice path (docs/ps2-spu2-audio.md, stage 4).
 *
 * When the PS-ADPCM bank (pd.pssb, written by tools/ps2/pd_spu2bank) sits
 * next to the ROM and pdsnd.irx starts, the n_audio synthesiser stops
 * building RSP-style command lists for its physical voices. Instead the
 * envelope mixer's parameter events (start, volume/pan segments, pitch,
 * stop) are translated into pdsnd voice commands, batched per audio frame
 * and sent to the IOP with one non-blocking SIF RPC. Decoding, resampling,
 * enveloping and mixing of those voices then cost the EE nothing.
 *
 * Without the bank (or with Audio.HardwareVoices=0) nothing changes: the
 * software mixer stays the reference path.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <kernel.h>
#include <delaythread.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <sbv_patches.h>

#include "pdsnd_rpc.h"
#include "spu2_bank.h"
#include "spu2_voices_ps2.h"

#include "fs.h"
#include "system.h"
#include "config.h"
#include "platform.h"
#include "log_ps2.h"

extern unsigned char pdsnd_irx[] __attribute__((aligned(16)));
extern unsigned int size_pdsnd_irx;

extern u8 *_sfxtblSegmentRomStart, *_sfxtblSegmentRomEnd;
extern u8 *_seqtblSegmentRomStart, *_seqtblSegmentRomEnd;

#define PS2_HW_BANK_NAME "pd.pssb"
/* SPU2 RAM window for the sample cache. Below: audsrv's sound-data input
 * areas and its ADPCM region start (0x5010); above: core reverb work
 * areas: core 0's echo ends at 0x1dffff, core 1's reverb at 0x1fffff. */
#define PS2_HW_SPU_BASE 0x00010000u
#define PS2_HW_SPU_END 0x001c0000u

bool g_Ps2HwVoices;

static s32 s_enabled_cfg = 1;
/* SPU2 effects for hardware voices: 0 off, 1 follow the game's FX buses.
 * Depth is a percentage applied to the derived effect return level. */
static s32 s_reverb_mode = 1;
static s32 s_reverb_depth = 100;
static SifRpcClientData_t s_client __attribute__((aligned(64)));
static PdSndFrameArgs s_frame[2] __attribute__((aligned(64)));
static u32 s_frame_index;
static PdSndStatus s_status __attribute__((aligned(64)));
static Spu2BankEntry *s_entries;
static u32 s_entry_count;
static u32 s_frames_sent;
static u32 s_overflow;
static u32 s_misses;
static u32 s_output_rate = 22020;

static PdSndFrameArgs *cur_frame(void)
{
    return &s_frame[s_frame_index & 1u];
}

PD_CONSTRUCTOR static void ps2HwVoicesConfigInit(void)
{
    configRegisterInt("Audio.HardwareVoices", &s_enabled_cfg, 0, 1);
    configRegisterInt("Audio.HardwareReverb", &s_reverb_mode, 0, 1);
    configRegisterInt("Audio.HardwareReverbDepth", &s_reverb_depth, 0, 200);
}

static bool load_bank_index(void)
{
    FILE *f = fsFileOpenRead(PS2_HW_BANK_NAME);
    if (!f) {
        return false;
    }
    Spu2BankHeader h;
    bool ok = fread(&h, sizeof(h), 1, f) == 1 && h.magic == SPU2_BANK_MAGIC &&
        h.version == SPU2_BANK_VERSION && h.entry_count > 0 && h.entry_count < 0xffffu;
    if (ok) {
        s_entries = sysMemAlloc(h.entry_count * sizeof(Spu2BankEntry));
        ok = s_entries && fread(s_entries, sizeof(Spu2BankEntry), h.entry_count, f) == h.entry_count;
        s_entry_count = ok ? h.entry_count : 0;
    }
    fclose(f);
    if (!ok) {
        sysLogPrintf(LOG_WARNING, "SPU2 voices: %s is not a valid bank", PS2_HW_BANK_NAME);
    }
    return ok;
}

static bool bind_server(void)
{
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (SifBindRpc(&s_client, PDSND_RPC_ID, 0) < 0) {
            return false;
        }
        if (s_client.server != NULL) {
            return true;
        }
        nopdelay();
        DelayThread(1000);
    }
    return false;
}

void ps2HwVoicesInit(s32 output_rate)
{
    g_Ps2HwVoices = false;
    s_output_rate = output_rate > 0 ? (u32)output_rate : 22020u;
    if (!s_enabled_cfg || !load_bank_index()) {
        sysLogPrintf(LOG_NOTE, "SPU2 voices: off (%s)",
            s_enabled_cfg ? "no " PS2_HW_BANK_NAME : "disabled in config");
        return;
    }

    sbv_patch_enable_lmb();
    int module_result = 0;
    const int id = SifExecModuleBuffer(pdsnd_irx, size_pdsnd_irx, 0, NULL, &module_result);
    if (id < 0 || module_result == 1 /* NO_RESIDENT_END */ || !bind_server()) {
        sysLogPrintf(LOG_ERROR, "SPU2 voices: pdsnd.irx failed (id=%d result=%d)", id, module_result);
        return;
    }

    static PdSndInitArgs args __attribute__((aligned(64)));
    memset(&args, 0, sizeof(args));
    args.protocol = PDSND_PROTOCOL;
    args.spu_base = PS2_HW_SPU_BASE;
    args.spu_end = PS2_HW_SPU_END;
    strncpy(args.path, fsFullPath(PS2_HW_BANK_NAME), sizeof(args.path) - 1);
    if (SifCallRpc(&s_client, PDSND_FUNC_INIT, 0, &args, sizeof(args), &args, sizeof(args), NULL, NULL) < 0) {
        sysLogPrintf(LOG_ERROR, "SPU2 voices: INIT RPC failed");
        return;
    }
    const PdSndInitResult *r = (const PdSndInitResult *)&args;
    if (r->result != 0) {
        sysLogPrintf(LOG_ERROR, "SPU2 voices: IOP could not open %s (%d)", args.path, (int)r->result);
        return;
    }
    s_frame[0].count = s_frame[1].count = 0;
    g_Ps2HwVoices = true;
    sysLogPrintf(LOG_NOTE, "SPU2 voices: on, %u samples from %s (rom crc %08x), output %u Hz",
        (unsigned)r->entries, PS2_HW_BANK_NAME, (unsigned)r->rom_crc, (unsigned)s_output_rate);
}

s32 ps2HwVoicesEntryFor(const u8 *base, s32 len)
{
    const uintptr_t b = (uintptr_t)base;
    u16 source;
    u32 offset;
    if (b >= (uintptr_t)_sfxtblSegmentRomStart && b < (uintptr_t)_sfxtblSegmentRomEnd) {
        source = SPU2_BANK_SOURCE_SFX;
        offset = (u32)(b - (uintptr_t)_sfxtblSegmentRomStart);
    } else if (b >= (uintptr_t)_seqtblSegmentRomStart && b < (uintptr_t)_seqtblSegmentRomEnd) {
        source = SPU2_BANK_SOURCE_SEQ;
        offset = (u32)(b - (uintptr_t)_seqtblSegmentRomStart);
    } else {
        if (s_misses++ < 8) {
            sysLogPrintf(LOG_NOTE, "SPU2 voices: miss base=%p len=%d sfx=%p..%p seq=%p..%p", base, (int)len,
                _sfxtblSegmentRomStart, _sfxtblSegmentRomEnd, _seqtblSegmentRomStart, _seqtblSegmentRomEnd);
        }
        return -1;
    }
    const Spu2BankEntry *e = spu2BankFind(s_entries, s_entry_count, source, offset, (u32)len);
    if (!e || e->data_size == 0) {
        if (s_misses++ < 8) {
            sysLogPrintf(LOG_NOTE, "SPU2 voices: no entry src=%u off=%x len=%d", source, (unsigned)offset, (int)len);
        }
        return -1;
    }
    return (s32)(e - s_entries);
}

s32 ps2HwVoicesMp3Entry(s32 filenum, u32 *rate, u32 *samples)
{
    if (!g_Ps2HwVoices || filenum <= 0) {
        return -1;
    }
    /* The key is the file number; the stored length (the file's size) is
     * not known to the caller, so search with it taken from the match. */
    u32 lo = 0, hi = s_entry_count;
    while (lo < hi) {
        const u32 mid = lo + (hi - lo) / 2u;
        const Spu2BankEntry *e = &s_entries[mid];
        if (e->source < SPU2_BANK_SOURCE_MP3 ||
            (e->source == SPU2_BANK_SOURCE_MP3 && e->tbl_base < (u32)filenum)) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    if (lo >= s_entry_count) {
        return -1;
    }
    const Spu2BankEntry *e = &s_entries[lo];
    if (e->source != SPU2_BANK_SOURCE_MP3 || e->tbl_base != (u32)filenum ||
        e->data_size == 0 || e->lead_samples == 0) {
        return -1;
    }
    *rate = e->lead_samples;
    *samples = e->source_samples;
    return (s32)lo;
}

u16 ps2HwVoicesPitch(f32 ratio)
{
    /* N64 ratio = source samples per output sample at the synth's output
     * rate; SPU2 PITCH 0x1000 plays 48000 source samples per second. */
    f32 p = ratio * (f32)s_output_rate * (4096.0f / 48000.0f);
    if (p < 1.0f) {
        p = 1.0f;
    }
    if (p > 16383.0f) {
        p = 16383.0f;
    }
    return (u16)(p + 0.5f);
}

void ps2HwVoicesPush(const PdSndVoiceCmd *cmd)
{
    PdSndFrameArgs *f = cur_frame();
    if (f->count >= PDSND_MAX_CMDS) {
        ++s_overflow;
        return;
    }
    f->cmds[f->count++] = *cmd;
}

/*
 * The game's two FX buses are delay networks: bus 0 a multi-tap reverb
 * (room character set by the level's sequences), bus 1 a single echo.
 * Map bus 0 onto the SPU2 reverb preset whose size matches its longest
 * tap, and bus 1 onto the SPU2 echo with the tap's delay and feedback.
 */
void ps2HwVoicesSetFx(s32 bus, s32 delay_ms, s32 gain, s32 feedback)
{
    if (!g_Ps2HwVoices || bus < 0 || bus > 1) {
        return;
    }
    int mode = 0, delay = 0, fb = 0;
    s32 depth = 0;
    if (s_reverb_mode != 0 && gain > 0 && delay_ms > 0) {
        if (bus == 0) {
            mode = delay_ms < 40 ? 1 /* room */
                : delay_ms < 80 ? 2 /* studio small */
                : delay_ms < 140 ? 3 /* studio medium */
                : delay_ms < 220 ? 4 /* studio large */
                : delay_ms < 400 ? 5 /* hall */
                : 6; /* space */
            depth = gain * 6 / 10;
        } else {
            mode = 7; /* echo */
            delay = delay_ms * 127 / 1000;
            delay = delay < 1 ? 1 : delay > 127 ? 127 : delay;
            fb = feedback * 127 / 0x7fff;
            depth = gain / 2;
        }
        depth = depth * s_reverb_depth / 100;
        depth = depth < 0x400 ? 0x400 : depth > 0x6000 ? 0x6000 : depth;
    }
    static s32 last[2][4] = { { -1, -1, -1, -1 }, { -1, -1, -1, -1 } };
    if (last[bus][0] == mode && last[bus][1] == depth && last[bus][2] == delay && last[bus][3] == fb) {
        return;
    }
    last[bus][0] = mode;
    last[bus][1] = depth;
    last[bus][2] = delay;
    last[bus][3] = fb;
    PdSndVoiceCmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.op = PDSND_OP_FX;
    cmd.voice = bus == 0 ? 1 : 0; /* core */
    cmd.entry = (u16)mode;
    cmd.pitch = (u16)depth;
    cmd.cur_l = (s16)delay;
    cmd.cur_r = (s16)fb;
    ps2HwVoicesPush(&cmd);
    sysLogPrintf(LOG_NOTE, "SPU2 voices: FX bus %d -> mode %d depth %04x delay %d feedback %d (taps %d ms, gain %d)",
        (int)bus, mode, (unsigned)depth, delay, fb, (int)delay_ms, (int)gain);
}

void ps2HwVoicesFlush(void)
{
    if (!g_Ps2HwVoices) {
        return;
    }
    PdSndFrameArgs *f = cur_frame();
    /* One call in flight at a time; the IOP handler only copies, so this
     * wait is normally already over. */
    while (SifCheckStatRpc(&s_client)) {
        nopdelay();
    }
    f->frame = ++s_frames_sent;
    const u32 bytes = 16u + f->count * (u32)sizeof(PdSndVoiceCmd);
    SifCallRpc(&s_client, PDSND_FUNC_FRAME, SIF_RPC_M_NOWAIT, f, (int)((bytes + 15u) & ~15u),
        NULL, 0, NULL, NULL);
    ++s_frame_index;
    cur_frame()->count = 0;

    if ((s_frames_sent % 1200u) == 0u) {
        while (SifCheckStatRpc(&s_client)) {
            nopdelay();
        }
        SifCallRpc(&s_client, PDSND_FUNC_STATUS, 0, NULL, 0, &s_status, sizeof(s_status), NULL, NULL);
        sysLogPrintf(LOG_NOTE,
            "SPU2 voices: frames=%u loads=%u (%u KiB, %u ms) evict=%u fail=%u cache=%u KiB qdrop=%u overflow=%u miss=%u fx=%u streams=%u",
            (unsigned)s_status.frames, (unsigned)s_status.loads,
            (unsigned)(s_status.load_bytes >> 10), (unsigned)(s_status.load_us / 1000u),
            (unsigned)s_status.evictions, (unsigned)s_status.failures,
            (unsigned)(s_status.cache_used >> 10), (unsigned)s_status.queue_drops,
            (unsigned)s_overflow, (unsigned)s_misses, (unsigned)s_status.fx_changes,
            (unsigned)s_status.streams);
    }
}
