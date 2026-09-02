#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>
#include <PR/ultratypes.h>
#include "lib/rzip.h"
#include "romdata.h"
#include "fs.h"
#include "system.h"
#include "preprocess.h"
#include "platform.h"
#include "romdata_policy.h"
#ifdef PLATFORM_PS2
#include "log_ps2.h"
#include "romsource.h"
#define ROMDATA_CHECKPOINT() ps2LogCheckpointForce()
#else
#define ROMDATA_CHECKPOINT() ((void)0)
#endif

/**
 * asset files and ROM segments can be replaced by optional external files,
 * but asset filenames still have to be either pulled from the ROM or from an
 * external file, so stuff can't be completely custom
 * 
 * all data is assumed to be big endian, so it has to be byteswapped
 * at load time, which is fucking terrible
 */

#define ROMDATA_FILEDIR "files"
#define ROMDATA_SEGDIR "segs"

#define ROMDATA_ROM_NAME "pd." VERSION_ROMID ".z64"
#define ROMDATA_ROM_SIZE 33554432

#if VERSION == VERSION_NTSC_FINAL
#define ROMDATA_ROM_TITLE "Perfect Dark"
#define ROMDATA_ROM_ID "NPDE"
#define ROMDATA_ROM_DESC "NTSC v1.1"
#define ROMDATA_FILES_OFS 0x28080
#define ROMDATA_DATA_OFS 0x39850
#elif VERSION == VERSION_PAL_FINAL
#define ROMDATA_ROM_TITLE "Perfect Dark"
#define ROMDATA_ROM_ID "NPDP"
#define ROMDATA_ROM_DESC "PAL"
#define ROMDATA_FILES_OFS 0x28910
#define ROMDATA_DATA_OFS 0x39850
#elif VERSION == VERSION_JPN_FINAL
#define ROMDATA_ROM_TITLE "PERFECT DARK"
#define ROMDATA_ROM_ID "NPDJ"
#define ROMDATA_ROM_DESC "JPN"
#define ROMDATA_FILES_OFS 0x28800
#define ROMDATA_DATA_OFS 0x39850
#else
#error "This ROM version is unsupported."
#endif

#define ROMDATA_MAX_FILES 2048

#ifdef PLATFORM_PS2
#define ROMDATA_RZIP_INPUT_CHUNK 8192
#define ROMDATA_MAX_DATA_SEG_SIZE (4 * 1024 * 1024)
#endif

#define GBC_ROM_NAME "pd.gbc"
#define GBC_ROM_SIZE 4194304

u8 *g_RomFile;
u32 g_RomFileSize;

static u8 *romDataSeg;
static u32 romDataSegSize;
static const char *romName = ROMDATA_ROM_NAME;
#ifdef PLATFORM_PS2
static struct romsource romSource;
static u8 *romNameTable;
static u8 romEmptyFile;
#endif

enum loadsource {
	SRC_UNLOADED = 0,
	SRC_ROM,
	SRC_EXTERNAL
};

struct romfilepatch {
	u32 ofs;
	u32 len;
	const char *src;
	const char *dst;
};

struct romfile {
	u8 **segstart;
	u8 **segend;
	const char *name;
	u8 *data;
	u32 size;
	preprocessfunc preprocess;
	s32 source; // enum loadsource
	s32 preprocessed;
	const struct romfilepatch *patches;
	u32 numpatches;
	u32 romoffset;
	u32 romsize;
	s32 owned;
};

/* patches for individual files; applied on file load, before preprocFuncs, but */
/* after unzip; only applied when loading from a ROM file                       */
static const struct romfilepatch filePatches[] = {
	/* FILE_USETUPLUE: fixes Jon's double "if what" in Infiltration outro */
	{ 0x92a2, 1, "\x6c", "\x99" },
	{ 0x92b0, 1, "\x6c", "\x99" },
};

static struct romfile fileSlots[ROMDATA_MAX_FILES] = {
	[FILE_USETUPLUE] = { .patches = &filePatches[0], .numpatches = 2 },
};

#define ROMSEG_START(n) _ ## n ## SegmentRomStart
#define ROMSEG_END(n) _ ## n ## SegmentRomEnd

