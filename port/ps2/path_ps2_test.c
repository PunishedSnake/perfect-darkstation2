#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "path_ps2.h"

int main(void)
{
    char mass0Path[] = "mass0:/APPS/PD/pd-ps2-game.elf";
    assert(ps2PathCanonicalizeMass0ToLegacy(mass0Path));
    assert(!strcmp(mass0Path, "mass:/APPS/PD/pd-ps2-game.elf"));

    char legacyMassPath[] = "mass:/APPS/PD/pd-ps2-game.elf";
    assert(!ps2PathCanonicalizeMass0ToLegacy(legacyMassPath));
    assert(!strcmp(legacyMassPath, "mass:/APPS/PD/pd-ps2-game.elf"));

    char mass1Path[] = "mass1:/APPS/PD/pd-ps2-game.elf";
    assert(!ps2PathCanonicalizeMass0ToLegacy(mass1Path));
    assert(!strcmp(mass1Path, "mass1:/APPS/PD/pd-ps2-game.elf"));

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
