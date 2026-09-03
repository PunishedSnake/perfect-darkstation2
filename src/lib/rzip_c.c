// see https://github.com/n64decomp/007/blob/master/tools/mktex/src/libpdtex/reader.c
// and https://github.com/doomhack/perfect_dark/blob/master/src/lib/rzip.c

#include <zlib.h>

#include "lib/rzip.h"

void *var80091558; // g_RzipUnused

s32 rzipIs1172(void *buffer)
{
	const u8* src = buffer;
	return (src[0] == 0x11 && src[1] == 0x72);
}

s32 rzipIs1173(void *buffer)
{
	const u8* src = buffer;
	return (src[0] == 0x11 && src[1] == 0x73);
}

static inline s32 rzipInflate1172(z_stream *strm, u8 *src, void *dst)
{
	strm->avail_in = 0x2000;
	strm->next_in = src;

	do {
		strm->avail_out = 0x2000;
		strm->next_out = dst;
		if (inflate(strm, Z_FINISH) == Z_STREAM_ERROR) {
			rmonPrintf("rzipInflate1172: Z_STREAM_ERROR\n");
			return 0;
		}
	} while (strm->avail_out == 0);

	return strm->total_out;
}

static inline s32 rzipInflate1173(z_stream *strm, u8 *src, void *dst, u32 dstLen)
{
	strm->avail_in = -1; // compressed size unknown
	strm->next_in = src;
	strm->avail_out = dstLen;
	strm->next_out = dst;

	if (inflate(strm, Z_SYNC_FLUSH) == Z_STREAM_ERROR) {
		rmonPrintf("rzipInflate1173: Z_STREAM_ERROR\n");
		return 0;
	}

	return strm->total_out;
}

/**
 * Inflate a Rare 1172/1173 stream without allowing zlib to read or write past
 * caller-owned buffers. The original N64 API does not carry either bound, so
 * keep rzipInflate for matching callers and use this entry point for portable
 * file-backed data whose exact compressed extent is known.
 */
s32 rzipInflateSized(void *srcp, u32 srcLen, void *dst, u32 dstLen, void *scratch)
{
	u8 *src = srcp;
	u32 headerLen;
	u32 expectedLen;
	z_stream strm = { 0 };
	s32 zret;

	(void)scratch;

	if (!src || !dst || srcLen < 2 || dstLen == 0) {
		return 0;
	}

	if (rzipIs1173(src)) {
		if (srcLen < 5) {
			return 0;
		}

		headerLen = 5;
		expectedLen = ((u32)src[2] << 16) | ((u32)src[3] << 8) | (u32)src[4];

		if (expectedLen == 0 || expectedLen > dstLen) {
			return 0;
		}
	} else if (rzipIs1172(src)) {
		headerLen = 2;
		expectedLen = dstLen;
	} else {
		return 0;
	}

	if (srcLen <= headerLen) {
		return 0;
	}

	zret = inflateInit2(&strm, -15);

	if (zret != Z_OK) {
		return 0;
	}

	strm.next_in = src + headerLen;
	strm.avail_in = srcLen - headerLen;
	strm.next_out = dst;
	strm.avail_out = expectedLen;

	zret = inflate(&strm, Z_FINISH);

	if (zret != Z_STREAM_END || strm.total_out == 0 ||
		(rzipIs1173(src) && strm.total_out != expectedLen)) {
		inflateEnd(&strm);
		return 0;
	}

	var80091558 = strm.next_in;
	expectedLen = strm.total_out;
	inflateEnd(&strm);

	return expectedLen;
}

s32 rzipInflate(void *srcp, void *dst, void *scratch)
{
	s32 ret = 0;
	u8 *src = srcp;
	z_stream strm = { 0 };
	(void)scratch;

	ret = inflateInit2(&strm, -15);
	if (ret != Z_OK) {
		rmonPrintf("rzipInflate: inflateInit2 failed: %d\n", ret);
		return 0;
	}

	if (rzipIs1173(src)) {
		// 1173, we know the uncompressed length
		const u32 dstLen = ((u32)src[2] << 16) | ((u32)src[3] << 8) | (u32)src[4];
		ret = rzipInflate1173(&strm, src + 5, dst, dstLen);
	} else if (rzipIs1172(src)) {
		// 1172, uncompressed length unknown
		ret = rzipInflate1172(&strm, src + 2, dst);
	} else {
		rmonPrintf("rzipInflate: input not in any known rare zip format\n");
		ret = 0;
	}

	inflateEnd(&strm);

	if (ret) {
		var80091558 = strm.next_in;
		return strm.total_out;
	} else {
		return 0;
	}
}

u32 rzipInit(void)
{
	// this builds tables in the original assembly version, we don't need that
	return 0;
}

void *rzipGetSomething(void)
{
	return var80091558;
}
