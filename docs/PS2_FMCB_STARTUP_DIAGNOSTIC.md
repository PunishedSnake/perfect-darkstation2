# Perfect Dark PS2 FMCB startup diagnostic

This is a real-game diagnostic build. It is not a release build.

The executable is `pd-ps2-game-fmcb-startup.elf`. It runs the normal current
Perfect Dark PS2 runtime, but writes a direct GS BGCOLOR checkpoint before and
after early startup boundaries. Each pre-video checkpoint is held by a pure EE
busy-loop, so observing it does not depend on SIF, IOP, timers, filesystem,
gsKit, or ThreadMan sleeps.

Run this ELF directly through the launcher path that fails to start the normal
game. Note the last clearly visible colour before a hang, reset, or transition.

| Colour | Startup boundary reached | If it stops here |
| --- | --- | --- |
| Red | full-game `main()` entered | CRT reached the real application |
| Orange | `sysInitArgs()` returned | next operation is the first `sysArgCheck()` |
| Yellow | first argument scan returned | next operation is `crashInit()`, unless disabled |
| Lime | crash handler init returned or was skipped | next operation is `sysInit()` |
| Green | `sysInit()` returned | next operation is mass-storage/SIF bootstrap |
| Cyan | storage bootstrap returned | next operations are filesystem/config init |
| Light blue | filesystem and config init completed | next operation is controller/input init |
| Blue | `inputInit()` returned | next operation is audio init |
| Violet | `audioInit()` returned | next operation is ROM-data init |
| Magenta | ROM init/check completed | next operation is `videoInit()` |
| White | `videoInit()` returned | game-side initialization continues |
| Grey | final startup/argument parsing completed | next operation is `mainProc()` |

The markers after `videoInit()` are secondary evidence: once the renderer owns
the GS display circuits, BGCOLOR visibility depends on the configured PMODE and
framebuffer. The red-through-magenta markers are the authoritative launch-path
checkpoints.

Interpret a reset between two colours as a failure in the operation named by
the earlier colour. For example, orange followed by a reset before yellow
isolates the first scan of launcher-supplied argv.

This diagnostic intentionally adds visible stalls and must never be used for
performance measurements or releases.
