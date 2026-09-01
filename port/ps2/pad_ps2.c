#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <libpad.h>
#include <loadfile.h>
#include <sifrpc.h>

#include "log_ps2.h"
#include "pad_axis_ps2.h"
#include "pad_ps2.h"
#include "system.h"

#define PS2_PAD_RPC_BYTES 256
#define PS2_PAD_MODE_TABLE_MAX 8

enum Ps2PadConfigureStage {
    PS2_PAD_STAGE_WAIT_STABLE = 0,
    PS2_PAD_STAGE_WAIT_ANALOG,
    PS2_PAD_STAGE_WAIT_ACTUATORS,
    PS2_PAD_STAGE_READY,
};

struct Ps2PadPort {
    bool opened;
    bool was_connected;
    enum Ps2PadConfigureStage stage;
    uint8_t actuator_count;
    uint8_t rumble_small;
    uint8_t rumble_large;
    int last_libpad_state;
    bool read_ok;
    uint32_t successful_reads;
    struct Ps2PadState state;
};

/*
 * POTWIERDZONE / CURRENT PS2SDK libpad contract:
 * each pad area is exactly 256 bytes and must be 64-byte aligned. The second
 * row is also naturally aligned because the stride is a multiple of 64.
 * This is PAD RPC/device alignment, not a general heap alignment policy.
 */
static unsigned char s_pad_rpc_area[PS2_PAD_MAX_PLAYERS][PS2_PAD_RPC_BYTES]
    __attribute__((aligned(64)));
static struct Ps2PadPort s_ports[PS2_PAD_MAX_PLAYERS];
static bool s_pad_rpc_initialized;
static bool s_pad_initialized;
static int s_sio2_module_result = -1;
static int s_pad_module_result = -1;

static int ps2PadEnsureModule(const char *name, const char *rom_path)
{
    int result = SifSearchModuleByName(name);
    if (result >= 0) {
        sysLogPrintf(LOG_NOTE,
            "PAD: reuse resident IOP module %s id=%d", name, result);
        return result;
    }

    result = SifLoadModule(rom_path, 0, NULL);
    if (result >= 0) {
        sysLogPrintf(LOG_NOTE,
            "PAD: loaded IOP module %s from %s id=%d",
            name, rom_path, result);
        return result;
    }

    /*
     * Some loaders report a duplicate load as an error. Re-query before
     * declaring the backend unavailable; the resident RPC server is the
     * actual dependency, not ownership of the module load operation.
     */
    const int resident = SifSearchModuleByName(name);
    if (resident >= 0) {
        sysLogPrintf(LOG_WARNING,
            "PAD: %s load returned %d but resident module id=%d is usable",
            rom_path, result, resident);
        return resident;
    }

    sysLogPrintf(LOG_ERROR,
        "PAD: failed to provide IOP module %s from %s (%d)",
        name, rom_path, result);
    return result;
}

static void ps2PadResetLiveState(struct Ps2PadPort *port, bool emit_release)
{
    const uint32_t old_held = port->state.held;
    memset(&port->state, 0, sizeof(port->state));
    if (emit_release) {
        port->state.released = old_held;
    }
    port->read_ok = false;
}

static bool ps2PadModeTableHasDualShock(int player)
{
    int count = padInfoMode(player, 0, PAD_MODETABLE, -1);
    if (count <= 0) {
        return false;
    }
    if (count > PS2_PAD_MODE_TABLE_MAX) {
        count = PS2_PAD_MODE_TABLE_MAX;
    }

    for (int i = 0; i < count; ++i) {
        if (padInfoMode(player, 0, PAD_MODETABLE, i) == PAD_TYPE_DUALSHOCK) {
            return true;
        }
    }
    return false;
}

static void ps2PadStartActuatorConfig(int player, struct Ps2PadPort *port)
{
    port->actuator_count = padInfoAct(player, 0, -1, 0);
    if (port->actuator_count == 0) {
        port->state.rumble = false;
        port->stage = PS2_PAD_STAGE_READY;
        return;
    }

    char align[6] = { (char)0xff, (char)0xff, (char)0xff,
                      (char)0xff, (char)0xff, (char)0xff };
    const int count = port->actuator_count > 6 ? 6 : port->actuator_count;
    for (int i = 0; i < count; ++i) {
        align[i] = (char)i;
    }

    if (padSetActAlign(player, 0, align) != 0) {
        port->stage = PS2_PAD_STAGE_WAIT_ACTUATORS;
    } else {
        port->actuator_count = 0;
        port->state.rumble = false;
        port->stage = PS2_PAD_STAGE_READY;
    }
}