/* segment table for ntsc-final                                                     */
/* size will get calculated automatically if it is 0                                */
/* if there are replacement files in the data dir, they will be loaded instead      */
/* offsets are specified for ntsc-final, pal-final and jpn-final in that order      */
#define ROMSEG_LIST() \
	ROMSEG_DECL_SEG(fontjpnsingle,      0x194b20,  0x180330,  0x0,       0x0,      preprocessJpnFont       ) \
	ROMSEG_DECL_SEG(fontjpnmulti,       0x19fb40,  0x18b340,  0x0,       0x0,      preprocessJpnFont       ) \
	ROMSEG_DECL_SEG(animations,         0x1a15c0,  0x18cdc0,  0x190c50,  0x0,      preprocessAnimations    ) \
	ROMSEG_DECL_SEG(mpconfigs,          0x7d0a40,  0x7bc240,  0x7c00d0,  0x11e0,   preprocessMpConfigs     ) \
	ROMSEG_DECL_SEG(mpstringsE,         0x7d1c20,  0x7bd420,  0x7c12b0,  0x3700,   NULL                    ) \
	ROMSEG_DECL_SEG(mpstringsJ,         0x7d5320,  0x7c0b20,  0x7c49b0,  0x3700,   NULL                    ) \
	ROMSEG_DECL_SEG(mpstringsP,         0x7d8a20,  0x7c4220,  0x7c80b0,  0x3700,   NULL                    ) \
	ROMSEG_DECL_SEG(mpstringsG,         0x7dc120,  0x7c7920,  0x7cb7b0,  0x3700,   NULL                    ) \
	ROMSEG_DECL_SEG(mpstringsF,         0x7df820,  0x7cb020,  0x7ceeb0,  0x3700,   NULL                    ) \
	ROMSEG_DECL_SEG(mpstringsS,         0x7e2f20,  0x7ce720,  0x7d25b0,  0x3700,   NULL                    ) \
	ROMSEG_DECL_SEG(mpstringsI,         0x7e6620,  0x7d1e20,  0x7d5cb0,  0x3700,   NULL                    ) \
	ROMSEG_DECL_SEG(firingrange,        0x7e9d20,  0x7d5520,  0x7d93b0,  0x1550,   NULL                    ) \
	ROMSEG_DECL_SEG(fonttahoma,         0x7f7860,  0x7e3060,  0x7e6ef0,  0x0,      preprocessFont          ) \
	ROMSEG_DECL_SEG(fontnumeric,        0x7f8b20,  0x7e4320,  0x7e81b0,  0x0,      preprocessFont          ) \
	ROMSEG_DECL_SEG(fonthandelgothicsm, 0x7f9d30,  0x7e5530,  0x7e93c0,  0x0,      preprocessFont          ) \
	ROMSEG_DECL_SEG(fonthandelgothicxs, 0x7fbfb0,  0x7e87b0,  0x7ec640,  0x0,      preprocessFont          ) \
	ROMSEG_DECL_SEG(fonthandelgothicmd, 0x7fdd80,  0x7eae20,  0x7eecb0,  0x0,      preprocessFont          ) \
	ROMSEG_DECL_SEG(fonthandelgothiclg, 0x8008e0,  0x7eee70,  0x7f2d00,  0x0,      preprocessFont          ) \
	ROMSEG_DECL_SEG(sfxctl,             0x80a250,  0x7f87e0,  0x7fc670,  0x2fb80,  preprocessALBankFile    ) \
	ROMSEG_DECL_SEG(sfxtbl,             0x839dd0,  0x828360,  0x82c1f0,  0x4c2160, NULL                    ) \
	ROMSEG_DECL_SEG(seqctl,             0xcfbf30,  0xcea4c0,  0xcee350,  0xa060,   preprocessALBankFile    ) \
	ROMSEG_DECL_SEG(seqtbl,             0xd05f90,  0xcf4520,  0xcf83b0,  0x17c070, NULL                    ) \
	ROMSEG_DECL_SEG(sequences,          0xe82000,  0xe70590,  0xe74420,  0x563a0,  preprocessSequences     ) \
	ROMSEG_DECL_SEG(texturesdata,       0x1d65f40, 0x1d5ca20, 0x1d61f90, 0x0,      NULL                    ) \
	ROMSEG_DECL_SEG(textureslist,       0x1ff7ca0, 0x1fee780, 0x1ff68f0, 0x0,      preprocessTexturesList  ) \
	ROMSEG_DECL_SEG(copyright,          0x1ffea20, 0x1ff5500, 0x1ffd6b0, 0xb30,    NULL                    ) \
	ROMSEG_DECL_SEG(fontjpn,            0x0,       0x0,       0x178c40,  0x17920,  preprocessJpnFont       )

// declare the vars first

#undef ROMSEG_DECL_SEG
#define ROMSEG_DECL_SEG(name, ofs_ntsc, ofs_pal, ofs_jpn, size, preproc) u8 *ROMSEG_START(name), *ROMSEG_END(name);
ROMSEG_LIST()

// this is part of the animations seg and as such does not follow the naming convention
// these are set in preprocessAnimations
u8 *_animationsTableRomStart;
u8 *_animationsTableRomEnd;

// then build the table

#undef ROMSEG_DECL_SEG

#if VERSION == VERSION_NTSC_FINAL
#define ROMSEG_DECL_SEG(seg_name, ofs_ntsc, ofs_pal, ofs_jpn, seg_size, preproc) \
	{ .segstart = &ROMSEG_START(seg_name), .segend = &ROMSEG_END(seg_name), .name = #seg_name, \
	  .data = (u8 *)ofs_ntsc, .size = seg_size, .preprocess = preproc },
#elif VERSION == VERSION_PAL_FINAL
#define ROMSEG_DECL_SEG(seg_name, ofs_ntsc, ofs_pal, ofs_jpn, seg_size, preproc) \
	{ .segstart = &ROMSEG_START(seg_name), .segend = &ROMSEG_END(seg_name), .name = #seg_name, \
	  .data = (u8 *)ofs_pal, .size = seg_size, .preprocess = preproc },
