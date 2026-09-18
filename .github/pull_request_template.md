## Summary

Describe the PS2-specific change and the contract it is intended to fix or improve.

## Scope

- [ ] Renderer correctness
- [ ] VIF1 / VU1 / PATH1
- [ ] PATH3 / GS state
- [ ] Texture / TMEM / combiner
- [ ] Framebuffer / render target
- [ ] Input
- [ ] Audio / SPU2 / SIF
- [ ] ROM / filesystem / asset loading
- [ ] Memory / allocator / display-list safety
- [ ] Performance
- [ ] CI / tooling
- [ ] Documentation

## Validation

### Host

List the relevant tests run and their result.

```text
tests:
result:
```

### PS2 build

- [ ] `pd_ps2_game` links successfully
- [ ] linker map produced
- [ ] no new unresolved symbols
- [ ] no unexpected warnings

Build profile:

- [ ] Og correctness baseline
- [ ] O2 comparison
- [ ] diagnostic/special profile

## Retail-hardware result

Fill this section for hardware-sensitive changes. If not tested on hardware, explain why.

```text
commit:
ELF SHA-256:
PS2SDK/toolchain:
PS2 model:
hardware revision:
loader:
launch device:
ROM:
scene/stage:
sound:
file logging:
active IRX:
video mode:
result:
```

## Renderer evidence

For visual, GS, VU or material changes, attach or describe the relevant evidence:

- screenshot/photo:
- renderer counters:
- `pdps2-gs-trace.bin` / decoded JSON:
- `pdps2.log` if explicitly required:

## Performance evidence

Required only when making a performance claim.

Compare the same ROM, scene, camera path, logging mode and build profile.

```text
baseline:
candidate:
metric:
baseline value:
candidate value:
sample count:
p50 / p95 / p99 / max:
deadline misses:
correctness hash:
regressions:
```

## Known limitations / follow-up

List anything deliberately left unresolved.

## Documentation

- [ ] no documented contract changed
- [ ] relevant living documentation updated
