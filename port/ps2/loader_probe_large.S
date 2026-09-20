    .set noreorder
    .set noat

    .section .text.start, "ax", @progbits
    .globl _start
    .ent _start
_start:
    /* Magenta: a PD-sized load image reached its raw entry point. */
    lui     $t0, 0x1200
    li      $t1, 0x00ff00ff
    sd      $t1, 0x00e0($t0)
1:
    b       1b
    nop
    .end _start

    /*
     * Match the current game's approximately 2.9 MiB file-backed PT_LOAD and
     * approximately 0.65 MiB zero-fill tail closely enough to expose loaders
     * whose resident staging area collides with larger applications.
     */
    .section .probe_payload, "a", @progbits
    .balign 16
    .fill 0x2c9000, 1, 0x5a

    .section .bss, "aw", @nobits
    .balign 16
    .space 0x0a3000
