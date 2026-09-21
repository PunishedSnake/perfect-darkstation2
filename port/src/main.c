#include <stdlib.h>
#include <stdio.h>
#include <PR/ultratypes.h>
#include <PR/ultrasched.h>
#include <PR/os_message.h>

#include "lib/main.h"
#include "bss.h"
#include "data.h"

#include "video.h"
#include "audio.h"
#include "input.h"
#include "fs.h"
#include "romdata.h"
#include "config.h"
#include "mod.h"
#include "system.h"
#include "utils.h"

#ifdef PLATFORM_PS2
#include "log_ps2.h"
#include "storage_ps2.h"
#define GAME_STARTUP_CHECKPOINT() ps2LogCheckpointForce()

#ifdef PD_PS2_FMCB_STARTUP_DIAGNOSTIC
/*
 * Real-hardware launcher diagnostic.
 *
 * BGCOLOR is intentionally written without gsKit/SIF/filesystem dependencies,
 * matching the standalone loader probes. The inherited launcher scanout makes
 * the pre-video markers visible. Each marker is held by a pure EE busy-loop so
 * the diagnostic does not depend on ThreadMan, timers or IOP state.
 */
static void ps2FmcbStartupMarker(u64 color)
{
	*(volatile u64 *)0x120000e0 = color;

	u32 spins = 40000000u;
	__asm__ __volatile__(
		".set noreorder\n\t"
		"1:\n\t"
		"addiu %0, %0, -1\n\t"
		"bnez %0, 1b\n\t"
		"nop\n\t"
		".set reorder\n\t"
		: "+r"(spins)
		:
		: "memory");
}

#define PS2_FMCB_STARTUP_MARKER(color) ps2FmcbStartupMarker((u64)(color))

#define PS2_FMCB_COLOR_MAIN_ENTRY        0x000000ffULL /* red */
#define PS2_FMCB_COLOR_ARGS_STORED       0x000080ffULL /* orange */
#define PS2_FMCB_COLOR_ARG_SCAN_OK       0x0000ffffULL /* yellow */
#define PS2_FMCB_COLOR_CRASH_READY       0x0000ff80ULL /* lime */
#define PS2_FMCB_COLOR_SYSTEM_READY      0x0000ff00ULL /* green */
#define PS2_FMCB_COLOR_CLEAN_IOP_BEGIN   0x00800080ULL /* purple */
#define PS2_FMCB_COLOR_CLEAN_IOP_READY   0x00808000ULL /* teal */
#define PS2_FMCB_COLOR_STORAGE_READY     0x00ffff00ULL /* cyan: mass usable */
#define PS2_FMCB_COLOR_STORAGE_FAILED    0x00000080ULL /* dark red */
#define PS2_FMCB_COLOR_FS_INIT_READY     0x00ff8000ULL /* azure */
#define PS2_FMCB_COLOR_CONFIG_STAT_READY 0x00ff0080ULL /* violet */
#define PS2_FMCB_COLOR_CONFIG_LOAD_READY 0x00ff00ffULL /* magenta */
#define PS2_FMCB_COLOR_CONFIG_SAVE_READY 0x00ffffffULL /* white */
#define PS2_FMCB_COLOR_FILES_READY       0x0080ff00ULL /* mint */
#define PS2_FMCB_COLOR_INPUT_READY       0x00ff0000ULL /* blue */
#define PS2_FMCB_COLOR_AUDIO_READY       0x00ff0080ULL /* violet */
#define PS2_FMCB_COLOR_ROM_READY         0x00ff00ffULL /* magenta */
#define PS2_FMCB_COLOR_VIDEO_READY       0x00ffffffULL /* white */
#define PS2_FMCB_COLOR_MAIN_LOOP_READY   0x00808080ULL /* grey */
#else
#define PS2_FMCB_STARTUP_MARKER(color) ((void)0)
#endif
#else
#define GAME_STARTUP_CHECKPOINT() ((void)0)
#define PS2_FMCB_STARTUP_MARKER(color) ((void)0)
#endif

