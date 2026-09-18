# Perfect DarkStation 2 documentation

This directory contains the technical documentation for the PlayStation 2 port.

The project has accumulated detailed bring-up notes and audits during development. To keep those useful without making every old observation look like current truth, documents are grouped below by how they should be used.

## Start here

For active development, read these in order:

1. [PS2 development guide](PS2_DEVELOPMENT.md) - current branch model, build/test loop, debugging workflow and active priorities.
2. [PS2 port quick start](../port/ps2/README.md) - build commands, runtime files, command-line options and hardware handoff.
3. [Native renderer architecture](PS2_NATIVE_RENDERER_ARCHITECTURE.md) - current Fast3D/RDP -> GS/VU1 design.
4. [Renderer trace](PS2_RENDERER_TRACE.md) - capture and decode a real-hardware renderer frame.
5. [Renderer correctness audit](PS2_RENDERER_CORRECTNESS_AUDIT.md) - confirmed contracts and currently open correctness gaps.
6. [Optimization roadmap](PS2_OPTIMIZATION_ROADMAP.md) - performance work after correctness is preserved.

The repository-level [CONTRIBUTING.md](../CONTRIBUTING.md) describes the expected development and pull-request workflow.

## Living documents

These should describe the current implementation and should be updated when the relevant contract changes:

| Document | Owner / purpose |
| --- | --- |
| [PS2_DEVELOPMENT.md](PS2_DEVELOPMENT.md) | Canonical development workflow and current priorities. |
| [PS2_NATIVE_RENDERER_ARCHITECTURE.md](PS2_NATIVE_RENDERER_ARCHITECTURE.md) | Renderer ownership, GS state, VU1/PATH1 and PATH3 architecture. |
| [PS2_RENDERER_TRACE.md](PS2_RENDERER_TRACE.md) | Real-hardware trace capture and decoder format. |
| [PS2_RENDERER_CORRECTNESS_AUDIT.md](PS2_RENDERER_CORRECTNESS_AUDIT.md) | Current renderer-contract findings and open correctness work. |
| [PS2_OPTIMIZATION_ROADMAP.md](PS2_OPTIMIZATION_ROADMAP.md) | Ranked optimization work and measurement rules. |
| [PS2_WHOLE_RUNTIME_OPTIMIZATION_AUDIT.md](PS2_WHOLE_RUNTIME_OPTIMIZATION_AUDIT.md) | Non-render runtime performance work. |
| [N64_RDP_TMEM_SEMANTICS.md](N64_RDP_TMEM_SEMANTICS.md) | Texture/TMEM semantics used by the PS2 backend. |

## Reference and historical audits

These remain useful, but dated observations inside them are evidence from a particular bring-up stage rather than the current project frontier:

| Document | Use it for |
| --- | --- |
| [PS2_STARTUP_CHAIN.md](PS2_STARTUP_CHAIN.md) | Detailed runtime call chain, historical hardware checkpoints and failure interpretation. |
| [PS2_CODE_AND_FILE_AUDIT.md](PS2_CODE_AND_FILE_AUDIT.md) | Build ownership, cleanup policy and the 2026-09 repository audit. |
| [PS2_LIBRARY_AUDIT.md](PS2_LIBRARY_AUDIT.md) | Linked-library footprint and dependency decisions at audit time. |
| [PS2_SM64_PORT_COMPARISON.md](PS2_SM64_PORT_COMPARISON.md) | Techniques compared with the SM64 PS2 port and their applicability here. |
| [PS2_MODERN_OPTIMIZATION_AUDIT.md](PS2_MODERN_OPTIMIZATION_AUDIT.md) | Broader optimization review and rejected shortcuts. |
| [PS2_PORT_AUDIT_2026-09-18.md](PS2_PORT_AUDIT_2026-09-18.md) | Dated repository, CI and high-level renderer/dataflow audit after the menu/mission-loading milestone. |

Historical sections should normally be preserved when they explain why a design decision exists. Add a current-status note rather than deleting useful hardware evidence.

## Diagnostic documents

Hardware-isolation procedures live beside the PS2 backend:

- [Prototype / bootstrap hardware test](../port/ps2/PROTOTYPE_TEST.md)
- [VU1 colour diagnostic](../port/ps2/VU1_COLOR_DIAGNOSTIC.md)

These describe deliberately narrow diagnostic executables. They are not the normal game runtime.

## Documentation rules

To keep this directory from becoming archaeology with Markdown syntax:

- Put the current developer-facing truth in a living document.
- Date hardware observations and identify the tested build/commit when available.
- Separate **confirmed defects**, **current implementation**, and **hypotheses to test**.
- Do not promote an emulator-only observation to a PS2 hardware conclusion.
- When a milestone is superseded, retain it as history only if it explains a design or regression.
- Link to one canonical explanation instead of copying the same status paragraph into several files.
- Update [PS2_DEVELOPMENT.md](PS2_DEVELOPMENT.md) whenever the main blocker or accepted hardware frontier changes.
