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
| Cyan | storage bootstrap returned success | next operation is `fsInit()` |
| Dark red | storage bootstrap returned failure/timeout | mass is not usable; next operation is still `fsInit()` |
| Azure | `fsInit()` returned | next operation is first `fsFileSize(CONFIG_PATH)` / `stat()` |
| Violet | first config-file `stat()` returned | next operation is `configInit()` |
| Magenta | `configInit()` returned | next operation may create the default config |
| White | config-save stage completed or was skipped | next operation is final filesystem/config logging |
| Mint | filesystem and config init completed | next operation is controller/input init |
| Blue | `inputInit()` returned | next operation is audio init |
| Violet (later) | `audioInit()` returned | next operation is ROM-data init |
| Magenta (later) | ROM init/check completed | next operation is `videoInit()` |
| White (later) | `videoInit()` returned | game-side initialization continues |
| Grey | final startup/argument parsing completed | next operation is `mainProc()` |

The markers after `videoInit()` are secondary evidence: once the renderer owns
the GS display circuits, BGCOLOR visibility depends on the configured PMODE and
framebuffer. The pre-video markers are the authoritative launch-path checkpoints. The
diagnostic intentionally reuses some colours later in startup; sequence matters,
and the filesystem refinement above occurs immediately after green.

Interpret a reset between two colours as a failure in the operation named by
the earlier colour. For example, orange followed by a reset before yellow
isolates the first scan of launcher-supplied argv.

This diagnostic intentionally adds visible stalls and must never be used for
performance measurements or releases.


## PAD refinement

If the filesystem diagnostic reaches **Mint** and then stops before the main
input marker, the next diagnostic colours are emitted inside `ps2PadInit()`:

| Colour | PAD boundary reached | Next operation |
| --- | --- | --- |
| Dark red | entered `ps2PadInit()` | clear local PAD state |
| Red | `sceSifInitRpc(0)` returned | search resident `sio2man` |
| Orange | `SifSearchModuleByName("sio2man")` returned | reuse/load SIO2 |
| Yellow | SIO2 service/module step completed | search resident `padman` |
| Lime | `SifSearchModuleByName("padman")` returned | reuse/load PADMAN |
| Green | PADMAN service/module step completed | call `padInit(0)` |
| Cyan | `padInit(0)` returned successfully | open controller port 0 |
| Blue | port 0 open attempt returned | open controller port 1 |
| Magenta | port 1 open attempt returned | finish PAD backend init |
| White | `ps2PadInit()` is about to return | first `ps2PadUpdate()` follows |

Current PS2SDK `libpad` can wait indefinitely inside `padInit()` while binding
the expected PAD RPC server. Therefore Green with no Cyan is a particularly
strong signal that the resident PAD module/service and the current EE libpad
client do not agree.
