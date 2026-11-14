#include <ultra64.h>
#include <n_libaudio.h>
#include "naudio/n_abi.h"
#include "ultra/audio/synthInternals.h"
#include "naudio/n_synthInternals.h"
#include "lib/mp3.h"
#include "mp3/mp3.h"
#include "types.h"
#include "platform.h"

/**
 * These two ABI commands are used in this file, but the format of the data
 * doesn't match the format used by n_audio's ABI.
 */
#ifdef PLATFORM_N64

#define	acmd07(pkt, a, b)                                    \
{                                                            \
	Acmd *_a = (Acmd *)pkt;                                  \
	_a->words.w0 = _SHIFTL(0x07, 24, 8) | _SHIFTL(a, 0, 16); \
	_a->words.w1 = b;                                        \
}

#define	acmd08(pkt, a)                   \
{                                        \
	Acmd *_a = (Acmd *)pkt;              \
	_a->words.w0 = _SHIFTL(0x08, 24, 8); \
	_a->words.w1 = a;                    \
}

#else

#include "mixer.h"

#define	acmd07(pkt, a, b, c, r) aPlayMP3(pkt, a, b, c, r)
#define	acmd08(pkt, a) do { } while(0)

#endif

struct mp3vars g_Mp3Vars;
struct asistream *g_AsiStream;

#ifdef PLATFORM_PS2
#include "spu2_voices_ps2.h"
/* the voices header pulls in <stdbool.h>; keep the game's 32-bit bool */
#undef bool
#define bool s32

/*
 * PS2: speech clips that the SPU2 bank carries (pd.pssb, transcoded from
 * the ROM's MP3 files) play on a hardware voice. The EE then neither loads
 * the MP3 file nor runs either MPEG decoder; it only keeps the player's
 * state machine (playing/paused/stopped, volume, pan) and the clip's
 * running time, so everything above this file sees the same behaviour.
 * Clips without a bank entry keep the software path.
 */
#define MP3_HW_VOICE 47 /* above the synth's 30 physical voices */

static struct {
	s32 next_entry; /* chosen by mp3Ps2SelectFile() for the next play */
	u32 next_rate;
	u32 next_samples;
	s32 entry;      /* clip on the hardware voice, -1 = software path */
	u32 rate;
	u32 samples;
	u32 played;     /* output samples since KEYON */
	bool keyed;
	bool paused;
	s16 vol_l;
	s16 vol_r;
} g_Mp3Hw = { -1, 0, 0, -1, 0, 0, 0, false, false, 0, 0 };

bool mp3Ps2SelectFile(s32 filenum)
{
	g_Mp3Hw.next_entry = ps2HwVoicesMp3Entry(filenum, &g_Mp3Hw.next_rate, &g_Mp3Hw.next_samples);
	return g_Mp3Hw.next_entry >= 0;
}

static u16 mp3Ps2Pitch(void)
{
	return (u16)((g_Mp3Hw.rate * 4096u + 24000u) / 48000u);
}

/* Same pan handling as func00038924; same -6 dB unity as hwVoiceVol. */
static void mp3Ps2Volume(s16 *l, s16 *r)
{
	s32 pan = g_Mp3Vars.var8009c3ec & 0x7f;
	const s32 vol = (s32)g_Mp3Vars.var8009c3e4;

	if (var8009c340.headphone) {
		pan = (pan >> 1) + 32;
	} else if (var8009c340.mono) {
		pan = 64;
	}

	*l = (s16)(((n_eqpower[pan] * vol) >> 15) >> 2);
	*r = (s16)(((n_eqpower[127 - pan] * vol) >> 15) >> 2);
}

static void mp3Ps2Cmd(u8 op, u16 pitch, s16 l, s16 r, u32 ramp_us)
{
	PdSndVoiceCmd cmd = {0};
	cmd.op = op;
	cmd.voice = MP3_HW_VOICE;
	cmd.entry = (u16)g_Mp3Hw.entry;
	cmd.pitch = pitch;
	cmd.cur_l = cmd.tgt_l = l;
	cmd.cur_r = cmd.tgt_r = r;
	cmd.ramp_us = ramp_us;
	ps2HwVoicesPush(&cmd);
}

