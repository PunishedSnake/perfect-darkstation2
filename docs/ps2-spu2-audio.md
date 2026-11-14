# SPU2 hardware-voice audio (plan and groundwork)

## Why

Perfect Dark synthesises all audio in software, as on the N64. Each of the
~15 active voices is decoded from VADPCM, pitch-resampled, enveloped and mixed
on the EE. The rest of the mixer then runs reverb and buffer moves, and the
result goes to a single SPU2 PCM stream.

Measured in PCSX2 (Defection gameplay, COP0 cycle counters around every audio
command):

| Command        | EE cycles / s | Share |
|----------------|---------------|-------|
| resample       | ~10.8 M       | 33 %  |
| ADPCM decode   | ~6.0 M        | 18 %  |
| envelope mix   | ~5.8 M        | 18 %  |
| mix            | ~4.7 M        | 15 %  |
| buffer save/load, ADPCM book loads, misc | ~5 M | 16 % |
| **total**      | **~32 M**     | **~11 % of the EE** (~1.8 ms of a 60 fps frame) |

The SPU2 does all of this in hardware: 48 voices, ADPCM decoding, pitch,
ADSR/volume sweeps, panning and reverb. Moving the voices there takes it off
the EE entirely.

## Constraints found

* **Sample data does not fit in SPU2 RAM.** sfx.tbl is 0x4c2160 bytes
  (4.76 MiB) and seq.tbl (instruments) is 0x17c070 bytes (1.48 MiB) of VADPCM.
  As PS-ADPCM (16 bytes per 28 samples against 9 bytes per 16) that is about
  6.3 MiB. SPU2 RAM is 2 MiB, minus the reverb work area and the PCM stream
  buffers. A sample cache in SPU2 RAM is therefore required. The N64 had the
  same problem and solved it with its audio DMA cache. Instruments for the
  current song plus recently used effects fit.
* **audsrv is not enough.** Its ADPCM API loads a VAG and plays it on a channel
  with volume and pan. There is no per-voice pitch, envelope, key-off or reverb
  send. A project-owned IRX on top of libsd is needed:
  * `sceSdSetParam` for pitch, volume and ADSR
  * `sceSdSetAddr` for start and loop addresses
  * KON/KOFF to start and stop voices
  * `sceSdVoiceTrans` for uploads
  * the effect attributes for reverb
* **Transcoding cost.** Encoding PS-ADPCM on the EE costs tens of cycles per
  sample, so transcoding a sample when it is first played would cause hitches.
  Samples must be transcoded ahead of time. Two options:
  * once per ROM, by a host tool or a first-boot pass, into a bank file next
    to the ELF
  * streamed by the IOP straight from that file into SPU2 RAM, so the EE never
    touches sample data

## Mapping

| n_audio (N_PVoice)               | SPU2 voice |
|----------------------------------|------------|
| start voice (wavetable, pitch)   | cache lookup/upload, SSA/LSAX, pitch, KON |
| `rs_ratio` pitch                 | PITCH = ratio × 4096 × (sample rate / 48000) |
| envelope target/rate (linear)    | volume sweep (linear increase/decrease) |
| pan, dry/wet (`voldry`/`volwet`) | VOLL/VOLR, reverb enable per voice + effect volume |
| VADPCM loop start/end, count −1  | PS-ADPCM LOOP_START / END + REPEAT flags |
| finite loop count (rare)         | software fallback voice |
| stop / release                   | KOFF (ADSR release) |

Voice commands are batched per audio frame into one SIF RPC to the IRX.

## Using it

1. Build the bank once per ROM on a PC:

       cc -O2 -Iport/ps2 tools/ps2/pd_spu2bank.c port/ps2/spu2_adpcm.c -lm -o pd_spu2bank
       ./pd_spu2bank pd.ntsc-final.z64 pd.pssb --verify

   NTSC final gives 961 samples, 7.7 MiB of PS-ADPCM (138 looped), in
   about two seconds.
2. Copy `pd.pssb` next to the ROM (for example `mass:PDPS2/`).
3. Start the game. The log shows `SPU2 voices: on, 961 samples ...`.
   `Audio.HardwareVoices=0` in the config turns it off; without the file
   the software mixer is used as before.

## How it works

* **Bank** (`tools/ps2/pd_spu2bank.c`, `port/ps2/spu2_bank.h`): every
  wavetable reachable from sfx.ctl and seq.ctl is decoded and re-encoded as
  PS-ADPCM. Entries are keyed by (source, offset in the .tbl), which is what
  the runtime `ALWaveTable.base` points at.
  * Infinite loops are made block-exact: silence is prepended so the loop
    start lands on a 28-sample block, and the loop body is repeated until
    its length is a whole number of blocks (58 samples need this).
  * Finite loop counts are unrolled.
  * Round-trip SNR against the VADPCM decode is 25 dB median and 17 dB
    worst (noisy effects).
* **IOP module** (`port/ps2/iop/pdsnd`, built with the PS2SDK IOP toolchain
  and embedded in the ELF):
  * Streams samples from the bank file on the storage device into an LRU
    cache in SPU2 RAM (0x10000-0x1e0000). Slots are pinned while a voice
    plays them.
  * Applies voice commands: KEYON/KEYOFF, pitch, and linear volume ramps.
    The ramps are the N64 envelope segments, run in 1 ms steps by a
    high-priority IOP thread.
  * Uses voices 0-23 on core 0 (routed through core 1's external input)
    and 24-47 on core 1. audsrv keeps core 1's sound-data input for the PCM
    stream.
* **EE** (`port/ps2/spu2_voices_ps2.c`, hooks in `n_env.c` and
  `n_synthesizer.c`):
  * The envelope mixer translates its parameter events into voice commands
    instead of RSP-style DSP commands.
  * Commands are sent once per audio frame with a non-blocking SIF RPC.
  * Volumes are scaled to match the software mix's level (audsrv feeds
    core 1 at -6 dB).

Measured in PCSX2 (intro, heaviest 5 s windows): audio work drops from about
9.0 to 4.8 ms per game frame, and frames from 42.5 to 37.9 ms. The rest of
the audio cost is MP3 speech decoding and the bus mixing that is still done
on the EE. Per-second loudness of a 40 s capture matches the software mixer
to within about 15 %.

Known differences from the software mixer:

* Reverb is the SPU2's own (core 1 hall preset, `Audio.HardwareReverb` /
  `Audio.HardwareReverbDepth`), not PD's per-stage software reverb. A voice
  sends to it when its FX amount is significant. An SPU2 voice has one level
  for dry and send, so it plays at dry + 1/4 wet. Over the 40 s capture the
  loudness is 0.97x the software mixer's.
* PD's per-voice low-pass filter (`fx`, used for muffled sounds) is not
  applied.
* Parameter changes take effect at audio-frame granularity (184 samples,
  8 ms) rather than sample-exact.

## Stages

1. Done: sample codecs (`port/ps2/spu2_adpcm.[ch]`, host-tested in CI).
2. Done: bank transcoder.
3. Done: IRX with SPU2 RAM cache, streaming, voice commands and ramps.
4. Done: EE voice backend, enabled by the presence of `pd.pssb`.
5. Done: SPU2 reverb; the software aux buses skip their reverb and mixes.
6. Next: listening tests on hardware.
