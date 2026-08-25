#include <stdbool.h>
#include <string.h>
#include <zlib.h>

#include "romsource.h"
#include "system.h"
#include "log_ps2.h"

#define PD_ROM_SIZE (32u * 1024u * 1024u)
#define PD_ROM_DATA_OFS 0x39850u
#define PD_ROM_FILES_OFS 0x28080u
#define PD_RZIP_HEADER_SIZE 5u
#define PD_RZIP_MAX_DATA_SEG (4u * 1024u * 1024u)
#define PD_RZIP_INPUT_CHUNK 8192u

static u32 readBe32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
}

static u32 fnv1a32(const void *data, u32 size)
{
    const u8 *bytes = data;
    u32 hash = 2166136261u;

    for (u32 i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }

    return hash;
}

static bool validateRomHeader(struct romsource *source)
{
    u8 header[64];

    if (romSourceGetSize(source) != PD_ROM_SIZE) {
        sysLogPrintf(LOG_ERROR, "ROM probe: wrong size (%u, expected %u)",
            romSourceGetSize(source), PD_ROM_SIZE);
        return false;
    }

    if (!romSourceReadAt(source, 0, header, sizeof(header))) {
        sysLogPrintf(LOG_ERROR, "ROM probe: header read failed");
        return false;
    }

    if (readBe32(header) != 0x80371240u) {
        sysLogPrintf(LOG_ERROR, "ROM probe: wrong z64 magic %08x", readBe32(header));
        return false;
    }

    if (memcmp(header + 0x20, "Perfect Dark", 12) != 0 ||
        memcmp(header + 0x3b, "NPDE", 4) != 0) {
        sysLogPrintf(LOG_ERROR, "ROM probe: expected NTSC-final Perfect Dark header not found");
        return false;
    }

    return true;
}

