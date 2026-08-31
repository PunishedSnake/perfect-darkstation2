#include <assert.h>
#include <stdint.h>

#include "gs_dma_policy.h"

/* Functional CPCOND0 model, not a test of actual hardware timing. */
static bool completionCondition(uint32_t pcr, uint32_t stat)
{
    return ((stat | ~pcr) & 0x3ffu) == 0x3ffu;
}

int main(void)
{
    const uint32_t gif = 1u << 2;
    const uint32_t vif1 = 1u << 1;
    const uint32_t boot_mask = PS2_GS_BOOTSTRAP_FASTWAIT_CHANNELS;

    assert(boot_mask == gif);
    assert((boot_mask & vif1) == 0u);
    assert(!completionCondition(boot_mask, 0u));
    assert(completionCondition(boot_mask, gif));

    /* Regression: GIF finished, VIF1 idle but never submitted (CIS1=0). */
    assert(!completionCondition(gif | vif1, gif));
    assert(completionCondition(gif | vif1, gif | vif1));

    /* Clearing or completing VIF1 cannot alter a GIF-only bootstrap wait. */
    for (uint32_t stat = 0u; stat < 1024u; ++stat) {
        assert(completionCondition(boot_mask, stat) == ((stat & gif) != 0u));
        assert(completionCondition(boot_mask, stat) ==
            completionCondition(boot_mask, stat ^ vif1));
    }
    return 0;
}
