# PS2 bring-up diagnostics and PCSX2 notes

## Diagnostic build options (all OFF by default)

| CMake option | Effect |
| --- | --- |
| `PD_PS2_SCREEN_LOG=ON` | Shows every startup log line on the TV through PS2SDK's debug text screen from `sysInit()` until `videoInit()`. Needs no IOP service, so hangs and fatal errors during IOP reset, storage and ROM startup stay readable. |
| `PD_PS2_SIO_LOG=ON` | Mirrors every log line to the EE SIO TX FIFO. PCSX2 prints it as EE console output; it survives IOP resets. |
| `PD_PS2_ALLOW_HOST_BOOT=ON` | Accepts `host:` as the boot medium so PCSX2 can run past storage setup (PCSX2 has no USB mass-storage device). Never for hardware builds. |

## Running the game in PCSX2 (standard PS2 BIOS)

Verified with PCSX2 2.8.0 and a 1.70J BIOS:

```sh
cmake -S port/ps2 -B build-ps2-pcsx2 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/port/ps2/ps2dev-toolchain.cmake" \
  -DPD_PS2_SIO_LOG=ON -DPD_PS2_ALLOW_HOST_BOOT=ON
```

Put `pd-ps2-game.elf` and `pd.ntsc-final.z64` in one folder and boot the ELF
with `-elf`. `PCSX2.ini` needs `[EmuCore] HostFs = true` (the key is
`HostFs`, not `EnableHostFs`) and `[Logging] EnableEEConsole = true`; headless
runs also need `[SPU2/Output] Backend = Null`, or a modal audio error dialog
stalls emulation.

## Owned IOP runtime and module presence

After the owned clean IOP reboot the resident LOADFILE is the boot-ROM one.
On older consoles (1.70J: "Load File service.(99/11/05)") it does not
implement `LF_F_SEARCH_MOD_BY_NAME`, and `SifSearchModuleByName()` returns 0
for modules that were never loaded. PAD and memory-card setup treated that as
"resident", skipped SIO2MAN/PADMAN, and `padInit()` waited forever: a black
screen before video init. Module presence now comes from
`iop_registry_ps2.h`, filled by the game's own loads since the reboot.

## PCSX2 and the PSX (DESR) BIOS

`ps2-0180j-20031028` (`ROMVER 0180JC20031028`) is a PSX DESR BIOS: its ROM
directory carries `PSXVER`, `DVRDRV`, `DVRMISC`, `XATAPI` and `XFROMMAN`.
PCSX2 2.6.3 and 2.8.0 do not support it:

- The EE kernel stalls at `# Initialize Start.` polling DVE status
  `0x1A000006` bit 1 after a DVE command in mode `0x44`; PCSX2's DVE stub only
  completes modes `0x42`/`0x43`. A one-byte patch of PCSX2 2.8.0 (set the
  "command executing" flag instead of clearing it, file offset `0x54916b`,
  `0x00` -> `0x01`) gets past kernel init.
- Even then, a minimal PS2SDK hello-world faults before `main()` with a kernel
  TLB miss in the kernel's `memcpy`, with every PS2SDK kernel patch disabled.

Emulator results with a PSX BIOS are therefore not evidence about PSX
hardware. Use a standard PS2 BIOS dump for PCSX2 debugging, and
`PD_PS2_SCREEN_LOG=ON` builds for the PSX itself.
