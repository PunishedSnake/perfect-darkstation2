/*
 * ee_string.c - memcpy and memset for the R5900 (EE).
 *
 * The ps2sdk versions (weak symbols in libkernel) use 128-bit LQ/SQ only
 * when the destination, and for memcpy also the source, are 16-byte
 * aligned. Anything else is copied or filled one byte per loop iteration:
 * a 1 KiB memset of an 8-byte-aligned struct is 1024 SB stores. In the
 * game, that byte loop was the hottest part of both functions.
 *
 * Here the destination is brought to 16-byte alignment with a short head,
 * and the body always moves 32 bytes per iteration with SQ. A misaligned
 * source is read with LDR/LDL pairs and the two halves are joined with
 * PCPYLD. Copies under 16 bytes move 8 bytes at once with unaligned
 * LDL/LDR and SDL/SDR. These strong definitions take precedence over the
 * weak library ones at link time.
 *
 * EE_STRING_PREFIX renames them for side-by-side tests and benchmarks.
 */

#ifdef EE_STRING_PREFIX
#define EE_STR_SYM(name) "ee_" #name
#else
#define EE_STR_SYM(name) #name
#endif

/* n32/EABI argument registers: $4 dst, $5 src or value, $6 count. Only
 * caller-saved registers are used ($2, $8-$14). */