#elif VERSION == VERSION_JPN_FINAL
#define ROMSEG_DECL_SEG(seg_name, ofs_ntsc, ofs_pal, ofs_jpn, seg_size, preproc) \
	{ .segstart = &ROMSEG_START(seg_name), .segend = &ROMSEG_END(seg_name), .name = #seg_name, \
	  .data = (u8 *)ofs_jpn, .size = seg_size, .preprocess = preproc },
#endif

static struct romfile romSegs[] = {
	ROMSEG_LIST()
	{ 0 },
};

/* the game sets g_LoadType to the type of file it expects,              */
/* so we can hijack that in fileLoad and automatically byteswap the file */
static preprocessfunc filePreprocFuncs[] = {
	/* LOADTYPE_NONE  */ NULL,
	/* LOADTYPE_BG    */ NULL, // loaded in parts
	/* LOADTYPE_TILES */ preprocessTilesFile,
	/* LOADTYPE_LANG  */ preprocessLangFile,
	/* LOADTYPE_SETUP */ preprocessSetupFile,
	/* LOADTYPE_PADS  */ preprocessPadsFile,
	/* LOADTYPE_MODEL */ preprocessModelFile,
	/* LOADTYPE_GUN   */ preprocessGunFile,
};

static inline void romdataWrongRomError(const char *fmt, ...)
{
	char reason[1024];
	reason[0] = '\0';

	va_list args;
	va_start(args, fmt);
	vsnprintf(reason, sizeof(reason), fmt, args);
	va_end(args);

	sysFatalError("Wrong ROM file.\n%s\nEnsure that you have the correct " ROMDATA_ROM_DESC " ROM in z64 format.", reason);
}

static inline void romdataLoadRom(void)
{
	sysLogPrintf(LOG_NOTE, "ROM file: %s", romName);
	ROMDATA_CHECKPOINT();

#ifdef PLATFORM_PS2
	u8 header[64];
	static u8 inputScratch[ROMDATA_RZIP_INPUT_CHUNK];
	u32 dataSegLen = 0;
	u32 compressedSize = 0;
	const char *path = fsFullPath(romName);

	g_RomFile = NULL;
	romSourceClose(&romSource);

	sysLogPrintf(LOG_NOTE, "ROM source: opening %s", path);
	ROMDATA_CHECKPOINT();
	if (!romSourceOpenFile(&romSource, path)) {
		sysFatalError("Could not open ROM file %s.\nEnsure that it is in the %s directory.", romName, fsFullPath(""));
	}

	g_RomFileSize = romSourceGetSize(&romSource);
	sysLogPrintf(LOG_NOTE, "ROM source: opened size=%u", g_RomFileSize);
	ROMDATA_CHECKPOINT();

	if (!romSourceReadAt(&romSource, 0, header, sizeof(header))) {
		romdataWrongRomError("Could not read the ROM header.");
	}

	// zips are not guaranteed to start with PK, but might as well at least try
	if (!memcmp(header, "PK", 2) || !memcmp(header, "Rar", 3) || !memcmp(header, "7z", 2)) {
		romdataWrongRomError("Your ROM is in an archive file. Please extract it.");
	}

	if (g_RomFileSize != ROMDATA_ROM_SIZE) {
		romdataWrongRomError("ROM size does not match: expected: %u, got: %u.", ROMDATA_ROM_SIZE, g_RomFileSize);
	}

	if (memcmp(header + 0x3b, ROMDATA_ROM_ID, 4) ||
		memcmp(header + 0x20, ROMDATA_ROM_TITLE, sizeof(ROMDATA_ROM_TITLE) - 1)) {
		romdataWrongRomError("ROM header does not match.");
	}
	sysLogPrintf(LOG_NOTE, "ROM source: NTSC-final header validated");
	ROMDATA_CHECKPOINT();

	if (!romSourceGetRzip1173Size(&romSource, ROMDATA_DATA_OFS, &dataSegLen)) {
		romdataWrongRomError("Data segment is not 1173-compressed.");
	}

	if (dataSegLen < ROMDATA_FILES_OFS + 12 || dataSegLen > ROMDATA_MAX_DATA_SEG_SIZE) {
		romdataWrongRomError("Data segment size is invalid (%u).", dataSegLen);
	}
	sysLogPrintf(LOG_NOTE,
		"ROM source: RZIP data segment output=%u input_chunk=%u",
		dataSegLen, (u32)sizeof(inputScratch));
	ROMDATA_CHECKPOINT();

	romDataSeg = sysMemAlloc(dataSegLen);
	if (!romDataSeg) {
		sysFatalError("Could not allocate %u bytes for data segment.", dataSegLen);
	}

	sysLogPrintf(LOG_NOTE,
		"ROM source: inflate begin offset=%08x output=%u",
		ROMDATA_DATA_OFS, dataSegLen);
	ROMDATA_CHECKPOINT();
	if (!romSourceInflate1173(&romSource, ROMDATA_DATA_OFS, romDataSeg,
		dataSegLen, inputScratch, sizeof(inputScratch), &compressedSize)) {
		sysMemFree(romDataSeg);
		romDataSeg = NULL;
		sysFatalError("Could not stream-inflate the ROM data segment.");
	}

	romDataSegSize = dataSegLen;
	sysLogPrintf(LOG_NOTE, "ROM source is file-backed; data segment %u bytes from %u compressed bytes",
		dataSegLen, compressedSize);
	ROMDATA_CHECKPOINT();
#else
	g_RomFile = fsFileLoad(romName, &g_RomFileSize);

	if (!g_RomFile) {
		sysFatalError("Could not open ROM file %s.\nEnsure that it is in the %s directory.", romName, fsFullPath(""));
	}

	// zips are not guaranteed to start with PK, but might as well at least try
	if (g_RomFileSize > 2 && (!memcmp(g_RomFile, "PK", 2) || !memcmp(g_RomFile, "Rar", 3) || !memcmp(g_RomFile, "7z", 2))) {
		romdataWrongRomError("Your ROM is in an archive file. Please extract it.");
	}

	if (g_RomFileSize != ROMDATA_ROM_SIZE) {
		romdataWrongRomError("ROM size does not match: expected: %u, got: %u.", ROMDATA_ROM_SIZE, g_RomFileSize);
	}

	if (memcmp(g_RomFile + 0x3b, ROMDATA_ROM_ID, 4) || memcmp(g_RomFile + 0x20, ROMDATA_ROM_TITLE, sizeof(ROMDATA_ROM_TITLE) - 1)) {
		romdataWrongRomError("ROM header does not match.");
	}

	// inflate the compressed data segment since that's where some useful stuff is

	u8 *zipped = g_RomFile + ROMDATA_DATA_OFS;
	if (!rzipIs1173(zipped)) {
		romdataWrongRomError("Data segment is not 1173-compressed.");
	}

	const u32 dataSegLen = ((u32)zipped[2] << 16) | ((u32)zipped[3] << 8) | (u32)zipped[4];
	if (dataSegLen < ROMDATA_FILES_OFS) {
		romdataWrongRomError("Data segment too small (%u), need at least %u.", dataSegLen, ROMDATA_FILES_OFS);
	}

	u8 *dataSeg = sysMemAlloc(dataSegLen);
	if (!dataSeg) {
		sysFatalError("Could not allocate %u bytes for data segment.", dataSegLen);
	}

	u8 scratch[5 * 1024];
	if (rzipInflate(zipped, dataSeg, scratch) < 0) {
		free(dataSeg);
		sysFatalError("Could not inflate data segment.");
	}

	romDataSeg = dataSeg;
	romDataSegSize = dataSegLen;
#endif
}

