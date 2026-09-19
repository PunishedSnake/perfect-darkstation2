#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "renderer_trace.h"
#include "system.h"

enum Ps2RendererTraceState {
    PS2_TRACE_IDLE,
    PS2_TRACE_ARMED,
    PS2_TRACE_CAPTURING,
};

static enum Ps2RendererTraceState s_state;
static uint32_t s_stage;
static uint32_t s_warmup_frames;
static uint32_t s_frame_number;
static struct Ps2RendererTraceHeader s_header;
static struct Ps2RendererTraceEvent *s_events;
static struct Ps2RendererTraceQword *s_qwords;
static uint8_t *s_blob;
static uint32_t s_event_capacity;
static uint32_t s_qword_capacity;
static uint32_t s_blob_capacity;
static uint32_t s_blob_size;
static uint32_t s_dropped_blob_bytes;
static bool s_frame_end_marked;

static_assert(sizeof(struct Ps2RendererTraceHeader) == 104u,
    "renderer trace header is a versioned disk format");
static_assert(sizeof(struct Ps2RendererTraceEvent) == 48u,
    "renderer trace event is a versioned disk format");
static_assert(sizeof(struct Ps2RendererTraceQword) == 16u,
    "renderer trace qword must match GIF/VIF transport width");

static void ps2RendererTraceFreeBuffers(void)
{
    free(s_events);
    free(s_qwords);
    free(s_blob);
    s_events = NULL;
    s_qwords = NULL;
    s_blob = NULL;
    s_event_capacity = 0u;
    s_qword_capacity = 0u;
    s_blob_capacity = 0u;
    s_blob_size = 0u;
    s_dropped_blob_bytes = 0u;
}

static bool ps2RendererTraceAllocateProfile(
    uint32_t event_capacity, uint32_t qword_capacity,
    uint32_t blob_capacity)
{
    s_events = (struct Ps2RendererTraceEvent *)malloc(
        sizeof(*s_events) * event_capacity);
    s_qwords = (struct Ps2RendererTraceQword *)malloc(
        sizeof(*s_qwords) * qword_capacity);
    s_blob = (uint8_t *)malloc(blob_capacity);
    if (!s_events || !s_qwords || !s_blob) {
        ps2RendererTraceFreeBuffers();
        return false;
    }
    s_event_capacity = event_capacity;
    s_qword_capacity = qword_capacity;
    s_blob_capacity = blob_capacity;
    return true;
}

extern "C" void ps2RendererTraceRequest(
    uint32_t stage, uint32_t warmup_frames)
{
    ps2RendererTraceFreeBuffers();

    struct CaptureProfile {
        uint32_t events;
        uint32_t qwords;
        uint32_t blob;
    };
    static const struct CaptureProfile profiles[] = {
        { PS2_RENDERER_TRACE_EVENT_CAPACITY_MAX,
          PS2_RENDERER_TRACE_QWORD_CAPACITY_MAX,
          PS2_RENDERER_TRACE_BLOB_CAPACITY_MAX },
        { 16384u, 131072u, 2u * 1024u * 1024u },
        { 8192u, 65536u, 1u * 1024u * 1024u },
    };

    uint32_t profile = 0u;
    for (; profile < sizeof(profiles) / sizeof(profiles[0]); ++profile) {
        if (ps2RendererTraceAllocateProfile(
                profiles[profile].events,
                profiles[profile].qwords,
                profiles[profile].blob)) {
            break;
        }
    }
    if (!s_events || !s_qwords || !s_blob) {
        s_state = PS2_TRACE_IDLE;
        printf("RendererTrace: allocation FAILED\n");
        fflush(stdout);
        return;
    }

    s_stage = stage;
    s_warmup_frames = warmup_frames;
    s_state = PS2_TRACE_ARMED;
    printf("RendererTrace: armed stage=%u warmup=%u profile=%u "
           "events=%u qwords=%u blob=%u KiB\n",
        stage, warmup_frames, profile,
        s_event_capacity, s_qword_capacity, s_blob_capacity / 1024u);
    fflush(stdout);
}

