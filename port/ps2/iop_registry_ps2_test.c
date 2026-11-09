#include "iop_registry_ps2.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    ps2IopRegistryReset();
    /* Nothing is resident after a clean reboot, including "id 0". */
    assert(ps2IopRegistryFind("sio2man") == -1);
    assert(ps2IopRegistryFind(NULL) == -1);

    assert(ps2IopRegistryRecord("sio2man", 0) == 0);
    assert(ps2IopRegistryRecord("padman", 34) == 0);
    assert(ps2IopRegistryFind("sio2man") == 0);
    assert(ps2IopRegistryFind("padman") == 34);
    assert(ps2IopRegistryFind("pad") == -1);
    assert(ps2IopRegistryFind("padman2") == -1);

    /* Re-recording updates; invalid records are refused. */
    assert(ps2IopRegistryRecord("padman", 35) == 0);
    assert(ps2IopRegistryFind("padman") == 35);
    assert(ps2IopRegistryRecord("audsrv", -203) == -1);
    assert(ps2IopRegistryFind("audsrv") == -1);
    assert(ps2IopRegistryRecord("", 1) == -1);
    assert(ps2IopRegistryRecord(NULL, 1) == -1);
    assert(ps2IopRegistryRecord("this_name_is_far_too_long_for_it", 1) == -1);

    /* Capacity is bounded. */
    char name[8];
    for (unsigned int i = 0u; i < PS2_IOP_REGISTRY_CAPACITY + 4u; ++i) {
        snprintf(name, sizeof(name), "m%u", i);
        (void)ps2IopRegistryRecord(name, (int)i);
    }
    assert(ps2IopRegistryFind("m0") == 0);
    assert(ps2IopRegistryFind("m19") == -1);

    ps2IopRegistryReset();
    assert(ps2IopRegistryFind("padman") == -1);

    puts("PS2 IOP module registry tests passed");
    return 0;
}
