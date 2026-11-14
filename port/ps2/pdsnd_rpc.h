#ifndef PD_PS2_PDSND_RPC_H
#define PD_PS2_PDSND_RPC_H

/*
 * EE <-> IOP protocol of pdsnd.irx, the project's SPU2 hardware-voice module
 * (docs/ps2-spu2-audio.md, stage 3). Shared by both sides; plain C types.
 *
 * The EE sends one FRAME call per audio frame (non-blocking SIF RPC) with
 * the voice changes of that frame. The IOP queues them for its worker
 * thread, which streams any missing samples from the PS-ADPCM bank file
 * into an SPU2 RAM cache and applies the commands. Volume ramps (the N64
 * envelope segments) run on the IOP in 1 ms steps.
 */

#ifdef _IOP
#include <types.h>
typedef u8 pdsnd_u8;
typedef u16 pdsnd_u16;
typedef s16 pdsnd_s16;
typedef u32 pdsnd_u32;
typedef s32 pdsnd_s32;
#else
#include <stdint.h>
typedef uint8_t pdsnd_u8;
typedef uint16_t pdsnd_u16;
typedef int16_t pdsnd_s16;
typedef uint32_t pdsnd_u32;
typedef int32_t pdsnd_s32;
#endif

#define PDSND_RPC_ID 0x50445331u /* "PDS1" */
#define PDSND_PROTOCOL 3u

enum PdSndRpcFunc {
    PDSND_FUNC_INIT = 1,
    PDSND_FUNC_FRAME = 2,
    PDSND_FUNC_STATUS = 3,
    PDSND_FUNC_QUIT = 4,
};

#define PDSND_MAX_VOICES 48u /* 24 per core */
#define PDSND_MAX_CMDS 96u

enum PdSndOp {
    PDSND_OP_KEYON = 1,  /* start entry: cur volume -> target over ramp_us */
    PDSND_OP_RAMP = 2,   /* new envelope segment from the current volume */
    PDSND_OP_PITCH = 3,
    PDSND_OP_KEYOFF = 4, /* fast release, then the sample slot is unpinned */
    PDSND_OP_STOPALL = 5,
    /* voice = core, entry = SD_EFFECT_MODE_* (0 off), pitch = depth,
     * cur_l = delay, cur_r = feedback (echo/delay modes, 0..127) */
    PDSND_OP_FX = 6,
};

#define PDSND_FLAG_REVERB 0x0001u /* send this voice to its core's effect */
#define PDSND_FLAG_BUS1 0x0002u   /* FX bus 1: play on core 0 (echo unit) */

typedef struct PdSndVoiceCmd {
    pdsnd_u8 op;
    pdsnd_u8 voice;      /* game voice 0..47 (FX: core) */
    pdsnd_u16 entry;     /* bank entry (KEYON) */
    pdsnd_u16 pitch;     /* SPU2 PITCH, 0x1000 = 48 kHz */
    pdsnd_s16 cur_l;     /* KEYON start volume, 0..0x3fff */
    pdsnd_s16 cur_r;
    pdsnd_s16 tgt_l;     /* KEYON/RAMP target volume */
    pdsnd_s16 tgt_r;
    pdsnd_u16 flags;     /* PDSND_FLAG_* (KEYON/RAMP) */
    pdsnd_u32 ramp_us;   /* 0 = jump to target */
} PdSndVoiceCmd;         /* 20 bytes */

typedef struct PdSndInitArgs {
    pdsnd_u32 protocol;
    pdsnd_u32 spu_base;  /* SPU2 RAM cache window, bytes */
    pdsnd_u32 spu_end;
    pdsnd_u16 reserved0;
    pdsnd_u16 reserved1;
    char path[240];      /* bank file, e.g. "mass:PDPS2/pd.pssb" */
} PdSndInitArgs;         /* 256 bytes */

typedef struct PdSndInitResult {
    pdsnd_s32 result;    /* 0 ok, < 0 error */
    pdsnd_u32 entries;
    pdsnd_u32 rom_crc;
    pdsnd_u32 rom_version;
} PdSndInitResult;

typedef struct PdSndFrameArgs {
    pdsnd_u32 count;
    pdsnd_u32 frame;
    pdsnd_u32 pad[2];
    PdSndVoiceCmd cmds[PDSND_MAX_CMDS];
} PdSndFrameArgs;

typedef struct PdSndStatus {
    pdsnd_u32 frames;        /* FRAME calls applied */
    pdsnd_u32 loads;         /* samples streamed into SPU2 RAM */
    pdsnd_u32 load_bytes;
    pdsnd_u32 evictions;
    pdsnd_u32 failures;      /* KEYONs dropped (no space / bad entry / IO) */
    pdsnd_u32 cache_used;    /* bytes resident */
    pdsnd_u32 load_us;       /* cumulative time spent streaming */
    pdsnd_u32 queue_drops;
    pdsnd_u32 fx_changes;
    pdsnd_u32 streams;       /* long one-shots keyed on before fully resident */
    pdsnd_u32 pad[2];
} PdSndStatus;

#endif
