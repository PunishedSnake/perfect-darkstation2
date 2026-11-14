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

## Stages

1. **Done (this round):** sample codecs `port/ps2/spu2_adpcm.[ch]`. This is a
   VADPCM decoder matching the mixer's scalar decoder, plus a PS-ADPCM
   encoder/decoder with loop flags. Host tests run in CI; PS-ADPCM
   round-trip SNR is 31.5 dB on a noisy sweep.
2. Bank transcoder: parse sfx.ctl/seq.ctl wavetables and write a PS-ADPCM
   bank file with an index (wavetable → offset, length, loop block, rate).
3. IRX: SPU2 RAM cache allocator, uploads from the bank file, voice command
   RPC, reverb setup.
4. EE side: a voice backend for the n_audio synth behind a runtime option,
   with the software mixer kept as the fallback and reference.
5. A/B listening tests on hardware, then make it the default.
