#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include <PR/ultratypes.h>
#include "config.h"
#include "system.h"
#include "platform.h"
#include "utils.h"
#include "fs.h"
#ifdef PLATFORM_PS2
#include "path_ps2.h"
#endif
#ifdef PLATFORM_WIN32
#include <direct.h>
#endif

#define DEFAULT_BASEDIR_NAME "data"

static char baseDir[FS_MAXPATH + 1]; // replaces $B
static char modDir[FS_MAXPATH + 1];  // replaces $M
static char saveDir[FS_MAXPATH + 1]; // replaces $S
static char homeDir[FS_MAXPATH + 1]; // replaces $H
static char exeDir[FS_MAXPATH + 1];  // replaces $E

static s32 fsPathIsWritable(const char *path)
{
#ifdef PLATFORM_WIN32
	// on windows access() on directories will only check if the directory exists, so
	char tmp[FS_MAXPATH + 1] = { 0 };
	snprintf(tmp, sizeof(tmp), "%s/.tmp", path);
	FILE *f = fopen(tmp, "wb");
	if (f) {
		fclose(f);
		remove(tmp);
		return 1;
	}
	return 0;
#else
	return (access(path, W_OK) == 0);
#endif
}

s32 fsPathIsAbsolute(const char *path)
{
	if (!path || !path[0]) {
		return false;
	}

	if (path[0] == '/' || (isalpha((unsigned char)path[0]) && path[1] == ':')) {
		return true;
	}

#ifdef PLATFORM_PS2
	/* mass:, mc0:, host:, pfs0: and other IOP devices are absolute roots. */
	return ps2PathHasDevicePrefix(path);
#else
	return false;
#endif
}

s32 fsPathIsCwdRelative(const char *path)
{
	if (!path) {
		return false;
	}

	// ., .., ./, ../
	return (path[0] == '.' && (path[1] == '.' || path[1] == '/' || path[1] == '\\' || path[1] == '\0'));
}

const char *fsFullPath(const char *relPath)
{
	static char pathBuf[FS_MAXPATH + 1];

	if (!relPath) {
		return "";
	}

	if (relPath[0] == '$') {
		// expandable placeholder $X; will be replaced with the corresponding path, if any
		const char *expStr = NULL;
		switch (relPath[1]) {
			case 'E': expStr = exeDir; break;
			case 'H': expStr = homeDir; break;
			case 'M': expStr = modDir; break;
			case 'B': expStr = baseDir; break;
			case 'S': expStr = saveDir; break;
			default: break;
		}
		if (expStr) {
			if (expStr[0]) {
				snprintf(pathBuf, sizeof(pathBuf), "%s%s", expStr, relPath + 2);
				return pathBuf;
			}
		}
		// couldn't expand anything, return as is
		return relPath;
	} else if (!baseDir[0] || fsPathIsAbsolute(relPath) || fsPathIsCwdRelative(relPath)) {
		// user explicitly wants working directory or this is an absolute path or we have no baseDir set up yet
		return relPath;
	}

	// path relative to mod or base dir; this will be a read request, so check where the file actually is
	if (modDir[0]) {
		snprintf(pathBuf, sizeof(pathBuf), "%s/%s", modDir, relPath);
		if (fsFileSize(pathBuf) >= 0) {
			return pathBuf;
		}
	}
	// fall back to basedir
	snprintf(pathBuf, sizeof(pathBuf), "%s/%s", baseDir, relPath);
	return pathBuf;
}

