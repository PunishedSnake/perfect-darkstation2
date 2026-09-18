#include <stdio.h>
#include <string.h>

#include "renderer_trace.h"
#include "system.h"

static u64 s_now;

extern "C" u64 sysGetMicroseconds(void)
{
    return ++s_now;
}

extern "C" void sysGetExecutablePath(char *out_path, const u32 out_len)
{
    snprintf(out_path, out_len, "/tmp");
}

int main(void)
{
    const struct Ps2RendererTraceQword qwords[] = {
        { 1u | (1u << 15) | (1ull << 60), 0x0eu },
        { UINT64_C(0x123456789abcdef0), 0x47u },
    };
    ps2RendererTraceRequest(0x26u, 1u);
    ps2RendererTraceBeginFrame();
    if (ps2RendererTraceIsCapturing()) {
        return 1;
    }
    ps2RendererTraceBeginFrame();
    if (!ps2RendererTraceIsCapturing()) {
        return 2;
    }
    ps2RendererTraceRecord(PS2_TRACE_DEPTH, 7u, 1u, 2u, 3u, 4u);
    ps2RendererTraceRecordPath3Qwords(qwords, 2u);
    ps2RendererTraceRecordPath1Qwords(qwords, 2u, 7u, 3u, true);
    if (!ps2RendererTraceEndFrameAndWrite()) {
        return 3;
    }

    FILE *file = fopen("/tmp/pdps2-gs-trace.bin", "rb");
    if (!file) {
        return 4;
    }
    struct Ps2RendererTraceHeader header;
    const bool read = fread(&header, sizeof(header), 1u, file) == 1u;
    fclose(file);
    if (!read || memcmp(header.magic, "PDGSTRC\0", 8u) != 0 ||
        header.version != PS2_RENDERER_TRACE_VERSION ||
        header.stage != 0x26u || header.event_count != 5u ||
        header.qword_count != 4u || header.dropped_events != 0u ||
        header.dropped_qwords != 0u) {
        return 5;
    }
    return 0;
}