static s32 mp3Ps2Frame(s32 numsamples)
{
	s16 l, r;

	switch (g_Mp3Vars.var8009c3e0) {
	case 4: /* starting */
		mp3Ps2Volume(&l, &r);
		mp3Ps2Cmd(PDSND_OP_KEYON, mp3Ps2Pitch(), l, r, 0);
		g_Mp3Hw.vol_l = l;
		g_Mp3Hw.vol_r = r;
		g_Mp3Hw.keyed = true;
		g_Mp3Hw.paused = false;
		g_Mp3Hw.played = 0;
		g_Mp3Vars.var8009c3e0 = 1;
		break;
	case 5: /* resuming */
		if (g_Mp3Hw.paused) {
			mp3Ps2Cmd(PDSND_OP_PITCH, mp3Ps2Pitch(), 0, 0, 0);
			g_Mp3Hw.paused = false;
		}
		g_Mp3Vars.var8009c3e0 = 1;
		break;
	case 2: /* paused: hold the voice where it is */
		if (g_Mp3Hw.keyed && !g_Mp3Hw.paused) {
			mp3Ps2Cmd(PDSND_OP_PITCH, 0, 0, 0, 0);
			g_Mp3Hw.paused = true;
		}
		return 0;
	case 1:
		break;
	default: /* stopped or finished */
		if (g_Mp3Hw.keyed) {
			mp3Ps2Cmd(PDSND_OP_KEYOFF, 0, 0, 0, 0);
			g_Mp3Hw.keyed = false;
		}
		g_Mp3Hw.entry = -1;
		return 0;
	}

	mp3Ps2Volume(&l, &r);

	if (l != g_Mp3Hw.vol_l || r != g_Mp3Hw.vol_r) {
		const u32 ramp = (u32)SAMPLE184(g_Mp3Vars.var8009c3e8);
		mp3Ps2Cmd(PDSND_OP_RAMP, 0, l, r,
				n_syn->outputRate > 0 ? (u32)((u64)ramp * 1000000u / (u32)n_syn->outputRate) : 0);
		g_Mp3Hw.vol_l = l;
		g_Mp3Hw.vol_r = r;
	}

	/* The clip's running time decides when the player reports it done. */
	g_Mp3Hw.played += (u32)numsamples;

	if ((u64)g_Mp3Hw.played * g_Mp3Hw.rate >= (u64)g_Mp3Hw.samples * (u32)n_syn->outputRate) {
		mp3Ps2Cmd(PDSND_OP_KEYOFF, 0, 0, 0, 0);
		g_Mp3Hw.keyed = false;
		g_Mp3Hw.entry = -1;
		g_Mp3Vars.var8009c3e0 = 3;
	}

	return 0;
}
#endif

s32 func00038ba8(s32 arg0, u8 *arg1, s32 arg2, s32 arg3);

extern f32 *var8009c6d8;
extern f32 *var8009c6dc;
extern struct mp3decfourbytes *var8009c640;
extern f32 *var8009c644;

