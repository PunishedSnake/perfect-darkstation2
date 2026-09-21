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


## Clean-IOP recovery A/B

Real-hardware evidence now isolates the failing launch path further: the normal
startup reaches the PAD-internal Green marker and never reaches Cyan. Green is
emitted immediately before current PS2SDK `padInit(0)`; Cyan is emitted only
after it returns. Current PS2SDK `libpad` waits indefinitely while binding the
expected PAD RPC server, so this result isolates the hang to the PAD RPC bind/
initialization boundary.

A separate diagnostic target,
`pd-ps2-game-fmcb-clean-iop.elf`, performs one controlled IOP reboot after
EE/system startup but before any game-owned IOP service. It then reinitializes
SIF/LOADFILE and lets the normal storage path rebuild USBD/USBHDFSD before PAD
and audio start.

Additional colours:

| Colour | Meaning |
| --- | --- |
| Purple | clean IOP reboot is about to begin |
| Teal | IOP reboot synchronized and SIF/LOADFILE were reinitialized |
| Dark red after Purple | clean IOP bootstrap failed/timed out |

This is an A/B recovery experiment, not yet the default runtime policy. An IOP
reboot is a system-personality change: inherited modules, heaps, drivers and RPC
bindings are invalidated and every required service must be rebuilt.


## PAD RPC endpoint refinement

Current PS2SDK libpad hides three distinct operations inside `padInit()`:

1. bind PAD RPC endpoint #1 (new 0x80000100 or old 0x8000010f),
2. bind the matching endpoint #2 (0x80000101 or 0x8000011f),
3. issue the PAD INIT RPC for the new protocol.

The diagnostic now exposes the first two binds before calling libpad:

| Last colour | Meaning |
| --- | --- |
| Green | PAD module load returned, but RPC endpoint #1 has not appeared |
| Teal | endpoint #1 is bindable; waiting for endpoint #2 |
| Pink | both PAD RPC endpoints are bindable; any later hang is inside libpad/PAD INIT |
| Cyan | libpad `padInit(0)` returned |

A separate `pd-ps2-game-fmcb-current-pad.elf` build combines the clean IOP A/B
with embedded current-PS2SDK `sio2man.irx` and `padman.irx`. This tests a
matched current server/client stack instead of the ROM XSIO2MAN/XPADMAN pair.


## ROM startup refinement

The current-PAD A/B reaches the later main-level Violet marker. In this
sequence that Violet is emitted only after `audioInit()` returns. Input/PAD
and audio startup have therefore both completed; the next boundary is
`romdataInit()`.

| Colour | ROM boundary reached | Next operation |
| --- | --- | --- |
| Red | entered `romdataInit()` | resolve/open ROM source |
| Orange | ROM file opened and sized | read/validate the 64-byte ROM header |
| Yellow | NTSC-final ROM header validated | read the RZIP 1173 header |
| Lime | RZIP output size validated | allocate decompressed data segment |
| Green | data-segment allocation succeeded | streamed RZIP inflate |
| Cyan | data-segment inflate completed | initialise ROM segments |
| Blue | all ROM segments initialised | build ROM file/name table |
| Magenta | ROM file/name table initialised | release transient data segment |
| White | `romdataInit()` is about to return | GBC ROM probe follows |

A stop on Green specifically isolates the streamed RZIP inflate and its repeated
file-backed reads/decompression.


## ROM source-open refinement

A real-hardware run reached the ROM-level Red marker and did not reach the
previous source-open completion marker. The pre-open path is now split into a
dedicated source-open cycle:

| Colour | Boundary reached |
| --- | --- |
| Red | entered `romdataInit()` |
| Orange | argv/ROM segment policy setup completed; entering `romdataLoadRom()` |
| Yellow | ROM path resolved, prior source closed, about to call `fopen()` |
| Lime | `fopen(path, "rb")` returned a valid `FILE *` |
| Green | `fseek(..., SEEK_END)` returned |
| Cyan | `ftell()` returned a valid 32-bit file size |
| Blue | rewind `fseek(..., SEEK_SET)` returned |
| Magenta | ROM read cache allocated/reset; `romSourceOpenFile()` can return |
| White | caller observed successful `romSourceOpenFile()` |

This deliberately distinguishes a path/setup stall from newlib stdio or the
mass-backed seek/size operations.


## wLaunchELF mass0: -> mass: path-contract A/B

Real hardware now gives a launcher-dependent split with the same current-PAD
diagnostic build:

- direct FMCB hotkey (R1, bypassing OSDSYS) eventually launches the game,
- wLaunchELF R3Z stops at the ROM source-open Yellow marker.

Current wLaunchELF R3Z source explicitly normalises USB execution paths to
`massN:/...` before handing the target its argv path. The classic/direct FMCB
loader path can preserve the configured path without that normalisation.
Current PS2SDK documentation still exposes the legacy USB mass alias
`mass:`.

The `pd-ps2-game-fmcb-mass-alias.elf` A/B therefore makes exactly one
launcher-contract change: if and only if argv[0] begins with `mass0:`, the
executable/home path derived from argv[0] is rewritten to `mass:`.
It does **not** rewrite explicit `--basedir`, `--savedir`, or
`--rom-file` arguments, does not touch `mass1:` or higher units, and does
not change PAD/SIF/storage bootstrap policy.

HIPOTEZA DO TESTU: if this build launched through wLaunchELF passes the final
Yellow ROM-open marker and behaves like the direct R1 path, the incompatibility
is in the launcher-to-runtime path representation rather than the recovered
IOP/PAD stack.


## R3Z usb:/usb0:/mass0: -> mass: refinement

The first path-contract A/B only rewrote `mass0:`. Real hardware feedback
showed that the affected R3Z launch path can describe the USB filesystem using
the `usb:` family instead. Current PS2SDK source for the USBHDFSD/FAT
filesystem registers the IOP filesystem device as `mass`.

That distinction matters after the clean-IOP recovery: any R3Z-owned USB device
alias disappears with the inherited IOP personality, while the project then
loads current PS2SDK `usbd.irx` + `usbhdfsd.irx`, whose filesystem contract
is `mass:`.

The refined `pd-ps2-game-fmcb-usb-alias.elf` maps only unit-zero,
argv[0]-derived launcher paths:

- `usb:/...` -> `mass:/...`
- `usb0:/...` -> `mass:/...`
- `mass0:/...` -> `mass:/...`

It deliberately leaves `usb1:`, `mass1:` and explicit user-supplied paths
unchanged.
