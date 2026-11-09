#ifndef PERFECT_DARK_PS2_IOP_REGISTRY_PS2_H
#define PERFECT_DARK_PS2_IOP_REGISTRY_PS2_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * CURRENT IMPLEMENTATION: modules the game itself started since its owned
 * clean IOP reboot.
 *
 * SifSearchModuleByName() cannot answer "is it loaded?" here. It uses the
 * LOADFILE RPC LF_F_SEARCH_MOD_BY_NAME, which the boot-ROM LOADFILE left
 * resident by SifIopReset() does not implement on older consoles (e.g. BIOS
 * 1.70J, "Load File service.(99/11/05)"): it returns 0, which reads as
 * "resident, id 0". After a clean reboot the game is the only module owner,
 * so it records what it starts instead of asking the IOP.
 */
#define PS2_IOP_REGISTRY_CAPACITY 16
#define PS2_IOP_REGISTRY_NAME_MAX 23

/* Forget everything; call right after a successful clean IOP reboot. */
void ps2IopRegistryReset(void);

/* Module id recorded for `name`, or -1 when the game has not started it. */
int ps2IopRegistryFind(const char *name);

/*
 * Record a started module. Negative ids, empty names and overlong names are
 * ignored; re-recording a name updates its id. Returns 0 on success, -1 when
 * nothing was recorded.
 */
int ps2IopRegistryRecord(const char *name, int id);

#ifdef __cplusplus
}
#endif

#endif
