/*
 * pdsnd.irx - Perfect Dark's SPU2 hardware-voice module.
 *
 * Plays the game's synthesiser voices on SPU2 voices instead of mixing them
 * on the EE (docs/ps2-spu2-audio.md). Samples come from the PS-ADPCM bank
 * written by tools/ps2/pd_spu2bank, are streamed by this module from the
 * storage device into an LRU cache in SPU2 RAM, and are never touched by
 * the EE.
 *
 *  - RPC thread: accepts INIT / FRAME / STATUS calls and queues FRAME
 *    batches; it never blocks on IO, so the EE's non-blocking FRAME call
 *    returns at once.
 *  - Worker thread: applies queued batches in order, streaming a sample on
 *    a cache miss before its KEYON. Long one-shot samples (the speech
 *    clips) are keyed on after their first STREAM_HEAD bytes; the rest
 *    follows one IO_CHUNK per applied batch, far ahead of the voice.
 *  - Ramp thread: runs the N64 envelope segments (linear volume ramps) in
 *    1 ms steps.
 *
 * Coexists with audsrv, which keeps the PCM stream (speech, mixed residue)
 * on core 1's sound-data input. Voices 0..23 are core 1, which also runs the
 * SPU2 reverb for voices with an FX send; 24..47 are core 0, routed into
 * core 1 through its external input. audsrv silences core 0's master volume
 * whenever it changes state, so each FRAME restores it.
 */

#include "irx_imports.h"
#include "pdsnd_rpc.h"
#include "spu2_bank.h"

#define MODNAME "pdsnd"
#ifndef SD_CORE_0
#define SD_CORE_0 0
#define SD_CORE_1 1
#endif
IRX_ID(MODNAME, 1, 1);

#define IO_CHUNK (32 * 1024)
/* One-shot samples longer than this start playing after this much is
 * resident (about 2.5 s of 22 kHz PS-ADPCM). */
#define STREAM_HEAD (32 * 1024)
#define QUEUE_FRAMES 8
#define MAX_SLOTS 384
#define NO_ENTRY 0xffffu
#define RAMP_STEP_US 1000u
/* Voices keep their sample pinned for this many FRAME calls after KEYOFF,
 * covering the hardware release. */
#define RELEASE_FRAMES 3u

/* ------------------------------------------------------------------ */

static SifRpcDataQueue_t s_qd;
static SifRpcServerData_t s_sd;
static u8 s_rpc_buffer[sizeof(PdSndFrameArgs)] __attribute__((aligned(64)));

static int s_lock;      /* voice state, cache and SPU2 register access */
static int s_work_sema; /* counts queued frames */

static PdSndFrameArgs s_queue[QUEUE_FRAMES];
static volatile u32 s_q_head, s_q_tail;

static PdSndStatus s_status;
static PdSndInitResult s_init_result;

static int s_fd = -1;
static Spu2BankHeader s_header;
static Spu2BankEntry *s_entries;
static u8 *s_io_buf;

/* ---- SPU2 RAM cache: live slots sorted by address ------------------- */

struct Slot {
    u32 addr; /* SPU2 byte address */
    u32 size;
    u16 entry;
    u16 refs; /* voices currently playing it (+1 while still streaming) */
    u32 stamp;
    u32 loaded; /* bytes resident; < size while the tail is streaming */
};

static struct Slot s_slots[MAX_SLOTS];
static u32 s_slot_count;
static u32 s_spu_base, s_spu_end;
static u32 s_clock;

/* ---- voices -------------------------------------------------------- */

struct Voice {
    u16 entry;       /* pinned entry or NO_ENTRY */
    u16 release;     /* FRAME calls left before unpinning */
    s32 cur_l, cur_r; /* 16.16 */
    s32 step_l, step_r;
    s32 tgt_l, tgt_r;
    u32 steps_left;
};

static struct Voice s_voices[PDSND_MAX_VOICES];
/*
 * Hardware voices: 0..23 are core 1, whose effect unit carries the game's
 * FX bus 0 (the reverb); 24..47 are core 0, carrying FX bus 1 (the echo).
 * The game's voice numbers are mapped onto them at KEYON, on the core of
 * the voice's FX bus, so its send reaches the right effect.
 */