static void ps2PadConfigureStablePort(int player, struct Ps2PadPort *port)
{
    switch (port->stage) {
        case PS2_PAD_STAGE_WAIT_STABLE:
            if (ps2PadModeTableHasDualShock(player)) {
                if (padSetMainMode(player, 0, PAD_MMODE_DUALSHOCK, PAD_MMODE_LOCK) != 0) {
                    port->stage = PS2_PAD_STAGE_WAIT_ANALOG;
                    return;
                }
            }

            /* Digital/unknown pads remain useful for menus and fallback tests. */
            port->state.analog = false;
            ps2PadStartActuatorConfig(player, port);
            return;

        case PS2_PAD_STAGE_WAIT_ANALOG: {
            const int req = padGetReqState(player, 0);
            if (req == PAD_RSTAT_BUSY) {
                return;
            }

            port->state.analog = req == PAD_RSTAT_COMPLETE &&
                padInfoMode(player, 0, PAD_MODECURID, 0) == PAD_TYPE_DUALSHOCK;
            ps2PadStartActuatorConfig(player, port);
            return;
        }

        case PS2_PAD_STAGE_WAIT_ACTUATORS: {
            const int req = padGetReqState(player, 0);
            if (req == PAD_RSTAT_BUSY) {
                return;
            }

            port->state.rumble = req == PAD_RSTAT_COMPLETE && port->actuator_count > 0;
            if (!port->state.rumble) {
                port->actuator_count = 0;
            }
            port->stage = PS2_PAD_STAGE_READY;
            return;
        }

        case PS2_PAD_STAGE_READY:
        default:
            return;
    }
}

static void ps2PadUpdatePort(int player)
{
    struct Ps2PadPort *port = &s_ports[player];
    if (!port->opened) {
        return;
    }

    const int pad_state = padGetState(player, 0);
    port->last_libpad_state = pad_state;
    if (pad_state == PAD_STATE_DISCONN || pad_state == PAD_STATE_ERROR) {
        if (port->was_connected) {
            sysLogPrintf(LOG_WARNING, "PAD%d: controller disconnected", player + 1);
            ps2PadResetLiveState(port, true);
        } else {
            ps2PadResetLiveState(port, false);
        }
        port->was_connected = false;
        port->stage = PS2_PAD_STAGE_WAIT_STABLE;
        port->actuator_count = 0;
        port->rumble_small = 0;
        port->rumble_large = 0;
        return;
    }

    /* The official PS2SDK PADMAN sample accepts FINDCTP1 as readable too. */
    if (pad_state != PAD_STATE_STABLE && pad_state != PAD_STATE_FINDCTP1) {
        return;
    }

    if (!port->was_connected) {
        port->was_connected = true;
        port->state.connected = true;
        sysLogPrintf(LOG_NOTE, "PAD%d: controller connected", player + 1);
    }

    ps2PadConfigureStablePort(player, port);
    if (port->stage != PS2_PAD_STAGE_READY) {
        return;
    }

    struct padButtonStatus buttons;
    if (padRead(player, 0, &buttons) == 0) {
        port->read_ok = false;
        return;
    }

    const uint32_t old_held = port->state.held;
    const uint32_t held = ((uint32_t)(~buttons.btns)) & 0xffffu;

    port->state.connected = true;
    port->state.held = held;
    port->state.pressed = held & ~old_held;
    port->state.released = old_held & ~held;
    port->state.lx = ps2PadAxisHorizontalFromRaw(buttons.ljoy_h);
    port->state.ly = ps2PadAxisVerticalFromRaw(buttons.ljoy_v);
    port->state.rx = ps2PadAxisHorizontalFromRaw(buttons.rjoy_h);
    port->state.ry = ps2PadAxisVerticalFromRaw(buttons.rjoy_v);
    port->read_ok = true;
    ++port->successful_reads;
}