void mp3Init(ALHeap *heap)
{
	bzero(&g_Mp3Vars, sizeof(struct mp3vars));

#if VERSION < VERSION_NTSC_1_0
	rmonPrintf("MPEG : RWI -> Allocating %d bytes for ASISTREAM from audio heap\n", sizeof(struct asistream));
#endif

	g_AsiStream = alHeapAlloc(heap, sizeof(struct asistream), 1);
	var8005f6f8 = alHeapAlloc(heap, 1, 34 * 256 * sizeof(var8005f6f8[0]));
	var8005f6fc = alHeapAlloc(heap, 1, 34 * 256 * sizeof(var8005f6fc[0]));

	var8009c6d8 = alHeapAlloc(heap, 256 * sizeof(var8009c6d8[0]), 1);
	var8009c6dc = alHeapAlloc(heap, 256 * sizeof(var8009c6dc[0]), 1);
#ifdef PLATFORM_64BIT
	var8009c640 = alHeapAlloc(heap, 2*10500 * sizeof(struct mp3decfourbytes), 1);
#else
	var8009c640 = alHeapAlloc(heap, 10500 * sizeof(struct mp3decfourbytes), 1);
#endif
	var8009c644 = alHeapAlloc(heap, 8192 * sizeof(var8009c644[0]), 1);

	mp3mainInit();

	g_Mp3Vars.var8009c398 = alHeapAlloc(heap, 1, ALIGN16(sizeof(ALEnvMixer)));
	g_Mp3Vars.var8009c3d4[0] = alHeapAlloc(heap, 1, 0x440);
	g_Mp3Vars.var8009c3e0 = 0;
	g_Mp3Vars.var8009c3e4 = 0x7fff;
	g_Mp3Vars.var8009c3ec = g_Mp3Vars.var8009c3ee = 0x40;
	g_Mp3Vars.var8009c39e = 0x7fff;
	g_Mp3Vars.var8009c39c = 0x40;

	g_Mp3Vars.ivol1 = (n_eqpower[g_Mp3Vars.var8009c39c & 0x7f] * g_Mp3Vars.var8009c39e) >> 15;
	g_Mp3Vars.ivol2 = (n_eqpower[127 - (g_Mp3Vars.var8009c39c & 0x7f)] * g_Mp3Vars.var8009c39e) >> 15;

	g_Mp3Vars.var8009c3b4 = 1;
	g_Mp3Vars.var8009c3a4 = 0x7ffc;

	if (g_Mp3Vars.var8009c39c & 0x80) {
		g_Mp3Vars.var8009c3a6 = 1;
	} else {
		g_Mp3Vars.var8009c3a6 = 0;
	}

	func00038b90(func00038ba8);
}

void mp3PlayFile(uintptr_t romaddr, s32 filesize)
{
	if (g_Mp3Vars.var8009c3dc == NULL) {
		return;
	}

#ifdef PLATFORM_PS2
	if (g_Mp3Hw.keyed && g_Mp3Hw.entry >= 0) {
		/* a new clip cuts the current one, as the software player does */
		mp3Ps2Cmd(PDSND_OP_KEYOFF, 0, 0, 0, 0);
		g_Mp3Hw.keyed = false;
	}

	g_Mp3Hw.entry = g_Mp3Hw.next_entry;

	if (g_Mp3Hw.entry >= 0) {
		g_Mp3Hw.rate = g_Mp3Hw.next_rate;
		g_Mp3Hw.samples = g_Mp3Hw.next_samples;
		g_Mp3Vars.romaddr = 0;
		g_Mp3Vars.filesize = 0;
		g_Mp3Vars.var8009c3c4 = 0;
		g_Mp3Vars.var8009c3e8 = 0;
		g_Mp3Vars.var8009c3e4 = 0x7fff;
		g_Mp3Vars.var8009c3e0 = 4;
		return;
	}
#endif

	g_Mp3Vars.romaddr = romaddr;
	g_Mp3Vars.filesize = filesize;
	g_Mp3Vars.var8009c3c4 = 0;
	g_Mp3Vars.var8009c3e8 = 0;
	g_Mp3Vars.var8009c3e4 = 0x7fff;
	g_Mp3Vars.var8009c3f0 = 5;
#ifndef PLATFORM_N64
	g_Mp3Vars.reset = 1;
#endif

	mp3Dma();

	g_Mp3Vars.var8009c3e0 = 4;
}

void func00037e1c(void)
{
	g_Mp3Vars.var8009c3e0 = 3;
}

void func00037e38(void)
{
	if (g_Mp3Vars.var8009c3e0 == 1) {
		g_Mp3Vars.var8009c3e0 = 2;
	}
}

void func00037e68(void)
{
	if (g_Mp3Vars.var8009c3e0 == 2) {
		g_Mp3Vars.var8009c3f0 = 5;
		g_Mp3Vars.var8009c3e0 = 5;
	}
}