static inline int voice_core(int v) { return v < 24 ? 1 : 0; }
static inline int voice_index(int v) { return v % 24; }
static inline int voice_reg(int v) { return SD_VOICE(voice_core(v), voice_index(v)); }

static s8 s_map[PDSND_MAX_VOICES];   /* game voice -> hardware voice, -1 */
static s8 s_owner[PDSND_MAX_VOICES]; /* hardware voice -> game voice, -1 */
static u8 s_keyed[PDSND_MAX_VOICES]; /* hardware voice playing (not released) */
static u32 s_wet_mask[2];            /* per core: voices sending to its effect */

static void voice_set_wet(int v, int wet)
{
    const int core = voice_core(v);
    if (wet) {
        s_wet_mask[core] |= 1u << voice_index(v);
    } else {
        s_wet_mask[core] &= ~(1u << voice_index(v));
    }
}

/* Hardware voice for game voice g on its bus's core; may take a voice the
 * other core's range when its own is exhausted. */
static int voice_assign(int g, int bus)
{
    const int prefer = bus == 1 ? 24 : 0;
    const int cur = s_map[g];
    if (cur >= 0 && (cur >= 24) == (prefer == 24)) {
        return cur;
    }
    int pick = -1;
    for (int pass = 0; pass < 4 && pick < 0; ++pass) {
        const int base = (pass & 1) ? 24 - prefer : prefer;
        for (int h = base; h < base + 24; ++h) {
            if (pass < 2 ? s_owner[h] < 0 : (!s_keyed[h] && s_owner[h] != g)) {
                pick = h;
                break;
            }
        }
    }
    if (pick < 0) {
        return cur;
    }
    if (s_owner[pick] >= 0) {
        s_map[(int)s_owner[pick]] = -1;
    }
    if (cur >= 0) {
        s_owner[cur] = -1;
    }
    s_owner[pick] = (s8)g;
    s_map[g] = (s8)pick;
    return pick;
}


static void lock(void) { WaitSema(s_lock); }
static void unlock(void) { SignalSema(s_lock); }

static u32 now_us(void)
{
    iop_sys_clock_t clk;
    u32 sec, usec;
    GetSystemTime(&clk);
    SysClock2USec(&clk, &sec, &usec);
    return sec * 1000000u + usec;
}

/* ------------------------------------------------------------------ */

static int slot_find(u16 entry)
{
    for (u32 i = 0; i < s_slot_count; ++i) {
        if (s_slots[i].entry == entry) {
            return (int)i;
        }
    }
    return -1;
}

static void slot_erase(u32 i)
{
    s_status.cache_used -= s_slots[i].size;
    for (u32 k = i + 1; k < s_slot_count; ++k) {
        s_slots[k - 1] = s_slots[k];
    }
    --s_slot_count;
}

/* First fit between resident slots; evict LRU unpinned slots until it fits. */
static int slot_alloc(u16 entry, u32 size)
{
    for (;;) {
        if (s_slot_count < MAX_SLOTS) {
            u32 prev_end = s_spu_base;
            for (u32 i = 0; i <= s_slot_count; ++i) {
                const u32 next = i < s_slot_count ? s_slots[i].addr : s_spu_end;
                if (next >= prev_end && next - prev_end >= size) {
                    for (u32 k = s_slot_count; k > i; --k) {
                        s_slots[k] = s_slots[k - 1];
                    }
                    s_slots[i].addr = prev_end;
                    s_slots[i].size = size;
                    s_slots[i].entry = entry;
                    s_slots[i].refs = 0;
                    s_slots[i].stamp = ++s_clock;
                    s_slots[i].loaded = 0;
                    ++s_slot_count;
                    s_status.cache_used += size;
                    return (int)i;
                }
                if (i < s_slot_count) {
                    prev_end = s_slots[i].addr + s_slots[i].size;
                }
            }
        }
        int victim = -1;
        u32 oldest = 0xffffffffu;
        for (u32 i = 0; i < s_slot_count; ++i) {
            if (s_slots[i].refs == 0 && s_slots[i].stamp < oldest) {
                oldest = s_slots[i].stamp;
                victim = (int)i;
            }
        }
        if (victim < 0) {
            return -1;
        }
        slot_erase((u32)victim);
        ++s_status.evictions;
    }
}