bool ps2PadInit(void)
{
    if (s_pad_initialized) {
        return true;
    }

    memset(s_ports, 0, sizeof(s_ports));
    memset(s_pad_rpc_area, 0, sizeof(s_pad_rpc_area));
    for (int player = 0; player < PS2_PAD_MAX_PLAYERS; ++player) {
        s_ports[player].last_libpad_state = PAD_STATE_DISCONN;
    }

    /* Current PS2SDK padx sample uses the ROM extended SIO2/PAD modules. */
    sceSifInitRpc(0);

    s_sio2_module_result =
        ps2PadEnsureModule("sio2man", "rom0:XSIO2MAN");
    if (s_sio2_module_result < 0) {
        ps2LogCheckpoint();
        return false;
    }

    s_pad_module_result =
        ps2PadEnsureModule("padman", "rom0:XPADMAN");
    if (s_pad_module_result < 0) {
        ps2LogCheckpoint();
        return false;
    }

    if (padInit(0) != 1) {
        sysLogPrintf(LOG_ERROR, "PAD: padInit failed");
        ps2LogCheckpoint();
        return false;
    }
    s_pad_rpc_initialized = true;

    int opened = 0;
    for (int player = 0; player < PS2_PAD_MAX_PLAYERS; ++player) {
        struct Ps2PadPort *port = &s_ports[player];
        port->stage = PS2_PAD_STAGE_WAIT_STABLE;
        if (padPortOpen(player, 0, s_pad_rpc_area[player]) != 0) {
            port->opened = true;
            ++opened;
        } else {
            sysLogPrintf(LOG_WARNING, "PAD%d: padPortOpen failed", player + 1);
        }
    }

    s_pad_initialized = opened > 0;
    sysLogPrintf(s_pad_initialized ? LOG_NOTE : LOG_ERROR,
        "PAD: backend ready ports=%d/%d scheme=dual-analog shooter",
        opened, PS2_PAD_MAX_PLAYERS);
    ps2LogCheckpoint();
    return s_pad_initialized;
}

void ps2PadShutdown(void)
{
    if (!s_pad_rpc_initialized) {
        return;
    }

    for (int player = 0; player < PS2_PAD_MAX_PLAYERS; ++player) {
        if (s_ports[player].opened) {
            ps2PadSetRumble(player, 0, 0);
            padPortClose(player, 0);
            s_ports[player].opened = false;
        }
    }

    padEnd();
    s_pad_rpc_initialized = false;
    s_pad_initialized = false;
}

void ps2PadUpdate(void)
{
    if (!s_pad_initialized) {
        return;
    }

    for (int player = 0; player < PS2_PAD_MAX_PLAYERS; ++player) {
        ps2PadUpdatePort(player);
    }
}

const struct Ps2PadState *ps2PadGetState(int player)
{
    if (player < 0 || player >= PS2_PAD_MAX_PLAYERS) {
        return NULL;
    }
    return &s_ports[player].state;
}

bool ps2PadGetDiagnostics(int player, struct Ps2PadDiagnostics *diagnostics)
{
    if (!diagnostics || player < 0 || player >= PS2_PAD_MAX_PLAYERS) {
        return false;
    }

    const struct Ps2PadPort *port = &s_ports[player];
    diagnostics->backend_initialized = s_pad_rpc_initialized;
    diagnostics->port_open = port->opened;
    diagnostics->transport_ready =
        port->stage == PS2_PAD_STAGE_READY &&
        (port->last_libpad_state == PAD_STATE_STABLE ||
         port->last_libpad_state == PAD_STATE_FINDCTP1);
    diagnostics->read_ok = port->read_ok;
    diagnostics->libpad_state = port->last_libpad_state;
    diagnostics->configure_stage = (uint8_t)port->stage;
    diagnostics->successful_reads = port->successful_reads;
    diagnostics->raw_held = port->state.held;
    diagnostics->sio2_module_result = s_sio2_module_result;
    diagnostics->pad_module_result = s_pad_module_result;
    return true;
}

void ps2PadSetRumble(int player, uint8_t small_motor, uint8_t large_motor)
{
    if (!s_pad_initialized || player < 0 || player >= PS2_PAD_MAX_PLAYERS) {
        return;
    }

    struct Ps2PadPort *port = &s_ports[player];
    if (!port->opened || port->stage != PS2_PAD_STAGE_READY ||
        !port->state.connected || !port->state.rumble) {
        return;
    }

    small_motor = small_motor ? 1u : 0u;
    if (port->rumble_small == small_motor && port->rumble_large == large_motor) {
        return;
    }

    char direct[6] = { 0, 0, 0, 0, 0, 0 };
    if (port->actuator_count >= 1) {
        direct[0] = (char)small_motor;
    }
    if (port->actuator_count >= 2) {
        direct[1] = (char)large_motor;
    }

    if (padSetActDirect(player, 0, direct) != 0) {
        port->rumble_small = small_motor;
        port->rumble_large = large_motor;
    }
}
