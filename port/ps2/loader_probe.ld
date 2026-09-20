OUTPUT_ARCH(mips)
ENTRY(_start)

SECTIONS
{
    . = 0x00100000;

    .text ALIGN(128) : {
        KEEP(*(.text.start))
        *(.text*)
    }

    .rodata ALIGN(16) : {
        *(.rodata*)
        KEEP(*(.probe_payload))
    }

    .data ALIGN(16) : {
        *(.data*)
        *(.sdata*)
    }

    .bss ALIGN(16) (NOLOAD) : {
        *(.sbss*)
        *(.bss*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.comment)
        *(.pdr)
        *(.mdebug*)
    }
}