static void spu_upload(u32 addr, const u8 *src, u32 size)
{
    FlushDcache();
    sceSdVoiceTrans(0, SD_TRANS_WRITE | SD_TRANS_MODE_DMA, (u8 *)src, (u32 *)addr, size);
    sceSdVoiceTransStatus(0, 1);
}

/*
 * Read [from, from + size) of an entry's data into its slot. Called with the
 * lock held; drops it around the file IO so the ramp thread keeps running.
 * The caller pins the slot so it cannot be evicted meanwhile.
 */
static int load_range(u16 entry, u32 from, u32 size)
{
    const Spu2BankEntry *e = &s_entries[entry];
    const int i = slot_find(entry);
    if (i < 0) {
        return 0;
    }
    const u32 addr = s_slots[i].addr + from;
    unlock();
    const u32 t0 = now_us();
    int ok = iomanX_lseek(s_fd, (int)(s_header.data_offset + e->data_offset + from), FIO_SEEK_SET) >= 0;
    u32 done = 0;
    while (ok && done < size) {
        u32 n = size - done;
        if (n > IO_CHUNK) {
            n = IO_CHUNK;
        }
        ok = iomanX_read(s_fd, s_io_buf, (int)n) == (int)n;
        if (ok) {
            spu_upload(addr + done, s_io_buf, n);
            done += n;
        }
    }
    const u32 dt = now_us() - t0;
    lock();
    s_status.load_us += dt;
    if (ok) {
        s_status.load_bytes += size;
    }
    return ok;
}

/*
 * Make an entry resident. Called by the worker with the lock dropped around
 * the file IO, so the ramp thread keeps running; the slot is allocated (and
 * pinned with a reference) before IO so it cannot be evicted meanwhile.
 */
static int ensure_resident(u16 entry)
{
    int i = slot_find(entry);
    if (i >= 0) {
        s_slots[i].stamp = ++s_clock;
        return i;
    }
    const Spu2BankEntry *e = &s_entries[entry];
    if (e->data_size == 0 || e->data_size > s_spu_end - s_spu_base) {
        return -1;
    }
    i = slot_alloc(entry, e->data_size);
    if (i < 0) {
        return -1;
    }
    /* Long one-shots: only the head now, the tail from stream_step(). */
    const u32 want = e->loop_block == SPU2_BANK_NO_LOOP && e->data_size > 2u * STREAM_HEAD
        ? STREAM_HEAD : e->data_size;
    s_slots[i].refs++; /* pin during IO */
    const int ok = load_range(entry, 0, want);
    i = slot_find(entry);
    if (i < 0) {
        return -1;
    }
    s_slots[i].refs--;
    if (!ok) {
        slot_erase((u32)i);
        return -1;
    }
    s_slots[i].loaded = want;
    if (want < e->data_size) {
        s_slots[i].refs++; /* the stream's pin, dropped once complete */
        ++s_status.streams;
    } else {
        ++s_status.loads;
    }
    return i;
}

/*
 * Continue one streaming slot by one chunk. A voice reads PS-ADPCM at
 * 22050 * 16 / 28 = 12.6 KB/s; one 32 KiB chunk per audio frame keeps the
 * resident part seconds ahead of it.
 */
static void stream_step(void)
{
    for (u32 i = 0; i < s_slot_count; ++i) {
        if (s_slots[i].loaded >= s_slots[i].size) {
            continue;
        }
        const u16 entry = s_slots[i].entry;
        const u32 from = s_slots[i].loaded;
        u32 n = s_slots[i].size - from;
        if (n > IO_CHUNK) {
            n = IO_CHUNK;
        }
        const int ok = load_range(entry, from, n);
        const int k = slot_find(entry);
        if (k < 0) {
            return;
        }
        if (!ok) {
            /* Give up on the tail: the voice ends at the stale data's first
             * END flag at worst. Mark it complete so the slot can go. */
            ++s_status.failures;
            n = s_slots[k].size - from;
        }
        s_slots[k].loaded = from + n;
        if (s_slots[k].loaded >= s_slots[k].size) {
            s_slots[k].refs--;
            ++s_status.loads;
        }
        return;
    }
}

static void voice_unpin(struct Voice *vs)
{
    if (vs->entry != NO_ENTRY) {
        const int i = slot_find(vs->entry);
        if (i >= 0 && s_slots[i].refs > 0) {
            s_slots[i].refs--;
        }
        vs->entry = NO_ENTRY;
    }
    vs->release = 0;
}

