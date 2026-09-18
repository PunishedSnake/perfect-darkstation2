# Contributing to Perfect DarkStation 2

Thanks for helping with the PS2 port.

The project is in active hardware bring-up and renderer-correctness development. Small, testable changes are much easier to validate than large rewrites, especially when the final judge is a retail PlayStation 2 with the conversational skills of a brick.

## Development branch

Use `ps2` as the base branch for PS2 work.

```sh
git fetch origin
git switch ps2
git pull --ff-only
git switch -c <topic-branch>
```

Pull requests for the PS2 port should target `ps2`.

The inherited `port` branch is retained as a comparison/upstream baseline and is not the target for normal PS2 development.

## Before changing code

Read:

- [PS2 development guide](docs/PS2_DEVELOPMENT.md)
- [PS2 quick start](port/ps2/README.md)
- [Native renderer architecture](docs/PS2_NATIVE_RENDERER_ARCHITECTURE.md) for graphics work
- [Renderer correctness audit](docs/PS2_RENDERER_CORRECTNESS_AUDIT.md) for visual/state work

## Change discipline

Prefer one independently testable contract per commit.

For hardware-sensitive work, avoid mixing unrelated changes such as:

- renderer state plus filesystem changes;
- VU1 transport plus material equations;
- allocator changes plus optimization flags;
- correctness fixes plus broad cleanup.

A small commit that can be bisected on hardware is more useful than an elegant mega-refactor whose failure mode is "screen became cursed".

## Testing expectations

Run the narrowest relevant host tests and make sure the PS2 game target still links.

For changes involving any of the following, retail-hardware validation is strongly preferred before calling the change complete:

- VIF1/VU1 ownership;
- PATH1/PATH3 ordering;
- GS state and presentation;
- DMA synchronization;
- filesystem timing;
- SPU2/SIF behaviour;
- visual renderer correctness;
- performance claims.

Emulator results are useful evidence, but they do not replace retail-hardware validation for timing and DMA/VU/GS behaviour.

## ROM and assets

Do not commit or distribute copyrighted Perfect Dark ROM images or extracted proprietary game assets.

Development expects a legally obtained NTSC-final / US v1.1 ROM supplied by the developer at runtime.

## Logs and traces

Do not commit generated runtime state such as:

- `pd.ini`;
- `eeprom.bin`;
- `pdps2.log`;
- `pdps2-gs-trace.bin`;
- downloaded CI artifacts.

For renderer bugs, prefer the binary trace workflow documented in [PS2_RENDERER_TRACE.md](docs/PS2_RENDERER_TRACE.md).

## Documentation

Update documentation in the same change when you alter a documented contract.

Use [docs/README.md](docs/README.md) to choose the correct document. Do not copy a new "current status" paragraph into every audit file. The canonical project/development frontier belongs in [PS2_DEVELOPMENT.md](docs/PS2_DEVELOPMENT.md).

When recording a hypothesis, label it as a hypothesis. When hardware proves or disproves it, update the label instead of quietly rewriting history.

## Pull requests

A useful pull request description should include:

- what contract or bug is being changed;
- why the change is needed;
- host tests run;
- PS2 build status;
- hardware result when applicable;
- affected scene/stage;
- screenshots, trace or log evidence for visual/runtime changes;
- known regressions or remaining uncertainty.

Performance pull requests should compare the same ROM, scene, logging mode and build profile and should report measured results rather than impressions.