static inline void romdataUpdateSegStartEnd(struct romfile* seg)
{
	if (seg->segstart) {
		*seg->segstart = seg->data;
	}

	if (seg->segend) {
		*seg->segend = seg->data + seg->size;
	}
}

static inline void romdataInitSegment(struct romfile *seg)
{
	if (!seg->romoffset) {
		// unused in this ROM, skip it
		sysLogPrintf(LOG_NOTE, "skipping segment %s", seg->name);
		return;
	}

	if (!seg->size) {
		// size unknown
		if (seg[1].name) {
			// use next segment's base to calculate
			seg->size = seg[1].romoffset - seg->romoffset;
		} else {
			// this is the last segment, calculate based on rom size
			seg->size = g_RomFileSize - seg->romoffset;
		}
	}

	if (seg->romoffset > g_RomFileSize || seg->size > g_RomFileSize - seg->romoffset) {
		sysFatalError("ROM segment %s has an invalid range %08x..%08x.",
			seg->name, seg->romoffset, seg->romoffset + seg->size);
	}

	// check if we have an external replacement and load it if so
	char tmp[FS_MAXPATH];
	snprintf(tmp, sizeof(tmp), ROMDATA_SEGDIR "/%s", seg->name);
	u8 *newData = NULL;
	const s32 extFileSize = fsFileSize(tmp);
	if (extFileSize > 0) {
		newData = fsFileLoad(tmp, &seg->size);
		seg->owned = newData != NULL;
	}

	if (!newData) {
		// no external data, use the ROM source
#ifdef PLATFORM_PS2
		sysLogPrintf(LOG_NOTE,
			"ROM segment allocate: %s size=%u", seg->name, seg->size);
		ROMDATA_CHECKPOINT();
		newData = sysMemAlloc(seg->size);
		if (!newData) {
			sysFatalError("Could not allocate %u bytes for ROM segment %s.",
				seg->size, seg->name);
		}

		sysLogPrintf(LOG_NOTE,
			"ROM segment read begin: %s offset=%08x size=%u pointer=%p",
			seg->name, seg->romoffset, seg->size, newData);
		ROMDATA_CHECKPOINT();
		if (!romSourceReadAt(&romSource, seg->romoffset, newData, seg->size)) {
			sysMemFree(newData);
			newData = NULL;
		} else {
			sysLogPrintf(LOG_NOTE,
				"ROM segment read end: %s pointer=%p", seg->name, newData);
			ROMDATA_CHECKPOINT();
		}

		if (newData) {
			seg->owned = 1;
			seg->source = SRC_ROM;
			sysLogPrintf(LOG_NOTE, "loading segment %s from file-backed ROM (offset %08x size %u pointer %p)",
				seg->name, seg->romoffset, seg->size, newData);
		}
#else
		// Resident desktop ROM: point directly into the image.
		if (g_RomFile) {
			newData = g_RomFile + seg->romoffset;
			seg->source = SRC_ROM;
			sysLogPrintf(LOG_NOTE, "loading segment %s from ROM (offset %08x pointer %p)", seg->name, seg->romoffset, newData);
		}
#endif
		if (!newData) {
			sysFatalError("No ROM or external file for segment:\n%s", seg->name);
		}
	} else {
		// loaded external data
		seg->source = SRC_EXTERNAL;
		sysLogPrintf(LOG_NOTE, "loading segment %s from file (pointer %p)", seg->name, newData);
	}

	seg->data = newData;

	romdataUpdateSegStartEnd(seg);

	// call the post load function if any
	if (seg->preprocess && !seg->preprocessed) {
		sysLogPrintf(LOG_NOTE,
			"ROM segment preprocess begin: %s input=%u pointer=%p",
			seg->name, seg->size, seg->data);
		ROMDATA_CHECKPOINT();
		newData = seg->preprocess(seg->data, seg->size, &seg->size);
		sysLogPrintf(LOG_NOTE,
			"ROM segment preprocess end: %s output=%u replacement=%p",
			seg->name, seg->size, newData);
		ROMDATA_CHECKPOINT();

		if (newData) {
			if (seg->owned) {
				sysMemFree(seg->data);
			}
			seg->data = newData;
			seg->owned = 1;
			romdataUpdateSegStartEnd(seg);
		}
		
		seg->preprocessed = 1;
	}
}