static void voice_set_volume(int v, s32 l, s32 r)
{
    sceSdSetParam(voice_reg(v) | SD_VPARAM_VOLL, (u16)(l >> 16));
    sceSdSetParam(voice_reg(v) | SD_VPARAM_VOLR, (u16)(r >> 16));
}

static void voice_start_ramp(struct Voice *vs, s32 tgt_l, s32 tgt_r, u32 ramp_us)
{
    vs->tgt_l = tgt_l << 16;
    vs->tgt_r = tgt_r << 16;
    vs->steps_left = ramp_us / RAMP_STEP_US;
    if (vs->steps_left == 0) {
        vs->cur_l = vs->tgt_l;
        vs->cur_r = vs->tgt_r;
        vs->step_l = vs->step_r = 0;
    } else {
        vs->step_l = (vs->tgt_l - vs->cur_l) / (s32)vs->steps_left;
        vs->step_r = (vs->tgt_r - vs->cur_r) / (s32)vs->steps_left;
    }
}

static void apply_fx(int core, int mode, int depth, int delay, int feedback);

static void apply_frame(const PdSndFrameArgs *f)
{
    u32 kon[2] = { 0, 0 };
    u32 koff[2] = { 0, 0 };

    lock();
    const u32 wet_before[2] = { s_wet_mask[0], s_wet_mask[1] };
    /* Unpin voices whose release has run out. */
    for (int v = 0; v < (int)PDSND_MAX_VOICES; ++v) {
        struct Voice *vs = &s_voices[v];
        if (vs->release && --vs->release == 0) {
            voice_unpin(vs);
        }
    }

    const u32 count = f->count < PDSND_MAX_CMDS ? f->count : PDSND_MAX_CMDS;
    for (u32 c = 0; c < count; ++c) {
        const PdSndVoiceCmd *cmd = &f->cmds[c];
        if (cmd->op == PDSND_OP_STOPALL) {
            for (int k = 0; k < (int)PDSND_MAX_VOICES; ++k) {
                koff[voice_core(k)] |= 1u << voice_index(k);
                s_voices[k].steps_left = 0;
                s_voices[k].release = RELEASE_FRAMES;
                s_keyed[k] = 0;
            }
            continue;
        }
        if (cmd->op == PDSND_OP_FX) {
            apply_fx(cmd->voice & 1, cmd->entry, cmd->pitch, cmd->cur_l, cmd->cur_r);
            continue;
        }
        if (cmd->voice >= PDSND_MAX_VOICES) {
            continue;
        }
        int v;
        if (cmd->op == PDSND_OP_KEYON) {
            const int old = s_map[cmd->voice];
            v = voice_assign(cmd->voice, (cmd->flags & PDSND_FLAG_BUS1) ? 1 : 0);
            if (old >= 0 && old != v && s_keyed[old]) {
                /* moved to the other core: release the old voice */
                koff[voice_core(old)] |= 1u << voice_index(old);
                s_voices[old].steps_left = 0;
                s_voices[old].release = RELEASE_FRAMES;
                s_keyed[old] = 0;
                voice_set_wet(old, 0);
            }
        } else {
            v = s_map[cmd->voice];
        }
        if (v < 0) {
            continue;
        }
        struct Voice *vs = &s_voices[v];
        switch (cmd->op) {
        case PDSND_OP_KEYON: {
            if (cmd->entry >= s_header.entry_count) {
                ++s_status.failures;
                break;
            }
            /* A KEYON on a voice still keyed this frame restarts it. */
            voice_unpin(vs);
            const int slot = ensure_resident(cmd->entry);
            if (slot < 0) {
                ++s_status.failures;
                break;
            }
            const Spu2BankEntry *e = &s_entries[cmd->entry];
            const u32 addr = s_slots[slot].addr;
            s_slots[slot].refs++;
            vs->entry = cmd->entry;
            vs->release = 0;
            const int reg = voice_reg(v);
            sceSdSetAddr(reg | SD_VADDR_SSA, addr);
            sceSdSetAddr(reg | SD_VADDR_LSAX,
                e->loop_block != SPU2_BANK_NO_LOOP ? addr + e->loop_block * 16u : addr);
            sceSdSetParam(reg | SD_VPARAM_PITCH, cmd->pitch);
            /* Envelope is driven through VOL; ADSR holds full level and
             * releases quickly on KOFF. */
            sceSdSetParam(reg | SD_VPARAM_ADSR1, SD_SET_ADSR1(SD_ADSR_AR_LINEARi, 0, 0xf, 0xf));
            sceSdSetParam(reg | SD_VPARAM_ADSR2,
                SD_SET_ADSR2(SD_ADSR_SR_LINEARi, 0x7f, SD_ADSR_RR_LINEARd, 0x0a));
            vs->cur_l = (s32)cmd->cur_l << 16;
            vs->cur_r = (s32)cmd->cur_r << 16;
            voice_start_ramp(vs, cmd->tgt_l, cmd->tgt_r, cmd->ramp_us);
            voice_set_volume(v, vs->cur_l, vs->cur_r);
            voice_set_wet(v, cmd->flags & PDSND_FLAG_REVERB);
            kon[voice_core(v)] |= 1u << voice_index(v);
            koff[voice_core(v)] &= ~(1u << voice_index(v));
            s_keyed[v] = 1;
            break;
        }
        case PDSND_OP_RAMP:
            voice_set_wet(v, cmd->flags & PDSND_FLAG_REVERB);
            voice_start_ramp(vs, cmd->tgt_l, cmd->tgt_r, cmd->ramp_us);
            if (vs->steps_left == 0) {
                voice_set_volume(v, vs->cur_l, vs->cur_r);
            }
            break;
        case PDSND_OP_PITCH:
            sceSdSetParam(voice_reg(v) | SD_VPARAM_PITCH, cmd->pitch);
            break;
        case PDSND_OP_KEYOFF:
            koff[voice_core(v)] |= 1u << voice_index(v);
            kon[voice_core(v)] &= ~(1u << voice_index(v));
            vs->steps_left = 0;
            s_keyed[v] = 0;
            if (vs->entry != NO_ENTRY) {
                vs->release = RELEASE_FRAMES;
            }
            break;
        default:
            break;
        }
    }

    /* audsrv resets these on its state changes; core 0 feeds core 1's
     * external input, which audsrv keeps at full volume. */
    sceSdSetParam(SD_CORE_0 | SD_PARAM_MVOLL, 0x3fff);
    sceSdSetParam(SD_CORE_0 | SD_PARAM_MVOLR, 0x3fff);

    for (int core = 0; core < 2; ++core) {
        if (s_wet_mask[core] != wet_before[core]) {
            sceSdSetSwitch(core | SD_SWITCH_VMIXEL, s_wet_mask[core]);
            sceSdSetSwitch(core | SD_SWITCH_VMIXER, s_wet_mask[core]);
        }
    }

    for (int core = 0; core < 2; ++core) {
        if (koff[core]) {
            sceSdSetSwitch(core | SD_SWITCH_KOFF, koff[core]);
        }
        if (kon[core]) {
            sceSdSetSwitch(core | SD_SWITCH_KON, kon[core]);
        }
    }
    ++s_status.frames;
    unlock();
}