extern "C" void ps2RendererTraceBeginFrame(void)
{
    ++s_frame_number;
    if (s_state != PS2_TRACE_ARMED) {
        return;
    }
    if (s_warmup_frames != 0u) {
        --s_warmup_frames;
        return;
    }

    memset(&s_header, 0, sizeof(s_header));
    memcpy(s_header.magic, "PDGSTRC\0", 8u);
    s_header.version = PS2_RENDERER_TRACE_VERSION;
    s_header.header_size = sizeof(s_header);
    s_header.event_size = sizeof(*s_events);
    s_header.qword_size = sizeof(*s_qwords);
    s_header.stage = s_stage;
    s_header.frame_number = s_frame_number;
    s_header.start_microseconds = sysGetMicroseconds();
    s_header.event_capacity = s_event_capacity;
    s_header.qword_capacity = s_qword_capacity;
    s_header.event_offset = sizeof(s_header);
    s_header.qword_offset = sizeof(s_header);
    s_blob_size = 0u;
    s_dropped_blob_bytes = 0u;
    s_frame_end_marked = false;
    s_state = PS2_TRACE_CAPTURING;
    ps2RendererTraceRecord(PS2_TRACE_FRAME_BEGIN, 0u,
        s_stage, s_frame_number, 0u, 0u);
}

extern "C" bool ps2RendererTraceIsCapturing(void)
{
    return s_state == PS2_TRACE_CAPTURING;
}

extern "C" void ps2RendererTraceRecord(uint16_t type, uint16_t flags,
    uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    if (s_state != PS2_TRACE_CAPTURING) {
        return;
    }
    if (s_header.event_count >= s_event_capacity) {
        ++s_header.dropped_events;
        return;
    }

    struct Ps2RendererTraceEvent *event =
        &s_events[s_header.event_count++];
    event->sequence = s_header.event_count;
    event->type = type;
    event->flags = flags;
    event->microseconds = sysGetMicroseconds();
    event->a = a;
    event->b = b;
    event->c = c;
    event->d = d;
}

extern "C" void ps2RendererTraceRecordPath3Qwords(
    const void *qwords, uint32_t count)
{
    if (s_state != PS2_TRACE_CAPTURING || !qwords || count == 0u) {
        return;
    }

    const uint32_t offset = s_header.qword_count;
    uint32_t stored = count;
    const uint32_t available = s_qword_capacity -
        s_header.qword_count;
    if (stored > available) {
        stored = available;
        s_header.dropped_qwords += count - stored;
    }
    if (stored != 0u) {
        memcpy(&s_qwords[s_header.qword_count], qwords,
        (size_t)stored * sizeof(*s_qwords));
        s_header.qword_count += stored;
    }
    ps2RendererTraceRecord(PS2_TRACE_PATH3_SUBMIT,
        (uint16_t)PS2_TRACE_FLAG_PATH3 |
            (stored != count ?
                (uint16_t)PS2_TRACE_FLAG_DROPPED : 0u),
        offset, stored, count, 0u);
}

extern "C" void ps2RendererTraceRecordPath1Qwords(const void *qwords,
    uint32_t chain_qwords,
    uint32_t register_count, uint32_t vertex_count, bool textured)
{
    if (s_state != PS2_TRACE_CAPTURING || !qwords || chain_qwords == 0u) {
        return;
    }
    const uint32_t offset = s_header.qword_count;
    uint32_t stored = chain_qwords;
    const uint32_t available = s_qword_capacity -
        s_header.qword_count;
    if (stored > available) {
        stored = available;
        s_header.dropped_qwords += chain_qwords - stored;
    }
    if (stored != 0u) {
        memcpy(&s_qwords[s_header.qword_count], qwords,
            (size_t)stored * sizeof(*s_qwords));
        s_header.qword_count += stored;
    }
    ps2RendererTraceRecord(PS2_TRACE_PATH1_SUBMIT,
        (uint16_t)PS2_TRACE_FLAG_PATH1 |
            (textured ?
                (uint16_t)PS2_TRACE_FLAG_TEXTURED : 0u) |
            (stored != chain_qwords ?
                (uint16_t)PS2_TRACE_FLAG_DROPPED : 0u),
        offset, stored, chain_qwords,
        (uint64_t)register_count | ((uint64_t)vertex_count << 32u));
}