static inline s32 romdataLoadExternalFileList(void)
{
	romDataSeg = fsFileLoad("filenames.lst", &romDataSegSize); // this null terminates the file by itself
	if (!romDataSeg || !romDataSegSize) {
		return 0;
	}

	s32 n = 1;
	char *p = (char *)romDataSeg;
	while (*p && n < ROMDATA_MAX_FILES) {
		// skip whitespace
		while (*p && isspace(*p)) ++p;
		if (*p) {
			const char *start = p;
			// skip to next whitespace or end of file
			while (*p && !isspace(*p)) ++p;
			// null terminate the name if needed
			if (*p) {
				*p++ = '\0';
			}
			fileSlots[n++].name = start;
		}
	}

	return n - 1;
}

#ifdef PLATFORM_PS2
static bool romdataFindSourceStringLength(u32 offset, u32 *outLength)
{
	u8 chunk[64];
	u32 length = 0;

	while (length <= FS_MAXPATH && offset + length < g_RomFileSize) {
		const u32 remaining = g_RomFileSize - (offset + length);
		const u32 nameRemaining = FS_MAXPATH + 1u - length;
		u32 amount = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
		amount = amount < nameRemaining ? amount : nameRemaining;

		if (!romSourceReadAt(&romSource, offset + length, chunk, amount)) {
			return false;
		}

		const u8 *terminator = memchr(chunk, '\0', amount);
		if (terminator) {
			*outLength = length + (u32)(terminator - chunk);
			return true;
		}

		length += amount;
	}

	return false;
}

static u32 romdataInitFileNames(u32 nameTableOffset, u32 fileCount)
{
	const u32 offsetsSize = (fileCount + 2u) * sizeof(u32);
	u32 *nameOffsets = NULL;
	u32 maxNameOffset = 0;
	u32 lastNameLength = 0;
	u32 tableSize;

	if (nameTableOffset > g_RomFileSize || offsetsSize > g_RomFileSize - nameTableOffset) {
		sysFatalError("ROM filename offset table is outside the ROM.");
	}

	nameOffsets = sysMemAlloc(offsetsSize);
	if (!nameOffsets || !romSourceReadAt(&romSource, nameTableOffset, nameOffsets, offsetsSize)) {
		if (nameOffsets) {
			sysMemFree(nameOffsets);
		}
		sysFatalError("Could not read the ROM filename offset table.");
	}

	for (u32 i = 1; i <= fileCount; ++i) {
		const u32 relative = PD_BE32(nameOffsets[i]);

		if (!relative || relative < offsetsSize || relative >= g_RomFileSize - nameTableOffset) {
			sysMemFree(nameOffsets);
			sysFatalError("ROM filename %u has invalid relative offset %08x.", i, relative);
		}

		if (relative > maxNameOffset) {
			maxNameOffset = relative;
		}
	}

	if (PD_BE32(nameOffsets[fileCount + 1u]) != 0 ||
		!romdataFindSourceStringLength(nameTableOffset + maxNameOffset, &lastNameLength)) {
		sysMemFree(nameOffsets);
		sysFatalError("ROM filename table is unterminated or inconsistent.");
	}

	tableSize = maxNameOffset + lastNameLength + 1u;
	romNameTable = sysMemAlloc(tableSize);

	if (!romNameTable || !romSourceReadAt(&romSource, nameTableOffset, romNameTable, tableSize)) {
		if (romNameTable) {
			sysMemFree(romNameTable);
			romNameTable = NULL;
		}
		sysMemFree(nameOffsets);
		sysFatalError("Could not materialize the ROM filename table.");
	}

	for (u32 i = 1; i <= fileCount; ++i) {
		const u32 relative = PD_BE32(nameOffsets[i]);
		const u32 remaining = tableSize - relative;

		if (!memchr(romNameTable + relative, '\0', remaining)) {
			sysMemFree(nameOffsets);
			sysFatalError("ROM filename %u is not terminated.", i);
		}

		fileSlots[i].name = (const char *)romNameTable + relative;
	}

	sysMemFree(nameOffsets);
	return tableSize;
}
#endif