/*
 * One SPU2 effect per core. A mode change re-initialises (and clears) the
 * work area at the top of SPU2 RAM; depth-only changes just set EVOL.
 */
static struct { int mode, depth, delay, feedback; } s_fx[2];

static void apply_fx(int core, int mode, int depth, int delay, int feedback)
{
    if (mode == s_fx[core].mode && delay == s_fx[core].delay &&
        feedback == s_fx[core].feedback) {
        if (depth != s_fx[core].depth) {
            sceSdSetParam(core | SD_PARAM_EVOLL, (u16)depth);
            sceSdSetParam(core | SD_PARAM_EVOLR, (u16)depth);
            s_fx[core].depth = depth;
        }
        return;
    }
    sceSdSetCoreAttr(core | SD_CORE_EFFECT_ENABLE, 0);
    sceSdSetParam(core | SD_PARAM_EVOLL, 0);
    sceSdSetParam(core | SD_PARAM_EVOLR, 0);
    if (mode != 0) {
        sceSdEffectAttr fx;
        memset(&fx, 0, sizeof(fx));
        fx.core = core;
        fx.mode = mode | SD_EFFECT_MODE_CLEAR;
        fx.depth_L = (short)depth;
        fx.depth_R = (short)depth;
        fx.delay = delay;
        fx.feedback = feedback;
        sceSdSetAddr(core | SD_ADDR_EEA, core == 1 ? 0x1fffff : 0x1dffff);
        sceSdSetEffectAttr(core, &fx);
        sceSdSetParam(core | SD_PARAM_EVOLL, (u16)depth);
        sceSdSetParam(core | SD_PARAM_EVOLR, (u16)depth);
        sceSdSetCoreAttr(core | SD_CORE_EFFECT_ENABLE, 1);
    }
    s_fx[core].mode = mode;
    s_fx[core].depth = depth;
    s_fx[core].delay = delay;
    s_fx[core].feedback = feedback;
    ++s_status.fx_changes;
}

