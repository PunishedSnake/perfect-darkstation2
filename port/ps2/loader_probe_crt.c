#include <tamtypes.h>

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /*
     * Blue: the normal current-PS2SDK crt0/newlib startup reached main().
     * Direct GS BGCOLOR avoids gsKit, DMA, SIF, filesystem and input.
     */
    *(volatile u64 *)0x120000e0 = 0x00ff0000u;

    for (;;) {
        __asm__ __volatile__("nop");
    }
}