static inline void romdataInitFiles(void)
{
#ifdef PLATFORM_PS2
	const u32 tableEntries = (romDataSegSize - ROMDATA_FILES_OFS) / sizeof(u32);
	const u32 *offsets = (const u32 *)(romDataSeg + ROMDATA_FILES_OFS);
	u32 nameTableOffset = 0;
	u32 fileCount = 0;
	u32 emptyFileCount = 0;

	for (u32 i = 1; i + 1 < tableEntries && i < ROMDATA_MAX_FILES; ++i) {
		const u32 offset = PD_BE32(offsets[i]);
		const u32 nextOffset = PD_BE32(offsets[i + 1]);

		if (!offset) {
			break;
		}

		if (!nextOffset) {
			nameTableOffset = offset;
			fileCount = i - 1u;
			break;
		}

		if (!romdataFileExtentValid(offset, nextOffset, g_RomFileSize)) {
			sysFatalError("ROM file %u has invalid extent %08x..%08x.", i, offset, nextOffset);
		}

		if (offset == nextOffset) {
			emptyFileCount++;
		}

		fileSlots[i].romoffset = offset;
		fileSlots[i].romsize = nextOffset - offset;
		fileSlots[i].data = NULL;
		fileSlots[i].size = fileSlots[i].romsize;
		fileSlots[i].source = SRC_UNLOADED;
		fileSlots[i].preprocessed = 0;
		fileSlots[i].owned = 0;
	}

	if (!nameTableOffset || fileCount != NUM_FILES - 1u || nameTableOffset >= g_RomFileSize) {
		sysFatalError("ROM file table does not contain a valid filename table.");
	}

	const u32 nameTableSize = romdataInitFileNames(nameTableOffset, fileCount);
	sysLogPrintf(LOG_NOTE,
		"ROM file table: %u file extents (%u empty), %u-byte resident filename table",
		fileCount, emptyFileCount, nameTableSize);
#else
	if (!g_RomFile) {
		// no ROM; try to load the file name list from disk
		if (!romdataLoadExternalFileList()) {
			sysFatalError("No ROM file or external filename table found.");
		}
		return;
	}

	// the file offset table is in the data seg
	const u32 *offsets = (u32 *)(romDataSeg + ROMDATA_FILES_OFS);
	u32 i;
	for (i = 1; offsets[i]; ++i) {
		if (offsets + i + 1 < (u32 *)(romDataSeg + romDataSegSize)) {
			const u32 nextofs = PD_BE32(offsets[i + 1]);
			const u32 ofs = PD_BE32(offsets[i]);
			fileSlots[i].data = g_RomFile + ofs;
			fileSlots[i].size = nextofs - ofs;
			fileSlots[i].romoffset = ofs;
			fileSlots[i].romsize = nextofs - ofs;
			fileSlots[i].source = SRC_UNLOADED;
			fileSlots[i].preprocessed = 0;
		}
	}

	// last offset is to the name table
	const u32 *nameOffsets = (u32 *)(g_RomFile + PD_BE32(offsets[i - 1]));
	for (i = 1; nameOffsets[i]; ++i) {
		const u32 ofs = PD_BE32(nameOffsets[i]);
		fileSlots[i].name = (const char *)nameOffsets + ofs; // ofs is relative to the start of the name table
	}
#endif
}

static inline struct romfile *romdataGetSeg(const char *name)
{
	struct romfile *seg = romSegs;
	while (seg->name && strcmp(name, seg->name)) {
		++seg;
	}
	return seg;
}

s32 romdataInit(void)
{
	const char *altRomName = sysArgGetString("--rom-file");
	if (altRomName) {
		romName = altRomName;
	}

	for (struct romfile *seg = romSegs; seg->name; ++seg) {
		seg->romoffset = (u32)(uintptr_t)seg->data;
	}

	romdataLoadRom();
	ROMDATA_CHECKPOINT();

	// set segments to point to the rom or load them externally
	for (struct romfile *seg = romSegs; seg->name; ++seg) {
		sysLogPrintf(LOG_NOTE,
			"ROM segment begin: %s offset=%08x declared=%u",
			seg->name, seg->romoffset, seg->size);
		ROMDATA_CHECKPOINT();
		romdataInitSegment(seg);
		sysLogPrintf(LOG_NOTE,
			"ROM segment ready: %s size=%u source=%d owned=%d",
			seg->name, seg->size, seg->source, seg->owned);
		ROMDATA_CHECKPOINT();
	}

	// load file table from the files segment
	sysLogPrintf(LOG_NOTE, "ROM file table initialisation begin");
	ROMDATA_CHECKPOINT();
	romdataInitFiles();
	ROMDATA_CHECKPOINT();

#ifdef PLATFORM_PS2
	// The decompressed data segment is only a bootstrap producer for the file
	// extent table. Names now live in their compact permanent table and lazy
	// files retain stable source offsets, so release this transient buffer.
	sysMemFree(romDataSeg);
	romDataSeg = NULL;
	romDataSegSize = 0;
#endif

	sysLogPrintf(LOG_NOTE, "romdataInit: loaded rom, size = %u", g_RomFileSize);

	return 0;
}