__asm__(
    "    .text\n"
    "    .set push\n"
    "    .set noat\n"
    "    .set reorder\n"

    "    .globl " EE_STR_SYM(memcpy) "\n"
    "    .type " EE_STR_SYM(memcpy) ", @function\n"
    "    .ent " EE_STR_SYM(memcpy) "\n"
    EE_STR_SYM(memcpy) ":\n"
    "    move    $2, $4\n"
    "    sltiu   $8, $6, 16\n"
    "    bnez    $8, 5f\n"
    /* head: copy bytes until dst is 16-byte aligned */
    "    andi    $9, $4, 15\n"
    "    beqz    $9, 2f\n"
    "    li      $10, 16\n"
    "    subu    $9, $10, $9\n"
    "    subu    $6, $6, $9\n"
    "    sltiu   $8, $9, 8\n"
    "    bnez    $8, 1f\n"
    "    ldr     $10, 0($5)\n"
    "    ldl     $10, 7($5)\n"
    "    addiu   $9, $9, -8\n"
    "    addiu   $5, $5, 8\n"
    "    sdr     $10, 0($4)\n"
    "    sdl     $10, 7($4)\n"
    "    addiu   $4, $4, 8\n"
    "    beqz    $9, 2f\n"
    "1:  lbu     $10, 0($5)\n"
    "    addiu   $9, $9, -1\n"
    "    addiu   $5, $5, 1\n"
    "    sb      $10, 0($4)\n"
    "    addiu   $4, $4, 1\n"
    "    bnez    $9, 1b\n"
    /* body: 32 bytes per iteration, then one more 16 if bit 4 is set */
    "2:  srl     $11, $6, 5\n"
    "    andi    $9, $5, 15\n"
    "    bnez    $9, 4f\n"
    /* aligned source */
    "    beqz    $11, 31f\n"
    "3:  lq      $10, 0($5)\n"
    "    lq      $12, 16($5)\n"
    "    addiu   $11, $11, -1\n"
    "    addiu   $5, $5, 32\n"
    "    sq      $10, 0($4)\n"
    "    sq      $12, 16($4)\n"
    "    addiu   $4, $4, 32\n"
    "    bnez    $11, 3b\n"
    "31: andi    $9, $6, 16\n"
    "    andi    $6, $6, 15\n"
    "    beqz    $9, 5f\n"
    "    lq      $10, 0($5)\n"
    "    addiu   $5, $5, 16\n"
    "    sq      $10, 0($4)\n"
    "    addiu   $4, $4, 16\n"
    "    b       5f\n"
    /* misaligned source: unaligned doubleword pairs joined into QWs */
    "4:  beqz    $11, 41f\n"
    "40: ldr     $10, 0($5)\n"
    "    ldl     $10, 7($5)\n"
    "    ldr     $12, 8($5)\n"
    "    ldl     $12, 15($5)\n"
    "    ldr     $13, 16($5)\n"
    "    ldl     $13, 23($5)\n"
    "    ldr     $14, 24($5)\n"
    "    ldl     $14, 31($5)\n"
    "    pcpyld  $10, $12, $10\n"
    "    pcpyld  $13, $14, $13\n"
    "    addiu   $11, $11, -1\n"
    "    addiu   $5, $5, 32\n"
    "    sq      $10, 0($4)\n"
    "    sq      $13, 16($4)\n"
    "    addiu   $4, $4, 32\n"
    "    bnez    $11, 40b\n"
    "41: andi    $9, $6, 16\n"
    "    andi    $6, $6, 15\n"
    "    beqz    $9, 5f\n"
    "    ldr     $10, 0($5)\n"
    "    ldl     $10, 7($5)\n"
    "    ldr     $12, 8($5)\n"
    "    ldl     $12, 15($5)\n"
    "    pcpyld  $10, $12, $10\n"
    "    addiu   $5, $5, 16\n"
    "    sq      $10, 0($4)\n"
    "    addiu   $4, $4, 16\n"
    /* tail (and small copies): one unaligned doubleword, then bytes */
    "5:  sltiu   $8, $6, 8\n"
    "    bnez    $8, 6f\n"
    "    ldr     $10, 0($5)\n"
    "    ldl     $10, 7($5)\n"
    "    addiu   $6, $6, -8\n"
    "    addiu   $5, $5, 8\n"
    "    sdr     $10, 0($4)\n"
    "    sdl     $10, 7($4)\n"
    "    addiu   $4, $4, 8\n"
    "6:  beqz    $6, 8f\n"
    "7:  lbu     $10, 0($5)\n"
    "    addiu   $6, $6, -1\n"
    "    addiu   $5, $5, 1\n"
    "    sb      $10, 0($4)\n"
    "    addiu   $4, $4, 1\n"
    "    bnez    $6, 7b\n"
    "8:  jr      $31\n"
    "    .end " EE_STR_SYM(memcpy) "\n"
    "    .size " EE_STR_SYM(memcpy) ", .-" EE_STR_SYM(memcpy) "\n"

    "    .globl " EE_STR_SYM(memset) "\n"
    "    .type " EE_STR_SYM(memset) ", @function\n"
    "    .ent " EE_STR_SYM(memset) "\n"
    EE_STR_SYM(memset) ":\n"
    "    move    $2, $4\n"
    "    andi    $5, $5, 0xff\n"
    /* replicate the byte across 128 bits */
    "    dsll    $9, $5, 8\n"
    "    or      $5, $5, $9\n"
    "    dsll    $9, $5, 16\n"
    "    or      $5, $5, $9\n"
    "    dsll32  $9, $5, 0\n"
    "    or      $5, $5, $9\n"
    "    pcpyld  $5, $5, $5\n"
    "    sltiu   $8, $6, 16\n"
    "    bnez    $8, 5f\n"
    "    andi    $9, $4, 15\n"
    "    beqz    $9, 2f\n"
    /* head: up to 15 bytes, 8 at a time when possible */
    "    li      $10, 16\n"
    "    subu    $9, $10, $9\n"
    "    subu    $6, $6, $9\n"
    "    sltiu   $8, $9, 8\n"
    "    bnez    $8, 1f\n"
    "    sdr     $5, 0($4)\n"
    "    sdl     $5, 7($4)\n"
    "    addiu   $4, $4, 8\n"
    "    addiu   $9, $9, -8\n"
    "    beqz    $9, 2f\n"
    "1:  sb      $5, 0($4)\n"
    "    addiu   $9, $9, -1\n"
    "    addiu   $4, $4, 1\n"
    "    bnez    $9, 1b\n"
    "2:  srl     $11, $6, 5\n"
    "    beqz    $11, 31f\n"
    "3:  sq      $5, 0($4)\n"
    "    sq      $5, 16($4)\n"
    "    addiu   $11, $11, -1\n"
    "    addiu   $4, $4, 32\n"
    "    bnez    $11, 3b\n"
    "31: andi    $9, $6, 16\n"
    "    andi    $6, $6, 15\n"
    "    beqz    $9, 5f\n"
    "    sq      $5, 0($4)\n"
    "    addiu   $4, $4, 16\n"
    "5:  sltiu   $8, $6, 8\n"
    "    bnez    $8, 6f\n"
    "    sdr     $5, 0($4)\n"
    "    sdl     $5, 7($4)\n"
    "    addiu   $6, $6, -8\n"
    "    addiu   $4, $4, 8\n"
    "6:  beqz    $6, 8f\n"
    "7:  sb      $5, 0($4)\n"
    "    addiu   $6, $6, -1\n"
    "    addiu   $4, $4, 1\n"
    "    bnez    $6, 7b\n"
    "8:  jr      $31\n"
    "    .end " EE_STR_SYM(memset) "\n"
    "    .size " EE_STR_SYM(memset) ", .-" EE_STR_SYM(memset) "\n"
    "    .set pop\n");
