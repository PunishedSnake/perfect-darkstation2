#include "iop_registry_ps2.h"

#include <string.h>

struct Ps2IopRegistryEntry {
    char name[PS2_IOP_REGISTRY_NAME_MAX + 1];
    int id;
};

static struct Ps2IopRegistryEntry s_entries[PS2_IOP_REGISTRY_CAPACITY];
static unsigned int s_count;

void ps2IopRegistryReset(void)
{
    memset(s_entries, 0, sizeof(s_entries));
    s_count = 0u;
}

static struct Ps2IopRegistryEntry *ps2IopRegistryLookup(const char *name)
{
    for (unsigned int i = 0u; i < s_count; ++i) {
        if (strcmp(s_entries[i].name, name) == 0) {
            return &s_entries[i];
        }
    }
    return NULL;
}

int ps2IopRegistryFind(const char *name)
{
    if (!name) {
        return -1;
    }
    const struct Ps2IopRegistryEntry *entry = ps2IopRegistryLookup(name);
    return entry ? entry->id : -1;
}

int ps2IopRegistryRecord(const char *name, int id)
{
    if (!name || !name[0] || id < 0 ||
        strlen(name) > PS2_IOP_REGISTRY_NAME_MAX) {
        return -1;
    }

    struct Ps2IopRegistryEntry *entry = ps2IopRegistryLookup(name);
    if (!entry) {
        if (s_count >= PS2_IOP_REGISTRY_CAPACITY) {
            return -1;
        }
        entry = &s_entries[s_count++];
        strcpy(entry->name, name);
    }
    entry->id = id;
    return 0;
}