static inline bool romdataCheckGbcRomContents(const u8 *gbcRomFile, const u32 gbcRomSize)
{
	if (gbcRomSize != GBC_ROM_SIZE) {
		return false;
	}

	// ROM title
	if (memcmp(gbcRomFile + 0x134, "PerfDark   VPDE", 15) != 0) {
		return false;
	}

	// Licensee code
	if (memcmp(gbcRomFile + 0x144, "4Y", 2) != 0) {
		return false;
	}

	// Header and global checksums
	if (gbcRomFile[0x14D] != 0xA1 || gbcRomFile[0x14E] != 0xAD || gbcRomFile[0x14F] != 0x0F) {
		return false;
	}

	return true;
}

s32 romdataCheckGbcRom(void)
{
	if (fsFileSize(GBC_ROM_NAME) < 0) {
		// bail early if it doesn't exist to avoid generating error messages
		return false;
	}

	u32 gbcRomSize = 0;
	u8 *gbcRomFile = fsFileLoad(GBC_ROM_NAME, &gbcRomSize);
	if (!gbcRomFile) {
		return false;
	}

	const bool ret = romdataCheckGbcRomContents(gbcRomFile, gbcRomSize);
	sysMemFree(gbcRomFile);

	if (ret) {
		sysLogPrintf(LOG_NOTE, "romdataCheckGbcRom: valid GBC rom found");
	}

	return ret;
}

s32 romdataFileGetSize(s32 fileNum)
{
	if (fileNum < 1 || fileNum >= ROMDATA_MAX_FILES) {
		sysLogPrintf(LOG_ERROR, "romdataFileGetSize: invalid file num %d", fileNum);
		return -1;
	}

#ifdef PLATFORM_PS2
	if (fileSlots[fileNum].source != SRC_UNLOADED) {
		return fileSlots[fileNum].size;
	}

	if (fileSlots[fileNum].name) {
		char tmp[FS_MAXPATH] = { 0 };
		snprintf(tmp, sizeof(tmp), ROMDATA_FILEDIR "/%s", fileSlots[fileNum].name);
		const s32 externalSize = fsFileSize(tmp);
		if (externalSize > 0) {
			return externalSize;
		}
	}

	if (fileSlots[fileNum].romoffset) {
		return fileSlots[fileNum].romsize;
	}
#else
	// Ensure any external files are loaded and we use their size.
	if (romdataFileLoad(fileNum, NULL)) {
		return fileSlots[fileNum].size;
	}
#endif

	sysLogPrintf(LOG_ERROR, "romdataFileGetSize: could not load file num %d", fileNum);
	return -1;
}

u8 *romdataFileGetData(s32 fileNum)
{
	return romdataFileLoad(fileNum, NULL);
}

u8 *romdataFileLoad(s32 fileNum, u32 *outSize)
{
	if (fileNum < 1 || fileNum >= ROMDATA_MAX_FILES) {
		sysLogPrintf(LOG_ERROR, "romdataFileLoad: invalid file num %d", fileNum);
		return NULL;
	}

	u8 *out = NULL;

	// try to load external file
	if (fileSlots[fileNum].source == SRC_UNLOADED) {
		if (fileSlots[fileNum].name) {
			char tmp[FS_MAXPATH] = { 0 };
			snprintf(tmp, sizeof(tmp), ROMDATA_FILEDIR "/%s", fileSlots[fileNum].name);
			if (fsFileSize(tmp) > 0) {
				u32 size = 0;
				out = fsFileLoad(tmp, &size);
				if (out && size) {
					sysLogPrintf(LOG_NOTE, "file %d (%s) loaded externally", fileNum, fileSlots[fileNum].name);
					fileSlots[fileNum].data = out;
					fileSlots[fileNum].size = size;
					fileSlots[fileNum].source = SRC_EXTERNAL;
					fileSlots[fileNum].owned = 1;
					// external file; do not apply patches to this
					fileSlots[fileNum].numpatches = 0;
				}
			}
		}

		if (!out) {
#ifdef PLATFORM_PS2
			struct romfile *file = &fileSlots[fileNum];

			if (file->romoffset && file->romsize == 0) {
				/* Desktop builds expose a non-null pointer into the resident ROM
				 * for empty files. Preserve that contract without retaining 32 MB. */
				file->data = &romEmptyFile;
				file->size = 0;
				file->source = SRC_ROM;
				file->owned = 0;
				out = file->data;
				sysLogPrintf(LOG_NOTE, "file %d (%s) is an empty ROM extent",
					fileNum, file->name ? file->name : "unnamed");
			} else {
				if (file->romoffset && file->romsize &&
					file->romoffset <= g_RomFileSize &&
					file->romsize <= g_RomFileSize - file->romoffset) {
					out = sysMemAlloc(file->romsize);
					if (out && !romSourceReadAt(&romSource, file->romoffset, out, file->romsize)) {
						sysMemFree(out);
						out = NULL;
					}
				}

				if (out) {
					file->data = out;
					file->size = file->romsize;
					file->source = SRC_ROM;
					file->owned = 1;
					sysLogPrintf(LOG_NOTE, "file %d (%s) loaded from ROM offset %08x (%u bytes)",
						fileNum, file->name, file->romoffset, file->romsize);
				} else {
					sysLogPrintf(LOG_ERROR, "file %d (%s) could not be read from the ROM source",
						fileNum, file->name ? file->name : "unnamed");
				}
			}
#else
			// Rebuild the resident-ROM view after a prior external override.
			if (g_RomFile && fileSlots[fileNum].romoffset && fileSlots[fileNum].romsize) {
				fileSlots[fileNum].data = g_RomFile + fileSlots[fileNum].romoffset;
				fileSlots[fileNum].size = fileSlots[fileNum].romsize;
				fileSlots[fileNum].source = SRC_ROM;
				out = fileSlots[fileNum].data;
			}
#endif
		}
	}

	if (!out) {
		out = fileSlots[fileNum].data;
	}

	if (out && outSize) {
		*outSize = fileSlots[fileNum].size;
	}

	return out;
}