s32 fsInit(void)
{
	sysGetExecutablePath(exeDir, FS_MAXPATH);

	// if this is set, default to exe path for everything
	const s32 portable = sysArgCheck("--portable");
	if (portable) {
		strcpy(homeDir, exeDir);
	} else {
		sysGetHomePath(homeDir, FS_MAXPATH);
	}

	// get path to base dir and expand it if needed
	const char *path = sysArgGetString("--basedir");
	if (!path) {
#ifdef PLATFORM_PS2
		/* PS2 launchers do not promise a useful current working directory. */
		path = "$E";
#else
		// check if there's a `data` directory in working directory or homeDir, otherwise default to exe directory
		path = "$E/" DEFAULT_BASEDIR_NAME;
		if (!portable) {
			if (fsFileSize("./" DEFAULT_BASEDIR_NAME) >= 0) {
				path = "./" DEFAULT_BASEDIR_NAME;
			} else if (fsFileSize("$H/" DEFAULT_BASEDIR_NAME) >= 0) {
				path = "$H/" DEFAULT_BASEDIR_NAME;
			}
		}
#endif
	}
	snprintf(baseDir, sizeof(baseDir), "%s", fsFullPath(path));

	// get path to mod dir and expand it if needed
	// mod directory is overlaid on top of base directory
	path = sysArgGetString("--moddir");
	if (path) {
		if (fsPathIsAbsolute(path) || fsPathIsCwdRelative(path) || path[0] == '$') {
			// path is explicit; check as-is
			if (fsFileSize(path) >= 0) {
				snprintf(modDir, sizeof(modDir), "%s", fsFullPath(path));
			}
		} else {
			// path is relative to workdir; try to find it
			const char *priority[] = { ".", "$E", "$H" };
			for (s32 i = 0; i < 2 + (portable != 0); ++i) {
				char *tmp = strFmt("%s/%s", priority[i], path);
				if (fsFileSize(tmp) >= 0) {
					snprintf(modDir, sizeof(modDir), "%s", fsFullPath(tmp));
					break;
				}
			}
		}
		if (!modDir[0]) {
			sysLogPrintf(LOG_WARNING, "could not find specified moddir `%s`", path);
		}
	}

	// get path to save dir and expand it if needed
	path = sysArgGetString("--savedir");
	if (!path) {
#ifdef PLATFORM_PS2
		/* Store config and saves on the same mounted device as the executable. */
		path = "$E";
#else
		if (portable) {
			path = "$E";
		} else {
#if defined(PLATFORM_LINUX) || defined(PLATFORM_OSX)
			// check if there's a config in the working directory, otherwise default to homeDir
			if (fsFileSize("./" CONFIG_FNAME) >= 0) {
				path = ".";
			} else {
				path = "$H";
			}
#else
			// check if working directory is writable, otherwise default to homeDir
			if (fsPathIsWritable("./")) {
				path = ".";
			} else {
				sysLogPrintf(LOG_WARNING, "cannot write to working directory, will use %s for saves instead", homeDir);
				path = "$H";
			}
#endif
		}
#endif
	}

	snprintf(saveDir, sizeof(saveDir), "%s", fsFullPath(path));

	if (modDir[0]) {
		sysLogPrintf(LOG_NOTE, " mod dir: %s", modDir);
	}
	sysLogPrintf(LOG_NOTE, "base dir: %s", baseDir);
	sysLogPrintf(LOG_NOTE, "save dir: %s", saveDir);

	return 0;
}

const char *fsGetModDir(void)
{
	return modDir[0] ? modDir : NULL;
}

