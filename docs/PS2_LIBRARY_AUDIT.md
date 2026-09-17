# PS2 runtime library audit

This audit records the linked-library cost measured from the `pd-ps2-game.map`
produced by CI run 272 and the replacement policy for the PS2 runtime.  The
numbers are allocatable section contributions, not archive file sizes.

| Component | Measured contribution | Runtime role | Decision |
| --- | ---: | --- | --- |
| `libc.a` | about 125 KiB | C runtime, allocation, formatting and file APIs | Keep Newlib. Replace individual hot calls only when a profile and an explicit accuracy contract justify it. |
| `libstdc++.a` | about 75 KiB | Mostly `std::unordered_map`, `std::map`, `std::list`, allocation and exception support pulled by Fast3D caches | Remove dynamic STL containers from the PS2 build. Use fixed contiguous caches while retaining the portable STL implementation on desktop. |
| `libkernel.a` | about 49 KiB | EE kernel and platform services | Keep. This is a platform contract, not an interchangeable utility library. |
| `libgcc.a` | about 44 KiB | compiler runtime helpers | Keep, then inspect individual helpers in profiles and disassembly. |
| `libz.a` | about 28 KiB | ROM decompression during loading | Keep the bounded streaming API. Benchmark zlib-ng, libdeflate or miniz on R5900 before any replacement. Their advertised SIMD gains do not imply gains on an R5900 without MSA. |
| `libcglue.a` | about 10 KiB | PS2SDK/Newlib integration | Keep. |
| `libcdvd.a` | about 8 KiB | optical media access | Keep. |
| `libpthreadglue.a` | about 5 KiB | threading glue | Keep until the linked call graph proves it removable. |
| `libm.a` | less than 1 KiB | remaining standard math entry points | Keep. The project already supplies targeted PS2 math paths, including hardware square root. A global replacement would add risk for negligible current size. |
| gsKit/dmaKit | platform libraries | GS setup and DMA support | Keep for initialization/control. Performance-critical rendering remains in the custom backend. |
| `audsrv` | platform service | PCM transport to IOP/SPU2 | Keep for compatibility. A custom nonblocking IOP/SPU2 service is a future architectural experiment, not a drop-in library swap. |
| `padx`, patches, kernel | platform services | input and runtime setup | Keep. |

## Implemented changes

- The PS2 texture cache is a fixed contiguous 64-entry LRU instead of an
  `unordered_map` plus `list`.
- The PS2 color-combiner cache is a fixed 256-entry LRU instead of `std::map`.
- The PS2 framebuffer metadata table is fixed at 16 entries instead of
  `std::map`.
- Desktop builds retain their existing STL containers.
- Texture conversion scratch now starts at 64 KiB instead of allocating
  `max_texture_size * max_texture_size * 4` bytes.  The old GS maximum of 1024
  therefore reserved 4 MiB.  The buffer grows only when a real conversion
  request exceeds the initial N64-TMEM-derived budget.
- CI rejects future PS2 map files that pull the removed STL container and
  exception archive members back into the runtime.

## Replacement rules

1. A smaller archive is not automatically a faster runtime.
2. A host SIMD benchmark is not evidence for R5900 performance.
3. Compression candidates must preserve bounded streaming and be benchmarked
   with representative ROM chunks on the PS2 toolchain.
4. Math replacements require a named accuracy and range contract per call
   site; standard symbols are not globally overridden.
5. Platform service replacements require an A/B artifact and hardware traces
   before becoming the default.