void romdataFilePreprocess(s32 fileNum, s32 loadType, u8 *data, u32 size, u32 *outSize)
{
	if (fileNum < 1 || fileNum >= ROMDATA_MAX_FILES) {
		sysLogPrintf(LOG_ERROR, "romdataFilePreprocess: invalid file num %d", fileNum);
		return;
	}

	if (data && size /* && !fileSlots[fileNum].preprocessed*/) {
		if (loadType > 0 && loadType < ARRAYCOUNT(filePreprocFuncs) && filePreprocFuncs[loadType]) {
			// apply patches
			for (u32 i = 0; i < fileSlots[fileNum].numpatches; ++i) {
				const struct romfilepatch *p = &fileSlots[fileNum].patches[i];
				if (!memcmp(data + p->ofs, p->src, p->len)) {
					memcpy(data + p->ofs, p->dst, p->len);
					sysLogPrintf(LOG_NOTE, "file %d (%s) patched at offset 0x%x", fileNum, fileSlots[fileNum].name, p->ofs);
				}
			}
			// then preprocess
			filePreprocFuncs[loadType](data, size, outSize);
			// fileSlots[fileNum].preprocessed = 1;
		}
	}
}

void romdataFileFree(s32 fileNum)
{
	if (fileNum < 1 || fileNum >= ROMDATA_MAX_FILES) {
		sysLogPrintf(LOG_ERROR, "fsFileFree: invalid file num %d", fileNum);
		return;
	}

	if (fileSlots[fileNum].owned) {
		sysMemFree(fileSlots[fileNum].data);
		fileSlots[fileNum].data = NULL;
		fileSlots[fileNum].owned = 0;
	}

	fileSlots[fileNum].size = fileSlots[fileNum].romsize;

	fileSlots[fileNum].source = SRC_UNLOADED;
}

const char *romdataFileGetName(s32 fileNum)
{
	if (fileNum < 1 || fileNum >= ROMDATA_MAX_FILES) {
		return NULL;
	}
	return fileSlots[fileNum].name;
}

s32 romdataFileGetNumForName(const char *name)
{
	if (!name || !name[0]) {
		return -1;
	}

	for (s32 i = 0; i < ROMDATA_MAX_FILES; ++i) {
		if (fileSlots[i].name && !strcmp(fileSlots[i].name, name)) {
			return i;
		}
	}

	return -1;
}

u8 *romdataSegGetData(const char *segName)
{
	return romdataGetSeg(segName)->data;
}

u8 *romdataSegGetDataEnd(const char *segName)
{
	struct romfile *seg = romdataGetSeg(segName);
	return seg->data + seg->size;
}

u32 romdataSegGetSize(const char *segName)
{
	return romdataGetSeg(segName)->size;
}

u32 romdataFileGetEstimatedSize(const u32 size, const u32 loadtype)
{
#ifdef PLATFORM_64BIT
	switch (loadtype) {
	case LOADTYPE_BG:	   return (u32)(size * 1.1f);
	case LOADTYPE_TILES: return (u32)(size * 1.1f);
	case LOADTYPE_LANG:  return (u32)(size * 1.3f);
	case LOADTYPE_SETUP: return (u32)(size * 1.5f);
	case LOADTYPE_PADS:  return (u32)(size * 1.7f);
	case LOADTYPE_MODEL: return (u32)(size * 1.7f);
	case LOADTYPE_GUN: return (u32)(size * 1.7f);
	default:
		sysLogPrintf(LOG_WARNING, "romdataFileGetEstimatedSize: wrong loadtype %d", loadtype);
	}
#else
	(void)loadtype;
#endif
	return size;
}
