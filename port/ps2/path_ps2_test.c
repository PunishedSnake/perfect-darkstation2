#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "path_ps2.h"

int main(void)
{
    char mass0Path[64] = "mass0:/APPS/PD/pd-ps2-game.elf";
    assert(ps2PathCanonicalizeUsbMassToLegacy(mass0Path, sizeof(mass0Path)));
    assert(!strcmp(mass0Path, "mass:/APPS/PD/pd-ps2-game.elf"));

    char usb0Path[64] = "usb0:/APPS/PD/pd-ps2-game.elf";
    assert(ps2PathCanonicalizeUsbMassToLegacy(usb0Path, sizeof(usb0Path)));
    assert(!strcmp(usb0Path, "mass:/APPS/PD/pd-ps2-game.elf"));

    char usbPath[64] = "usb:/APPS/PD/pd-ps2-game.elf";
    assert(ps2PathCanonicalizeUsbMassToLegacy(usbPath, sizeof(usbPath)));
    assert(!strcmp(usbPath, "mass:/APPS/PD/pd-ps2-game.elf"));

    char legacyMassPath[64] = "mass:/APPS/PD/pd-ps2-game.elf";
    assert(!ps2PathCanonicalizeUsbMassToLegacy(
        legacyMassPath, sizeof(legacyMassPath)));
    assert(!strcmp(legacyMassPath, "mass:/APPS/PD/pd-ps2-game.elf"));

    char mass1Path[64] = "mass1:/APPS/PD/pd-ps2-game.elf";
    assert(!ps2PathCanonicalizeUsbMassToLegacy(mass1Path, sizeof(mass1Path)));
    assert(!strcmp(mass1Path, "mass1:/APPS/PD/pd-ps2-game.elf"));

    char usb1Path[64] = "usb1:/APPS/PD/pd-ps2-game.elf";
    assert(!ps2PathCanonicalizeUsbMassToLegacy(usb1Path, sizeof(usb1Path)));
    assert(!strcmp(usb1Path, "usb1:/APPS/PD/pd-ps2-game.elf"));

    char shortUsb[5] = "usb:";
    assert(!ps2PathCanonicalizeUsbMassToLegacy(shortUsb, sizeof(shortUsb)));
    assert(!strcmp(shortUsb, "usb:"));

    assert(ps2PathHasDevicePrefix("mass:PDPS2/pd.ntsc-final.z64"));
    assert(ps2PathHasDevicePrefix("mass0:/PDPS2/pd.ntsc-final.z64"));
    assert(ps2PathHasDevicePrefix("mc0:/BESLES-XXX/save"));
    assert(ps2PathHasDevicePrefix("hdd0:+PARTITION"));
    assert(ps2PathHasDevicePrefix("pfs0:/data"));
    assert(ps2PathHasDevicePrefix("host:pd.elf"));
    assert(ps2PathHasDevicePrefix("rom0:ROMVER"));

    assert(!ps2PathHasDevicePrefix(NULL));
    assert(!ps2PathHasDevicePrefix(""));
    assert(!ps2PathHasDevicePrefix(":invalid"));
    assert(!ps2PathHasDevicePrefix("./relative"));
    assert(!ps2PathHasDevicePrefix("../relative"));
    assert(!ps2PathHasDevicePrefix("directory/file"));
    assert(!ps2PathHasDevicePrefix("mass/path:late"));

    return 0;
}