s32 fsFileLoadTo(const char *name, void *dst, u32 dstSize)
{
	if (!name || !dst) {
		return -1;
	}

	const char *fullName = fsFullPath(name);

	FILE *f = fopen(fullName, "rb");
	if (!f) {
		return -1;
	}

	if (fseek(f, 0, SEEK_END) != 0) {
		sysLogPrintf(LOG_ERROR, "fsFileLoadTo: could not seek to end: %s", fullName);
		fclose(f);
		return -1;
	}

	const long fileSize = ftell(f);
	if (fileSize < 0 || fileSize > INT_MAX || fseek(f, 0, SEEK_SET) != 0) {
		sysLogPrintf(LOG_ERROR, "fsFileLoadTo: invalid file size or rewind: %s", fullName);
		fclose(f);
		return -1;
	}
	const s32 size = (s32)fileSize;

	if (size < 0) {
		sysLogPrintf(LOG_ERROR, "fsFileLoadTo: empty file or invalid size (%d): %s", size, fullName);
		fclose(f);
		return -1;
	}

	if ((u32)size > dstSize) {
		sysLogPrintf(LOG_ERROR, "fsFileLoadTo: file too big for buffer (%u > %u): %s", size, dstSize, fullName);
		fclose(f);
		return -1;
	}

	const size_t readSize = fread(dst, 1, (size_t)size, f);
	const s32 closeResult = fclose(f);

	if (readSize != (size_t)size || closeResult != 0) {
		sysLogPrintf(LOG_ERROR,
			"fsFileLoadTo: incomplete read (%u/%u) or close failure: %s",
			(u32)readSize, (u32)size, fullName);
		return -1;
	}

	return size;
}

void *fsFileLoad(const char *name, u32 *outSize)
{
	if (outSize) {
		*outSize = 0;
	}

	if (!name) {
		return NULL;
	}

	const char *fullName = fsFullPath(name);

	FILE *f = fopen(fullName, "rb");
	if (!f) {
		sysLogPrintf(LOG_ERROR, "fsFileLoad: could not find file: %s", fullName);
		return NULL;
	}

	if (fseek(f, 0, SEEK_END) != 0) {
		sysLogPrintf(LOG_ERROR, "fsFileLoad: could not seek to end: %s", fullName);
		fclose(f);
		return NULL;
	}

	const long fileSize = ftell(f);
	if (fileSize < 0 || fileSize > INT_MAX || fseek(f, 0, SEEK_SET) != 0) {
		sysLogPrintf(LOG_ERROR, "fsFileLoad: invalid file size or rewind: %s", fullName);
		fclose(f);
		return NULL;
	}
	const s32 size = (s32)fileSize;

	void *buf = NULL;
	if (size) {
		buf = sysMemZeroAlloc(size + 1); // sick hack for a free null terminator
		if (!buf) {
			sysLogPrintf(LOG_ERROR, "fsFileLoad: could not alloc %d bytes for file: %s", size, fullName);
			fclose(f);
			return NULL;
		}

		const size_t readSize = fread(buf, 1, (size_t)size, f);
		if (readSize != (size_t)size) {
			sysLogPrintf(LOG_ERROR,
				"fsFileLoad: incomplete read (%u/%u): %s",
				(u32)readSize, (u32)size, fullName);
			sysMemFree(buf);
			fclose(f);
			return NULL;
		}
	}

	if (fclose(f) != 0) {
		sysLogPrintf(LOG_ERROR, "fsFileLoad: close failed: %s", fullName);
		sysMemFree(buf);
		return NULL;
	}

	if (outSize) {
		*outSize = size;
	}

	return buf;
}

s32 fsFileSize(const char *name)
{
	if (!name) {
		return -1;
	}

	const char *fullName = fsFullPath(name);
	struct stat st;
	if (stat(fullName, &st) < 0) {
		return -1;
	} else if (st.st_size < 0 || st.st_size > INT_MAX) {
		sysLogPrintf(LOG_ERROR, "fsFileSize: unsupported size for %s", fullName);
		return -1;
	} else {
		return (s32)st.st_size;
	}
}

FILE *fsFileOpenWrite(const char *name)
{
	return name ? fopen(fsFullPath(name), "wb") : NULL;
}

FILE *fsFileOpenRead(const char *name)
{
	return name ? fopen(fsFullPath(name), "rb") : NULL;
}

void fsFileFree(FILE *f)
{
	if (f) {
		fclose(f);
	}
}

s32 fsCreateDir(const char *path)
{
	if (!path) {
		return -1;
	}

#ifdef PLATFORM_WIN32
	return _mkdir(fsFullPath(path));
#else
	return mkdir(fsFullPath(path), 0777);
#endif
}