u32 g_OsMemSize = 0;
s32 g_OsMemSizeMb = 16;
u8 g_Is4Mb = 0;
s8 g_Resetting = false;
OSSched g_Sched;

OSMesgQueue g_MainMesgQueue;
OSMesg g_MainMesgBuf[32];

u8 *g_MempHeap = NULL;
u32 g_MempHeapSize = 0;

u32 g_VmNumTlbMisses = 0;
u32 g_VmNumPageMisses = 0;
u32 g_VmNumPageReplaces = 0;
u8 g_VmShowStats = 0;

s32 g_TickRateDiv = 1;
s32 g_TickExtraSleep = true;

s32 g_SkipIntro = false;

s32 g_FileAutoSelect = -1;

extern s32 g_StageNum;

s32 bootGetMemSize(void)
{
	return (s32)g_OsMemSize;
}

void *bootAllocateStack(s32 threadid, s32 size)
{
	static u8 bruh[0x1000];
	return bruh;
}

void bootCreateSched(void)
{
	osCreateMesgQueue(&g_MainMesgQueue, g_MainMesgBuf, ARRAYCOUNT(g_MainMesgBuf));
	if (osTvType == OS_TV_MPAL) {
		osCreateScheduler(&g_Sched, NULL, OS_VI_MPAL_LAN1, 1);
	} else {
		osCreateScheduler(&g_Sched, NULL, OS_VI_NTSC_LAN1, 1);
	}
}

static void gameInit(void)
{
	osMemSize = g_OsMemSizeMb * 1024 * 1024;

	for (s32 i = 0; i < MAX_PLAYERS; ++i) {
		struct extplayerconfig *cfg = g_PlayerExtCfg + i;
		cfg->fovzoommult = cfg->fovzoom ? cfg->fovy / 60.0f : 1.0f;
	}

	if (g_HudCenter == HUDCENTER_NORMAL) {
		g_HudAlignModeL = G_ASPECT_CENTER_EXT;
		g_HudAlignModeR = G_ASPECT_CENTER_EXT;
	} else if (g_HudCenter == HUDCENTER_WIDE) {
		g_HudAlignModeL = G_ASPECT_LEFT_EXT | G_ASPECT_WIDE_EXT;
		g_HudAlignModeR = G_ASPECT_RIGHT_EXT | G_ASPECT_WIDE_EXT;
	}
}

static void cleanup(void)
{
	sysLogPrintf(LOG_NOTE, "shutdown");
	inputSaveBinds();
	configSave(CONFIG_PATH);
	videoShutdown();
	crashShutdown();
	// TODO: actually shut down all subsystems
}