/* ------------------------------------------------------------------ */

static void ramp_thread(void *arg)
{
    (void)arg;
    for (;;) {
        DelayThread(RAMP_STEP_US);
        lock();
        for (int v = 0; v < (int)PDSND_MAX_VOICES; ++v) {
            struct Voice *vs = &s_voices[v];
            if (vs->steps_left == 0) {
                continue;
            }
            if (--vs->steps_left == 0) {
                vs->cur_l = vs->tgt_l;
                vs->cur_r = vs->tgt_r;
            } else {
                vs->cur_l += vs->step_l;
                vs->cur_r += vs->step_r;
            }
            voice_set_volume(v, vs->cur_l, vs->cur_r);
        }
        unlock();
    }
}

static void worker_thread(void *arg)
{
    (void)arg;
    for (;;) {
        WaitSema(s_work_sema);
        const u32 tail = s_q_tail;
        apply_frame(&s_queue[tail % QUEUE_FRAMES]);
        s_q_tail = tail + 1;
        lock();
        stream_step();
        unlock();
    }
}

/* ------------------------------------------------------------------ */

static int do_init(const PdSndInitArgs *a)
{
    if (a->protocol != PDSND_PROTOCOL) {
        return -1;
    }
    if (s_fd >= 0) {
        return 0; /* already initialised */
    }
    char path[sizeof(a->path) + 1];
    memcpy(path, a->path, sizeof(a->path));
    path[sizeof(a->path)] = '\0';
    const int fd = iomanX_open(path, FIO_O_RDONLY);
    if (fd < 0) {
        return -2;
    }
    if (iomanX_read(fd, &s_header, sizeof(s_header)) != (int)sizeof(s_header) ||
        s_header.magic != SPU2_BANK_MAGIC || s_header.version != SPU2_BANK_VERSION ||
        s_header.entry_count == 0 || s_header.entry_count >= NO_ENTRY) {
        iomanX_close(fd);
        return -3;
    }
    const u32 table = s_header.entry_count * sizeof(Spu2BankEntry);
    s_entries = AllocSysMemory(0, table, NULL);
    s_io_buf = AllocSysMemory(0, IO_CHUNK, NULL);
    if (!s_entries || !s_io_buf) {
        iomanX_close(fd);
        return -4;
    }
    if (iomanX_read(fd, s_entries, (int)table) != (int)table) {
        iomanX_close(fd);
        return -5;
    }
    s_spu_base = (a->spu_base + 15u) & ~15u;
    s_spu_end = a->spu_end & ~15u;
    if (s_spu_end <= s_spu_base) {
        iomanX_close(fd);
        return -6;
    }
    for (int v = 0; v < (int)PDSND_MAX_VOICES; ++v) {
        s_voices[v].entry = NO_ENTRY;
    }
    /* Voices: dry to the core's output; core 1 also mixes its external
     * input (core 0) and sound-data input (audsrv's stream) dry, and its
     * voices' wet sends through the effect unit. */
    sceSdSetParam(SD_CORE_0 | SD_PARAM_MMIX, 0x000f);
    sceSdSetParam(SD_CORE_1 | SD_PARAM_MMIX, 0x0ccf);
    sceSdSetSwitch(SD_CORE_0 | SD_SWITCH_VMIXL, 0xffffff);
    sceSdSetSwitch(SD_CORE_0 | SD_SWITCH_VMIXR, 0xffffff);
    sceSdSetSwitch(SD_CORE_1 | SD_SWITCH_VMIXL, 0xffffff);
    sceSdSetSwitch(SD_CORE_1 | SD_SWITCH_VMIXR, 0xffffff);
    sceSdSetSwitch(SD_CORE_0 | SD_SWITCH_VMIXEL, 0);
    sceSdSetSwitch(SD_CORE_0 | SD_SWITCH_VMIXER, 0);
    sceSdSetSwitch(SD_CORE_1 | SD_SWITCH_VMIXEL, 0);
    sceSdSetSwitch(SD_CORE_1 | SD_SWITCH_VMIXER, 0);
    s_wet_mask[0] = s_wet_mask[1] = 0;
    for (int v = 0; v < (int)PDSND_MAX_VOICES; ++v) {
        s_map[v] = -1;
        s_owner[v] = -1;
        s_keyed[v] = 0;
    }
    s_fx[0].mode = s_fx[1].mode = 0;
    sceSdSetParam(SD_CORE_0 | SD_PARAM_MVOLL, 0x3fff);
    sceSdSetParam(SD_CORE_0 | SD_PARAM_MVOLR, 0x3fff);

    /* Effects start off; the EE sends the game's FX bus settings. */
    s_fd = fd;
    s_init_result.entries = s_header.entry_count;
    s_init_result.rom_crc = s_header.rom_crc;
    s_init_result.rom_version = s_header.rom_version;
    return 0;
}

