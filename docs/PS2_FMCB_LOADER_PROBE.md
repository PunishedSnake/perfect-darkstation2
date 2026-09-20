# FMCB loader probe

These three ELFs isolate a launch failure before the Perfect Dark runtime.

Run them directly from the same FMCB menu/path that fails to start
`pd-ps2-game.elf`.

| ELF | Expected screen | What it proves |
| --- | --- | --- |
| `pd-ps2-loader-probe-raw.elf` | green | the launcher loaded an ELF and `ExecPS2` reached a raw entry point |
| `pd-ps2-loader-probe-crt.elf` | blue | current PS2SDK crt0/newlib startup reached `main()` |
| `pd-ps2-loader-probe-large.elf` | magenta | a roughly Perfect-Dark-sized `PT_LOAD` also reaches its raw entry point |

The raw probes contain no gsKit, SIF, USB, filesystem, pad, audio or game code.
The large probe intentionally carries about 2.9 MiB of file-backed payload plus
about 0.65 MiB of BSS so it exercises loader memory behaviour without exercising
Perfect Dark startup.

Interpretation:

- raw black: failure is before our code, in the launcher/load/ExecPS2 boundary.
- raw green, CRT black: investigate CRT/ABI/startup-state compatibility.
- raw green, CRT blue, large black: investigate loader staging/load-image size
  or overlap.
- all three colours work while Perfect Dark stays black: the loader enters the
  application correctly; instrument the first instructions/runtime startup next.

These probes are diagnostics only and are not release executables.