int main(int argc, const char **argv)
{
	PS2_FMCB_STARTUP_MARKER(PS2_FMCB_COLOR_MAIN_ENTRY);

	sysInitArgs(argc, argv);
	PS2_FMCB_STARTUP_MARKER(PS2_FMCB_COLOR_ARGS_STORED);

	const s32 disable_crash_handler = sysArgCheck("--no-crash-handler");
	PS2_FMCB_STARTUP_MARKER(PS2_FMCB_COLOR_ARG_SCAN_OK);

	if (!disable_crash_handler) {
		crashInit();
	}
	PS2_FMCB_STARTUP_MARKER(PS2_FMCB_COLOR_CRASH_READY);

	sysInit();
	PS2_FMCB_STARTUP_MARKER(PS2_FMCB_COLOR_SYSTEM_READY);
	sysLogPrintf(LOG_NOTE, "runtime: system ready");
	GAME_STARTUP_CHECKPOINT();

#if PLATFORM_PS2 && defined(PD_PS2_POST_STORAGE_USB_LOG_DIAGNOSTIC)
	/*
	 * PRE phase: capture the exact launcher handoff using the launcher's own
	 * still-live device namespace. The file is placed next to argv[0], forced
	 * durable, and CLOSED before any IOP reboot.
	 */
	{
		char inherited_base[FS_MAXPATH + 1];
		char inherited_log[FS_MAXPATH + 64];
		sysGetExecutablePath(inherited_base, sizeof(inherited_base));
		snprintf(inherited_log, sizeof(inherited_log),
			"%s/%s", inherited_base, "pdps2-r3z-pre.log");

		if (ps2LogOpenPostStorageFile(inherited_log)) {
			sysLogPrintf(LOG_NOTE,
				"R3Z PRE: inherited logger opened path=%s", inherited_log);
			sysLogPrintf(LOG_NOTE, "R3Z PRE: argc=%d", argc);
			for (s32 i = 0; i < argc; ++i) {
				sysLogPrintf(LOG_NOTE, "R3Z PRE: raw argv[%d]=%s", i,
					argv && argv[i] ? argv[i] : "(null)");
			}
			sysLogPrintf(LOG_NOTE,
				"R3Z PRE: argv0-derived executable base=%s", inherited_base);
			ps2LogCheckpointForce();
			ps2LogCloseFileSink();
		}
	}
#endif

#if PLATFORM_PS2 && defined(PD_PS2_CLEAN_IOP_STARTUP_DIAGNOSTIC)
	/*
	 * A/B recovery for launchers that leave PAD/SIO2/RPC in a poisoned state.
	 * This deliberately throws away the inherited IOP personality before any
	 * game-owned IOP service is initialized. Storage is rebuilt immediately
	 * afterwards from embedded current-PS2SDK modules.
	 */
	PS2_FMCB_STARTUP_MARKER(PS2_FMCB_COLOR_CLEAN_IOP_BEGIN);
	const s32 clean_iop_result = ps2StorageResetIopForCleanBoot();
	if (clean_iop_result < 0) {
		PS2_FMCB_STARTUP_MARKER(PS2_FMCB_COLOR_STORAGE_FAILED);
		sysFatalError("Clean IOP bootstrap failed (%d).", clean_iop_result);
	}
	PS2_FMCB_STARTUP_MARKER(PS2_FMCB_COLOR_CLEAN_IOP_READY);
#endif

#if PLATFORM_PS2
	/*
	 * The ROM and configuration are commonly next to an ELF launched from
	 * mass:. Do not make that storage service an undocumented property of the
	 * parent launcher. Reuse a working inherited stack, otherwise provide the
	 * current PS2SDK USBD/USBHDFSD pair without resetting the IOP.
	 */
	const s32 mass_result = ps2StorageEnsureMass(
		argc > 0 && argv ? argv[0] : NULL);
	sysLogPrintf(mass_result >= 0 ? LOG_NOTE : LOG_WARNING,
		"runtime: mass storage bootstrap result=%d", mass_result);
	GAME_STARTUP_CHECKPOINT();
	PS2_FMCB_STARTUP_MARKER(
		mass_result >= 0 ? PS2_FMCB_COLOR_STORAGE_READY
		                 : PS2_FMCB_COLOR_STORAGE_FAILED);

#ifdef PD_PS2_POST_STORAGE_USB_LOG_DIAGNOSTIC
	/*
	 * POST phase: now use only the current-PS2SDK filesystem rebuilt after the
	 * reset. PRE and POST are deliberately separate files/descriptor lifetimes.
	 */
	const char *const startup_log_path = "mass:/pdps2-r3z-post.log";
	const s32 startup_log_open = ps2LogOpenPostStorageFile(startup_log_path);
	if (startup_log_open) {
		char executable_base[FS_MAXPATH + 1];
		sysGetExecutablePath(executable_base, sizeof(executable_base));
		sysLogPrintf(LOG_NOTE,
			"R3Z TRACE: post-storage logger opened path=%s mass_result=%d",
			startup_log_path, mass_result);
		sysLogPrintf(LOG_NOTE, "R3Z TRACE: argc=%d", argc);
		for (s32 i = 0; i < argc; ++i) {
			sysLogPrintf(LOG_NOTE, "R3Z TRACE: raw argv[%d]=%s", i,
				argv && argv[i] ? argv[i] : "(null)");
		}
		sysLogPrintf(LOG_NOTE, "R3Z TRACE: argv0-derived executable base=%s",
			executable_base);
		ps2LogCheckpointForce();
	} else {
		sysLogPrintf(LOG_WARNING,
			"R3Z TRACE: could not open post-storage log at %s",
			startup_log_path);
	}
#endif
#endif

	if (fsInit() < 0) {
		sysFatalError("Filesystem initialisation failed.");
	}
	PS2_FMCB_STARTUP_MARKER(PS2_FMCB_COLOR_FS_INIT_READY);

#if PLATFORM_PS2
	const s32 initial_config_size = fsFileSize(CONFIG_PATH);
	PS2_FMCB_STARTUP_MARKER(PS2_FMCB_COLOR_CONFIG_STAT_READY);
#endif

	configInit();
	PS2_FMCB_STARTUP_MARKER(PS2_FMCB_COLOR_CONFIG_LOAD_READY);

#if PLATFORM_PS2
	/*
	 * The game normally saves configuration only from the atexit cleanup path.
	 * Console builds spend their lifetime in mainProc(), and an interrupted
	 * bring-up run therefore never created pd.ini. Publish the registered
	 * defaults once at startup so settings such as Game.SkipIntro remain
	 * available without requiring a clean process exit.
	 */
	if (initial_config_size <= 0) {
		const s32 saved = configSave(CONFIG_PATH);
		sysLogPrintf(saved ? LOG_NOTE : LOG_WARNING,
			"runtime: default configuration %s at %s (previous size=%d)",
			saved ? "created" : "could not be created", CONFIG_PATH,
			initial_config_size);
		GAME_STARTUP_CHECKPOINT();
	}
	PS2_FMCB_STARTUP_MARKER(PS2_FMCB_COLOR_CONFIG_SAVE_READY);
#endif

	sysLogPrintf(LOG_NOTE, "runtime: filesystem and configuration ready");
	GAME_STARTUP_CHECKPOINT();
	PS2_FMCB_STARTUP_MARKER(PS2_FMCB_COLOR_FILES_READY);

#if PLATFORM_PS2
	/*
	 * Initialise SIF/RPC-backed services before dmaKit takes ownership of the
	 * graphics channels. This is also the ordering already proven by the PS2
	 * hardware diagnostic. Neither PAD nor audio is allowed to keep the game
	 * behind one shared checkpoint: a missing optional service must remain
	 * visible and the runtime must still advance to video/ROM startup.
	 */
	sysLogPrintf(LOG_NOTE, "runtime: input initialisation begin");
	GAME_STARTUP_CHECKPOINT();
	const s32 input_result = inputInit();
	sysLogPrintf(input_result >= 0 ? LOG_NOTE : LOG_WARNING,
		"runtime: input initialisation end result=%d", input_result);
	GAME_STARTUP_CHECKPOINT();
	PS2_FMCB_STARTUP_MARKER(PS2_FMCB_COLOR_INPUT_READY);

	sysLogPrintf(LOG_NOTE, "runtime: audio initialisation begin");
	GAME_STARTUP_CHECKPOINT();
	const s32 audio_result = audioInit();
	sysLogPrintf(audio_result >= 0 ? LOG_NOTE : LOG_WARNING,
		"runtime: audio initialisation end result=%d", audio_result);
	GAME_STARTUP_CHECKPOINT();
	PS2_FMCB_STARTUP_MARKER(PS2_FMCB_COLOR_AUDIO_READY);
	sysLogPrintf(LOG_NOTE, "runtime: input and audio ready");
	GAME_STARTUP_CHECKPOINT();

	/*
	 * The bootstrap has hardware-validated bounded ROM streaming before GS
	 * startup. Keep all mass:/SIF-backed bootstrap reads on that known ordering
	 * and expose every permanent segment through durable checkpoints.
	 */
	sysLogPrintf(LOG_NOTE, "runtime: ROM data initialisation begin");
	GAME_STARTUP_CHECKPOINT();
	if (romdataInit() < 0) {
		sysFatalError("ROM data initialisation failed.");
	}
	sysLogPrintf(LOG_NOTE, "runtime: ROM data ready");
	GAME_STARTUP_CHECKPOINT();
	g_ValidGbcRomFound = romdataCheckGbcRom();
	PS2_FMCB_STARTUP_MARKER(PS2_FMCB_COLOR_ROM_READY);
#endif

	if (videoInit() < 0) {
		sysFatalError("Video initialisation failed.");
	}
	sysLogPrintf(LOG_NOTE, "runtime: video ready");
	GAME_STARTUP_CHECKPOINT();
	PS2_FMCB_STARTUP_MARKER(PS2_FMCB_COLOR_VIDEO_READY);

#if !PLATFORM_PS2
	inputInit();
	audioInit();
	sysLogPrintf(LOG_NOTE, "runtime: input and audio ready");
	GAME_STARTUP_CHECKPOINT();
	if (romdataInit() < 0) {
		sysFatalError("ROM data initialisation failed.");
	}
	sysLogPrintf(LOG_NOTE, "runtime: ROM data ready");
	GAME_STARTUP_CHECKPOINT();

	g_ValidGbcRomFound = romdataCheckGbcRom();
#endif

	gameInit();

	if (fsGetModDir()) {
		modConfigLoad(MOD_CONFIG_FNAME);
	}

	atexit(cleanup);

	bootCreateSched();

	g_OsMemSize = osGetMemSize();

	g_MempHeapSize = sysMemGetGameHeapSize(g_OsMemSize);
	if (g_MempHeapSize == 0) {
		sysFatalError("Platform cannot satisfy the game heap budget requested=%u bytes.",
			g_OsMemSize);
	}

	/* Keep libultra/game heuristics consistent with the arena actually owned. */
	g_OsMemSize = g_MempHeapSize;
	osMemSize = g_MempHeapSize;
	g_MempHeap = sysMemZeroAlloc(g_MempHeapSize);
	if (!g_MempHeap) {
		sysFatalError("Could not alloc %u bytes for memp heap.", g_MempHeapSize);
	}

	sysLogPrintf(LOG_NOTE, "memp heap at %p - %p", g_MempHeap, g_MempHeap + g_MempHeapSize);
	if (g_RomFile) {
		sysLogPrintf(LOG_NOTE, "rom  file at %p - %p", g_RomFile, g_RomFile + g_RomFileSize);
	} else {
		sysLogPrintf(LOG_NOTE, "rom  file-backed source, size %u", g_RomFileSize);
	}
	GAME_STARTUP_CHECKPOINT();

#if PLATFORM_PS2
	g_SndDisabled = sysArgCheck("--no-sound") || audio_result < 0;
#else
	g_SndDisabled = sysArgCheck("--no-sound");
#endif

	g_StageNum = sysArgGetInt("--boot-stage", STAGE_TITLE);

	if (g_StageNum == STAGE_TITLE && (sysArgCheck("--skip-intro") || g_SkipIntro)) {
		// shorthand for --boot-stage 0x26
		g_StageNum = STAGE_CITRAINING;
	} else if (g_StageNum < 0x01 || g_StageNum > 0x5d) {
		// stage num out of range
		g_StageNum = STAGE_TITLE;
	}

	if (g_StageNum != STAGE_TITLE) {
		sysLogPrintf(LOG_NOTE, "boot stage set to 0x%02x", g_StageNum);
	}

	g_FileAutoSelect = sysArgGetInt("--profile", -1);
	if (g_FileAutoSelect >= 0) {
		sysLogPrintf(LOG_NOTE, "player profile set to %d", g_FileAutoSelect);
	}

	sysLogPrintf(LOG_NOTE, "runtime: entering Perfect Dark main loop stage=0x%02x", g_StageNum);
	GAME_STARTUP_CHECKPOINT();
	PS2_FMCB_STARTUP_MARKER(PS2_FMCB_COLOR_MAIN_LOOP_READY);
#if PLATFORM_PS2 && defined(PD_PS2_POST_STORAGE_USB_LOG_DIAGNOSTIC)
	/*
	 * This artifact diagnoses startup only. Do not let synchronous USB logging
	 * contaminate renderer/audio timing once the game loop begins.
	 */
	ps2LogCloseFileSink();
#endif
	mainProc();

	return 0;
}

