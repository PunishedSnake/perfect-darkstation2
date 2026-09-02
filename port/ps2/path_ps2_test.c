#include <assert.h>
#include <stddef.h>

#include "path_ps2.h"

int main(void)
{
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