static int inflateDataSegment(struct romsource *source)
{
    u8 rzipHeader[PD_RZIP_HEADER_SIZE];
    static u8 input[PD_RZIP_INPUT_CHUNK];
    z_stream stream;
    u8 *output = NULL;
    u32 expectedSize;
    u32 nextOffset;
    u32 compressedBytes = 0;
    int result = -1;
    int zret;

    if (!romSourceReadAt(source, PD_ROM_DATA_OFS, rzipHeader, sizeof(rzipHeader))) {
        sysLogPrintf(LOG_ERROR, "ROM probe: RZIP header read failed");
        return -1;
    }

    if (rzipHeader[0] != 0x11 || rzipHeader[1] != 0x73) {
        sysLogPrintf(LOG_ERROR, "ROM probe: data segment is not RZIP 1173 (%02x%02x)",
            rzipHeader[0], rzipHeader[1]);
        return -1;
    }

    expectedSize = ((u32)rzipHeader[2] << 16) |
        ((u32)rzipHeader[3] << 8) | (u32)rzipHeader[4];

    if (expectedSize < PD_ROM_FILES_OFS + 12u || expectedSize > PD_RZIP_MAX_DATA_SEG) {
        sysLogPrintf(LOG_ERROR, "ROM probe: implausible data segment size %u", expectedSize);
        return -1;
    }

    sysLogPrintf(LOG_NOTE, "ROM probe: RZIP 1173 expected output=%u input_chunk=%u",
        expectedSize, PD_RZIP_INPUT_CHUNK);

    output = sysMemAlloc(expectedSize);
    if (!output) {
        sysLogPrintf(LOG_ERROR, "ROM probe: could not allocate %u-byte final data segment", expectedSize);
        return -1;
    }

    memset(&stream, 0, sizeof(stream));
    stream.next_out = output;
    stream.avail_out = expectedSize;

    zret = inflateInit2(&stream, -MAX_WBITS);
    if (zret != Z_OK) {
        sysLogPrintf(LOG_ERROR, "ROM probe: inflateInit2 failed: %d", zret);
        goto cleanup;
    }

    sysLogPrintf(LOG_NOTE, "ROM probe: entering bounded inflate loop at ROM offset %08x",
        PD_ROM_DATA_OFS + PD_RZIP_HEADER_SIZE);
    ps2LogFlush();

    nextOffset = PD_ROM_DATA_OFS + PD_RZIP_HEADER_SIZE;

    for (;;) {
        if (stream.avail_in == 0) {
            const u32 romSize = romSourceGetSize(source);
            const u32 remaining = romSize - nextOffset;
            const u32 amount = remaining < PD_RZIP_INPUT_CHUNK ? remaining : PD_RZIP_INPUT_CHUNK;

            if (nextOffset >= romSize || amount == 0 ||
                !romSourceReadAt(source, nextOffset, input, amount)) {
                sysLogPrintf(LOG_ERROR, "ROM probe: compressed stream ended before Z_STREAM_END");
                inflateEnd(&stream);
                goto cleanup;
            }

            nextOffset += amount;
            stream.next_in = input;
            stream.avail_in = amount;
        }

        const uLong beforeIn = stream.total_in;
        const uLong beforeOut = stream.total_out;
        zret = inflate(&stream, Z_NO_FLUSH);

        if (zret == Z_STREAM_END) {
            compressedBytes = (u32)stream.total_in;
            break;
        }

        if (zret != Z_OK) {
            sysLogPrintf(LOG_ERROR, "ROM probe: inflate failed: %d after %lu input bytes",
                zret, (unsigned long)stream.total_in);
            inflateEnd(&stream);
            goto cleanup;
        }

        if (stream.total_in == beforeIn && stream.total_out == beforeOut) {
            sysLogPrintf(LOG_ERROR, "ROM probe: inflate made no progress");
            inflateEnd(&stream);
            goto cleanup;
        }

        if (stream.avail_out == 0) {
            sysLogPrintf(LOG_ERROR, "ROM probe: output filled before Z_STREAM_END");
            inflateEnd(&stream);
            goto cleanup;
        }
    }

    zret = inflateEnd(&stream);
    if (zret != Z_OK || stream.total_out != expectedSize) {
        sysLogPrintf(LOG_ERROR, "ROM probe: decompressed size mismatch (%lu, expected %u)",
            (unsigned long)stream.total_out, expectedSize);
        goto cleanup;
    }

    {
        const u8 *offsets = output + PD_ROM_FILES_OFS;
        const u32 firstFile = readBe32(offsets + 4);
        const u32 secondFile = readBe32(offsets + 8);

        if (firstFile == 0 || secondFile <= firstFile || secondFile > PD_ROM_SIZE) {
            sysLogPrintf(LOG_ERROR, "ROM probe: file table sanity failed (%08x, %08x)",
                firstFile, secondFile);
            goto cleanup;
        }

        sysLogPrintf(LOG_NOTE, "ROM probe: RZIP 1173 -> %u bytes, consumed %u compressed bytes",
            expectedSize, compressedBytes);
        sysLogPrintf(LOG_NOTE, "ROM probe: first file extent %08x..%08x (%u bytes)",
            firstFile, secondFile, secondFile - firstFile);
        sysLogPrintf(LOG_NOTE, "ROM probe: data FNV1a %08x", fnv1a32(output, expectedSize));
    }

    result = 1;

cleanup:
    sysMemFree(output);
    return result;
}

int ps2RomProbe(const char *path)
{
    struct romsource source = { 0 };
    int result;

    if (!path || !path[0]) {
        sysLogPrintf(LOG_WARNING, "ROM probe: no path");
        return 0;
    }

    sysLogPrintf(LOG_NOTE, "ROM probe: opening %s", path);
    ps2LogFlush();

    if (!romSourceOpenFile(&source, path)) {
        sysLogPrintf(LOG_ERROR, "ROM probe: open failed: %s", path);
        return -1;
    }

    sysLogPrintf(LOG_NOTE, "ROM probe: source size=%u bytes", romSourceGetSize(&source));

    if (!validateRomHeader(&source)) {
        romSourceClose(&source);
        return -1;
    }

    sysLogPrintf(LOG_NOTE, "ROM probe: header and bounded source contract ok");
    result = inflateDataSegment(&source);
    romSourceClose(&source);

    if (result > 0) {
        sysLogPrintf(LOG_NOTE, "ROM probe: real ROM data path ok");
    }

    return result;
}