PD_CONSTRUCTOR static void gameConfigInit(void)
{
	configRegisterInt("Game.MemorySize", &g_OsMemSizeMb, 4, 2048);
	configRegisterInt("Game.CenterHUD", &g_HudCenter, 0, 2);
	configRegisterInt("Game.MenuMouseControl", &g_MenuMouseControl, 0, 1);
	configRegisterFloat("Game.ScreenShakeIntensity", &g_ViShakeIntensityMult, 0.f, 10.f);
	configRegisterInt("Game.TickRateDivisor", &g_TickRateDiv, 0, 10);
	configRegisterInt("Game.ExtraSleep", &g_TickExtraSleep, 0, 1);
	configRegisterInt("Game.SkipIntro", &g_SkipIntro, 0, 1);
	configRegisterInt("Game.DisableMpDeathMusic", &g_MusicDisableMpDeath, 0, 1);
	configRegisterInt("Game.GEMuzzleFlashes", &g_BgunGeMuzzleFlashes, 0, 1);
	configRegisterInt("Game.MaxExplosions", &g_MaxExplosions, 6, 96);
	for (s32 j = 0; j < MAX_PLAYERS; ++j) {
		const s32 i = j + 1;
		configRegisterFloat(strFmt("Game.Player%d.FovY", i), &g_PlayerExtCfg[j].fovy, 5.f, 175.f);
		configRegisterInt(strFmt("Game.Player%d.FovAffectsZoom", i), &g_PlayerExtCfg[j].fovzoom, 0, 1);
		configRegisterInt(strFmt("Game.Player%d.MouseAimMode", i), &g_PlayerExtCfg[j].mouseaimmode, 0, 1);
		configRegisterFloat(strFmt("Game.Player%d.MouseAimSpeedX", i), &g_PlayerExtCfg[j].mouseaimspeedx, 0.f, 10.f);
		configRegisterFloat(strFmt("Game.Player%d.MouseAimSpeedY", i), &g_PlayerExtCfg[j].mouseaimspeedy, 0.f, 10.f);
		configRegisterFloat(strFmt("Game.Player%d.RadialMenuSpeed", i), &g_PlayerExtCfg[j].radialmenuspeed, 0.f, 10.f);
		configRegisterFloat(strFmt("Game.Player%d.CrosshairSway", i), &g_PlayerExtCfg[j].crosshairsway, 0.f, 10.f);
		configRegisterFloat(strFmt("Game.Player%d.CrosshairEdgeBoundary", i), &g_PlayerExtCfg[j].crosshairedgeboundary, 0.0f, 1.0f);
		configRegisterInt(strFmt("Game.Player%d.CrouchMode", i), &g_PlayerExtCfg[j].crouchmode, 0, CROUCHMODE_TOGGLE_ANALOG);
		configRegisterInt(strFmt("Game.Player%d.ExtendedControls", i), &g_PlayerExtCfg[j].extcontrols, 0, 1);
		configRegisterUInt(strFmt("Game.Player%d.CrosshairColour", i), &g_PlayerExtCfg[j].crosshaircolour, 0, 0xFFFFFFFF);
		configRegisterUInt(strFmt("Game.Player%d.CrosshairSize", i), &g_PlayerExtCfg[j].crosshairsize, 0, 4);
		configRegisterInt(strFmt("Game.Player%d.CrosshairHealth", i), &g_PlayerExtCfg[j].crosshairhealth, 0, CROSSHAIR_HEALTH_ON_WHITE);
		configRegisterInt(strFmt("Game.Player%d.UseKeyReloads", i), &g_PlayerExtCfg[j].usereloads, 0, false);
	}
}