static void *rpc_handler(int func, void *buf, int size)
{
    (void)size;
    switch (func) {
    case PDSND_FUNC_INIT:
        lock();
        s_init_result.result = do_init((const PdSndInitArgs *)buf);
        unlock();
        memcpy(buf, &s_init_result, sizeof(s_init_result));
        return buf;
    case PDSND_FUNC_FRAME: {
        if (s_fd < 0) {
            return buf;
        }
        const u32 head = s_q_head;
        if (head - s_q_tail >= QUEUE_FRAMES) {
            ++s_status.queue_drops;
            return buf;
        }
        const PdSndFrameArgs *f = (const PdSndFrameArgs *)buf;
        const u32 count = f->count < PDSND_MAX_CMDS ? f->count : PDSND_MAX_CMDS;
        PdSndFrameArgs *q = &s_queue[head % QUEUE_FRAMES];
        memcpy(q, f, 16u + count * sizeof(PdSndVoiceCmd));
        q->count = count;
        s_q_head = head + 1;
        SignalSema(s_work_sema);
        return buf;
    }
    case PDSND_FUNC_STATUS:
        memcpy(buf, &s_status, sizeof(s_status));
        return buf;
    default:
        return buf;
    }
}

static void rpc_thread(void *arg)
{
    (void)arg;
    sceSifInitRpc(0);
    sceSifSetRpcQueue(&s_qd, GetThreadId());
    sceSifRegisterRpc(&s_sd, PDSND_RPC_ID, rpc_handler, s_rpc_buffer, NULL, NULL, &s_qd);
    sceSifRpcLoop(&s_qd);
}

static int start_thread(void (*fn)(void *), int priority, u32 stack)
{
    iop_thread_t t;
    t.attr = TH_C;
    t.option = 0;
    t.thread = fn;
    t.stacksize = stack;
    t.priority = priority;
    const int id = CreateThread(&t);
    if (id > 0) {
        StartThread(id, NULL);
    }
    return id;
}

int _start(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    iop_sema_t sema;
    sema.attr = 0;
    sema.option = 0;
    sema.initial = 1;
    sema.max = 1;
    s_lock = CreateSema(&sema);
    sema.initial = 0;
    sema.max = QUEUE_FRAMES;
    s_work_sema = CreateSema(&sema);
    if (s_lock < 0 || s_work_sema < 0) {
        return MODULE_NO_RESIDENT_END;
    }
    /* Ramp above worker (IO) so envelopes stay smooth during streaming. */
    if (start_thread(ramp_thread, 0x20, 0x800) <= 0 ||
        start_thread(worker_thread, 0x28, 0x1000) <= 0 ||
        start_thread(rpc_thread, 0x24, 0x1000) <= 0) {
        return MODULE_NO_RESIDENT_END;
    }
    printf("pdsnd: SPU2 voice module ready\n");
    return MODULE_RESIDENT_END;
}
