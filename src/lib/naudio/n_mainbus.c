#include <ultra64.h>
#include "n_synthInternals.h"
#ifdef PLATFORM_PS2
#include "spu2_voices_ps2.h"
#endif

s32 func00037fc0(s32 arg0, Acmd **cmd);
Acmd *n_alAuxBusPull(s32 sampleOffset, Acmd *cmdptr, s32 fxBus, s32 *numpulls);
void func0003ba64(struct fx *fx, f32 outputrate);

Acmd *n_alMainBusPull(s32 sampleOffset, Acmd *p)
{
	Acmd *ptr = p;
	s32 i;

	if (!func00037fc0(FIXED_SAMPLE, &ptr)) {
		aClearBuffer(ptr++, N_AL_MAIN_L_OUT, N_AL_DIVIDED << 1);
	}

	for (i = 0; i < n_syn->maxAuxBusses; i++) {
#ifdef PLATFORM_PS2
		/* SPU2 hardware voices: the voices and their reverb live on the
		 * SPU2. Still walk the bus so the voices' parameter events are
		 * turned into SPU2 commands, but skip the reverb and the mixes of
		 * what would only be silence. */
		if (g_Ps2HwVoices) {
			s32 pulls;
			ptr = n_alAuxBusPull(sampleOffset, ptr, i, &pulls);
			goto filter;
		}
#endif
		ptr = (n_syn->mainBus->filter.handler)(sampleOffset, ptr, i);

		if (var8009c344[i]) {
			if (var8009c346[i]) {
				aMix(ptr++, 0, 0x8000, N_AL_AUX_L_OUT, N_AL_MAIN_L_OUT);
			} else {
				aMix(ptr++, 0, 0x7fff, N_AL_AUX_L_OUT, N_AL_MAIN_R_OUT);
			}
		} else {
			if (var8009c346[i]) {
				aMix(ptr++, 0, 0x8000, N_AL_AUX_L_OUT, N_AL_MAIN_R_OUT);
			} else {
				aMix(ptr++, 0, 0x7fff, N_AL_AUX_L_OUT, N_AL_MAIN_R_OUT);
			}

			aMix(ptr++, 0, 0x7fff, N_AL_AUX_L_OUT, N_AL_MAIN_L_OUT);
		}

#ifdef PLATFORM_PS2
filter:
#endif
		if (n_syn->auxBus[i].unk44->fx.unk02 > 0) {
			struct auxbus44 *bus44 = n_syn->auxBus[i].unk44;

			if (bus44->unk28) {
				func0003ba64(&bus44->fx, n_syn->outputRate);
			}

			n_aLoadADPCM(ptr++, 32, osVirtualToPhysical(&bus44->fx.unk08));

			n_aPoleFilter(ptr++, 0, N_AL_MAIN_L_OUT, 0, osVirtualToPhysical(bus44->unk2c) & 0xffffff);
			n_aPoleFilter(ptr++, 0, N_AL_MAIN_R_OUT, 0, osVirtualToPhysical(bus44->unk30) & 0xffffff);

			bus44->unk28 = 0;
		}
	}

	return ptr;
}
