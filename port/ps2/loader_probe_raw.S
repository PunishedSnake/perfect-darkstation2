    .set noreorder
    .set noat

    .section .text.start, "ax", @progbits
    .globl _start
    .ent _start
_start:
    /* Green: ExecPS2 reached our raw entry point. No CRT, libc or SIF. */
    lui     $t0, 0x1200
    li      $t1, 0x0000ff00
    sd      $t1, 0x00e0($t0)
1:
    b       1b
    nop
    .end _start
