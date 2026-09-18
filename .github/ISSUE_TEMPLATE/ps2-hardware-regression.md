---
name: PS2 hardware regression
about: Report a runtime, renderer or performance regression observed on PlayStation 2 hardware
title: "[PS2] "
labels: ""
assignees: ""
---

## Summary

Describe the regression in one or two sentences.

## Known-good build

```text
commit:
ELF SHA-256:
artifact/profile:
```

## Regressed build

```text
commit:
ELF SHA-256:
artifact/profile:
```

## Hardware

```text
PS2 model:
loader:
launch device:
video mode/output:
controller:
other relevant hardware:
```

## Runtime setup

```text
ROM:
stage/scene:
sound enabled:
file logging enabled:
pd.ini differences:
eeprom state:
```

## Reproduction

1.
2.
3.

## Observed result

Describe exactly what is visible or what the console does.

- [ ] visual corruption
- [ ] missing geometry/material/effect
- [ ] hang
- [ ] return to OSD/reset
- [ ] fatal hold
- [ ] audio regression
- [ ] input regression
- [ ] performance regression
- [ ] other

## Expected result

Describe the known-good behaviour.

## Evidence

Attach what is relevant:

- photo/screenshot;
- console output;
- renderer counters/snapshot;
- `pdps2-gs-trace.bin` or decoded JSON;
- `pdps2.log` only when the run intentionally used file logging.

Do not upload copyrighted ROM images or extracted proprietary game assets.

## Notes

Include any A/B result, timing, scene-specific behaviour or other observations that may narrow the fault.