s32 func00037ea4(void)
{
	if (g_Mp3Vars.var8009c3e0 == 1
			|| g_Mp3Vars.var8009c3e0 == 4
			|| g_Mp3Vars.var8009c3e0 == 5
			|| g_Mp3Vars.var8009c3e0 == 2) {
		return g_Mp3Vars.var8009c3e0;
	} else {
		return 0;
	}
}

void func00037f08(s32 arg0, bool arg1)
{
	if (arg0 < 0) {
		g_Mp3Vars.var8009c3e4 = 0;
	} else if (arg0 > 0x7fff) {
		g_Mp3Vars.var8009c3e4 = 0x7fff;
	} else {
		g_Mp3Vars.var8009c3e4 = arg0;
	}

	g_Mp3Vars.var8009c3e8 = arg1;
}

void func00037f5c(s32 arg0, bool arg1)
{
	if (arg0 > 255) {
		arg0 = 255;
	}

	if (arg0 < 0) {
		arg0 = 0;
	}

	g_Mp3Vars.var8009c3ee = arg0;

	if (arg1) {
		g_Mp3Vars.var8009c3ec = g_Mp3Vars.var8009c3ee;
	}
}

void func00037fa8(s32 arg0, s32 arg1)
{
	// empty
}

s32 func00037fc0(s32 arg0, Acmd **cmd)
{
	s32 i;
	s32 sp60;
	s32 sp5c = 0;
	struct mp3thing *sp58;
	struct mp3thing *sp54 = NULL;
	s32 sp50;
	s32 sp4c = N_AL_MAIN_L_OUT;
	s32 sp48 = N_AL_MAIN_R_OUT;

	if (g_Mp3Vars.var8009c3ec != g_Mp3Vars.var8009c3ee) {
		sp60 = g_Mp3Vars.var8009c3ee - g_Mp3Vars.var8009c3ec;

		if (sp60 >= -2 && sp60 <= 2) {
			g_Mp3Vars.var8009c3ec += sp60;
		} else if (sp60 >= 3) {
			g_Mp3Vars.var8009c3ec += 2;
		} else {
			g_Mp3Vars.var8009c3ec -= 2;
		}
	}

#ifdef PLATFORM_PS2
	if (g_Mp3Hw.entry >= 0) {
		return mp3Ps2Frame(arg0);
	}
#endif

	if (g_Mp3Vars.var8009c3e0 == 4) {
		mp3Dma();

		if (g_Mp3Vars.var8009c3f0 == 0) {
			g_Mp3Vars.var8009c394 = mp3main00044460(0, g_Mp3Vars.var8009c3dc, g_Mp3Vars.filesize);

			if (g_Mp3Vars.var8009c394 == NULL) {
				g_Mp3Vars.var8009c3e0 = 0;
				return 0;
			} else {
				g_Mp3Vars.var8009c3e0 = 1;
				g_Mp3Vars.var8009c3b4 = 1;
				g_Mp3Vars.var8009c3c8 = 0;
				g_Mp3Vars.var8009c3cc = 0;
				g_Mp3Vars.var8009c3d0 = 0;
				g_Mp3Vars.var8009c3d8 = 0;

				bzero(g_Mp3Vars.var8009c3d4[0], 0x440);
			}
		} else {
			g_Mp3Vars.var8009c3f0--;
		}
	}

	if (g_Mp3Vars.var8009c3e0 == 5) {
		mp3Dma();

		if (g_Mp3Vars.var8009c3f0 == 0) {
			g_Mp3Vars.var8009c3e0 = 1;
		} else {
			g_Mp3Vars.var8009c3f0--;
		}
	}

	if (g_Mp3Vars.var8009c3e0 == 1) {
		if (arg0 + g_Mp3Vars.var8009c3d0 > g_Mp3Vars.var8009c3cc) {
			g_Mp3Vars.var8009c3cc -= g_Mp3Vars.var8009c3d0;

			if (g_Mp3Vars.var8009c3cc != 0 && g_Mp3Vars.var8009c3c8 != 0) {
				sp54 = (struct mp3thing *)(g_Mp3Vars.var8009c3d0 + g_Mp3Vars.var8009c3c8->unk00);
				sp50 = g_Mp3Vars.var8009c3cc;
			}

			g_Mp3Vars.var8009c3d0 = 0;
			g_Mp3Vars.var8009c3cc = 0;

			if (mp3main0004453c(g_Mp3Vars.var8009c394, &sp58, &sp5c)) {
				g_Mp3Vars.var8009c3c8 = sp58;

				for (i = 0; i < sp5c; i++) {
					acmd08((*cmd)++, osVirtualToPhysical(g_Mp3Vars.var8009c3d4[i]));
#ifdef PLATFORM_N64
					acmd07((*cmd)++, g_Mp3Vars.var8009c3d8, osVirtualToPhysical(sp58));
#else
					// hijack the command to pass the entirety of the mp3 data to the mixer
					acmd07((*cmd)++, g_Mp3Vars.romaddr, g_Mp3Vars.filesize, osVirtualToPhysical(sp58), g_Mp3Vars.reset);
					g_Mp3Vars.reset = 0;
#endif

					sp58++;
				}

				g_Mp3Vars.var8009c3d8 = (g_Mp3Vars.var8009c3d8 - 0x24) & 0x1e;
				g_Mp3Vars.var8009c3cc = 0x240;
				g_Mp3Vars.var8009c3f1 = sp5c == 2;
			} else {
				// empty
			}
		}

		mp3Dma();

		if (g_Mp3Vars.var8009c3f1 == 0) {
			sp4c = 0;
		}

		if (sp54 != NULL) {
			n_aLoadBuffer((*cmd)++, sp50 + sp50, sp4c, osVirtualToPhysical(sp54));

			if (g_Mp3Vars.var8009c3f1) {
				sp54++;
				n_aLoadBuffer((*cmd)++, sp50 + sp50, sp48, osVirtualToPhysical(sp54));
			}

			sp4c += sp50 + sp50;
			sp48 += sp50 + sp50;
			arg0 -= sp50;
		}

		if (arg0 > 0 && g_Mp3Vars.var8009c3c8) {
			sp54 = (struct mp3thing *)(g_Mp3Vars.var8009c3d0 * 2 + (uintptr_t)g_Mp3Vars.var8009c3c8);

			n_aLoadBuffer((*cmd)++, arg0 + arg0, sp4c, osVirtualToPhysical(sp54));

			if (g_Mp3Vars.var8009c3f1) {
				sp54++;
				n_aLoadBuffer((*cmd)++, arg0 + arg0, sp48, osVirtualToPhysical(sp54));
			}

			g_Mp3Vars.var8009c3d0 += arg0;
		}

		func00038924(&g_Mp3Vars);

		if (!g_Mp3Vars.var8009c3f1) {
			aClearBuffer((*cmd)++, N_AL_MAIN_L_OUT, N_AL_TEMP_2);

			if (g_Mp3Vars.var8009c3b4) {
				g_Mp3Vars.var8009c3b4 = 0;

				g_Mp3Vars.var8009c3ac = n_eqpower[g_Mp3Vars.var8009c39c & 0x7f] * g_Mp3Vars.var8009c39e >> 0xf;
				g_Mp3Vars.ratem1 = _getRate(g_Mp3Vars.ivol1, g_Mp3Vars.var8009c3ac, g_Mp3Vars.var8009c3bc, &g_Mp3Vars.ratel1);

				g_Mp3Vars.var8009c3b2 = n_eqpower[0x7f - (g_Mp3Vars.var8009c39c & 0x7f)] * g_Mp3Vars.var8009c39e >> 0xf;
				g_Mp3Vars.ratem2 = _getRate(g_Mp3Vars.ivol2, g_Mp3Vars.var8009c3b2, g_Mp3Vars.var8009c3bc, &g_Mp3Vars.ratel2);

				n_aSetVolume((*cmd)++, A_VOL | A_LEFT, g_Mp3Vars.ivol1, g_Mp3Vars.var8009c3a4, g_Mp3Vars.var8009c3a6);
				n_aSetVolume((*cmd)++, A_VOL | A_RIGHT, g_Mp3Vars.var8009c3b2, g_Mp3Vars.ratem2, g_Mp3Vars.ratel2);
				n_aSetVolume((*cmd)++, A_RATE, g_Mp3Vars.var8009c3ac, g_Mp3Vars.ratem1, g_Mp3Vars.ratel1);
				n_aEnvMixer((*cmd)++, 1, g_Mp3Vars.ivol2, osVirtualToPhysical(g_Mp3Vars.var8009c398));
			} else {
				n_aEnvMixer((*cmd)++, 0, 0, osVirtualToPhysical(g_Mp3Vars.var8009c398));
			}

			g_Mp3Vars.samples += SAMPLES;

			if (g_Mp3Vars.samples > g_Mp3Vars.var8009c3bc) {
				g_Mp3Vars.samples = g_Mp3Vars.var8009c3bc;
			}
		}
	} else {
		return 0;
	}

	return 1;
}

