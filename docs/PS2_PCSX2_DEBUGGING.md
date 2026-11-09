# PS2 bring-up diagnostics and PCSX2 notes

## Diagnostic build options (all OFF by default)

| CMake option | Effect |
| --- | --- |
| `PD_PS2_SCREEN_LOG=ON` | Shows every startup log line on the TV through PS2SDK's debug text screen from `sysInit()` until `videoInit()`. Needs no IOP service, so hangs and fatal errors during IOP reset, storage and ROM startup stay readable. |
| `PD_PS2_SIO_LOG=ON` | Mirrors every log line to the EE SIO TX FIFO. PCSX2 prints it as EE console output; it survives IOP resets. |

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