extern "C" void *ps2RendererTraceReserveBlob(
    uint32_t size, uint32_t alignment, uint32_t *offset)
{
    if (s_state != PS2_TRACE_CAPTURING || size == 0u ||
        !s_blob || !offset) {
        return NULL;
    }
    if (alignment == 0u) {
        alignment = 1u;
    }
    if (alignment > 64u || (alignment & (alignment - 1u)) != 0u) {
        return NULL;
    }

    const uint32_t aligned =
        (s_blob_size + alignment - 1u) & ~(alignment - 1u);
    if (aligned > s_blob_capacity ||
        size > s_blob_capacity - aligned) {
        s_dropped_blob_bytes += size;
        return NULL;
    }
    if (aligned > s_blob_size) {
        memset(s_blob + s_blob_size, 0, aligned - s_blob_size);
    }
    *offset = aligned;
    s_blob_size = aligned + size;
    return s_blob + aligned;
}

extern "C" bool ps2RendererTraceAppendBlob(
    const void *data, uint32_t size, uint32_t alignment, uint32_t *offset)
{
    if (!data) {
        return false;
    }
    void *destination = ps2RendererTraceReserveBlob(
        size, alignment, offset);
    if (!destination) {
        return false;
    }
    memcpy(destination, data, size);
    return true;
}

extern "C" void ps2RendererTraceMarkFrameEnd(void)
{
    if (s_state != PS2_TRACE_CAPTURING || s_frame_end_marked) {
        return;
    }
    ps2RendererTraceRecord(PS2_TRACE_FRAME_END, 0u,
        s_header.event_count, s_header.qword_count,
        s_header.dropped_events, s_header.dropped_qwords);
    s_header.end_microseconds = sysGetMicroseconds();
    s_frame_end_marked = true;
}

extern "C" bool ps2RendererTraceEndFrameAndWrite(void)
{
    if (s_state != PS2_TRACE_CAPTURING) {
        return false;
    }
    if (!s_frame_end_marked) {
        ps2RendererTraceMarkFrameEnd();
    }
    const uint64_t forensic_end = sysGetMicroseconds();
    s_header.qword_offset = s_header.event_offset +
        s_header.event_count * sizeof(*s_events);
    const uint32_t blob_offset = s_header.qword_offset +
        s_header.qword_count * sizeof(*s_qwords);
    s_header.reserved[0] = s_blob_size;
    s_header.reserved[1] = s_blob_capacity;
    s_header.reserved[2] = s_dropped_blob_bytes;
    s_header.reserved[3] = blob_offset;
    s_header.reserved[4] = 1u;
    s_header.reserved[5] = forensic_end >= s_header.end_microseconds
        ? forensic_end - s_header.end_microseconds : 0u;
    if (s_header.dropped_events != 0u ||
        s_header.dropped_qwords != 0u ||
        s_dropped_blob_bytes != 0u) {
        s_header.flags |= PS2_TRACE_FLAG_DROPPED;
    }

    char directory[256];
    char path[320];
    sysGetExecutablePath(directory, sizeof(directory));
    const int path_length = snprintf(path, sizeof(path),
        "%s/%s", directory, "pdps2-gs-trace.bin");
    bool ok = path_length > 0 && (size_t)path_length < sizeof(path);
    FILE *file = ok ? fopen(path, "wb") : NULL;
    ok = file != NULL;
    if (ok) {
        ok = fwrite(&s_header, sizeof(s_header), 1u, file) == 1u &&
            fwrite(s_events, sizeof(*s_events),
                s_header.event_count, file) == s_header.event_count &&
            fwrite(s_qwords, sizeof(*s_qwords),
                s_header.qword_count, file) == s_header.qword_count &&
            fwrite(s_blob, 1u, s_blob_size, file) == s_blob_size &&
            fflush(file) == 0;
        if (fclose(file) != 0) {
            ok = false;
        }
    }

    printf("RendererTrace: %s %s events=%u qwords=%u blob=%u "
           "dropped=%u/%u/%u\n",
        ok ? "saved" : "FAILED", path,
        s_header.event_count, s_header.qword_count, s_blob_size,
        s_header.dropped_events, s_header.dropped_qwords,
        s_dropped_blob_bytes);
    fflush(stdout);
    ps2RendererTraceFreeBuffers();
    s_state = PS2_TRACE_IDLE;
    return ok;
}