void func00038924(struct mp3vars *vars)
{
	if (vars->var8009c39e != vars->var8009c3e4 || vars->var8009c39c != vars->var8009c3ec) {
		if (vars->samples >= vars->var8009c3bc) {
			vars->var8009c3ac = (n_eqpower[vars->var8009c39c & 0x7f] * vars->var8009c39e >> 15);
			vars->var8009c3b2 = (n_eqpower[127 - (vars->var8009c39c & 0x7f)] * vars->var8009c39e >> 15);
			vars->samples = vars->var8009c3bc;
			vars->ivol1 = vars->var8009c3ac;
			vars->ivol2 = vars->var8009c3b2;
		} else {
			vars->ivol1 = _getVol(vars->ivol1, vars->samples, vars->ratem1, vars->ratel1);
			vars->ivol2 = _getVol(vars->ivol2, vars->samples, vars->ratem2, vars->ratel2);
		}

		if (vars->ivol1 == 0) {
			vars->ivol1 = 1;
		}

		if (vars->ivol2 == 0) {
			vars->ivol2 = 1;
		}

		vars->var8009c39e = vars->var8009c3e4;

		if (vars->var8009c39c != vars->var8009c3ec) {
			if (var8009c340.headphone) {
				vars->var8009c39c = ((s16)(vars->var8009c3ec & 0x7f) >> 1) + 32;
			} else {
				if (var8009c340.mono) {
					vars->var8009c39c = 64;
				} else {
					vars->var8009c39c = vars->var8009c3ec;
				}
			}

			vars->var8009c39c &= (vars->var8009c3ec & 0x80) + 0x7f;

			vars->var8009c3a6 = (vars->var8009c39c & 0x80) ? 1 : 0;
		}

		vars->samples = 0;
		vars->var8009c3bc = SAMPLE184(vars->var8009c3e8);
		vars->var8009c3b4 = 1;
	}
}

void func00038b90(void *fn)
{
	g_Mp3Vars.var8009c3dc = fn;
}

s32 func00038ba8(s32 arg0, u8 *arg1, s32 arg2, s32 arg3)
{
	uintptr_t sp1c;
	ALDMAproc proc;

	if (arg3 != -1) {
		g_Mp3Vars.var8009c3c4 = arg3;
	}

	if (g_Mp3Vars.var8009c3c4 + arg2 > g_Mp3Vars.filesize) {
		arg2 = g_Mp3Vars.filesize - g_Mp3Vars.var8009c3c4;
	}

	proc = n_syn->dma(&sp1c);
	sp1c = OS_K0_TO_PHYSICAL(proc(g_Mp3Vars.romaddr + g_Mp3Vars.var8009c3c4, arg2, 0));

	bcopy((u8 *)sp1c, arg1, arg2);

	g_Mp3Vars.var8009c3c4 += arg2;

	return arg2;
}

void mp3Dma(void)
{
	uintptr_t state;
	ALDMAproc proc;

	proc = n_syn->dma(&state);

	proc(g_Mp3Vars.romaddr + g_Mp3Vars.var8009c3c4, 0x400, 0);
}
