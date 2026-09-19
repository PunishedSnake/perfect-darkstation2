#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PD_PS2_GIT_COMMIT
#define PD_PS2_GIT_COMMIT "unknown"
#endif

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
static uint32_t s_capture_profile;
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
        { 49152u, 393216u, 8u * 1024u * 1024u },
        { 32768u, 262144u, 4u * 1024u * 1024u },
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
    s_capture_profile = profile;
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
    s_header.flags = PS2_TRACE_FLAG_FORENSIC_HEAVY;
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

extern "C" uint64_t ps2RendererTraceHash(const void *data, uint32_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint64_t hash = UINT64_C(1469598103934665603);
    if (!bytes && size != 0u) {
        return 0u;
    }
    for (uint32_t i = 0u; i < size; ++i) {
        hash = (hash ^ bytes[i]) * UINT64_C(1099511628211);
    }
    return hash;
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

static bool ps2RendererTraceBuildPath(
    char *path, size_t path_size, const char *directory, const char *name)
{
    if (!path || path_size == 0u || !directory || !name) {
        return false;
    }
    const int length = snprintf(path, path_size, "%s/%s", directory, name);
    return length > 0 && (size_t)length < path_size;
}

static bool ps2RendererTraceWriteBytes(
    const char *directory, const char *name, const void *data, size_t size)
{
    char path[320];
    if (!ps2RendererTraceBuildPath(
            path, sizeof(path), directory, name)) {
        return false;
    }
    FILE *file = fopen(path, "wb");
    if (!file) {
        return false;
    }
    bool ok = size == 0u || fwrite(data, 1u, size, file) == size;
    if (fflush(file) != 0) {
        ok = false;
    }
    if (fclose(file) != 0) {
        ok = false;
    }
    return ok;
}

static bool ps2RendererTraceFindScreenshot(
    uint16_t request_subtype, uint16_t result_subtype,
    uint32_t *offset, uint32_t *size)
{
    bool requested = false;
    bool successful = false;
    uint32_t found_offset = 0u;
    uint32_t found_size = 0u;

    for (uint32_t i = 0u; i < s_header.event_count; ++i) {
        const struct Ps2RendererTraceEvent *event = &s_events[i];
        if (event->type != PS2_TRACE_SCREENSHOT) {
            continue;
        }
        const uint16_t subtype = (event->flags >> 8u) & 0x7fu;
        if (subtype == request_subtype &&
            (event->flags & PS2_TRACE_FLAG_DROPPED) == 0u) {
            found_offset = (uint32_t)event->a;
            found_size = (uint32_t)(event->a >> 32u);
            requested = true;
        } else if (subtype == result_subtype) {
            successful =
                (event->flags & PS2_TRACE_FLAG_SUPPORTED) != 0u &&
                (event->flags & PS2_TRACE_FLAG_DROPPED) == 0u;
        }
    }

    if (!requested || !successful ||
        found_offset > s_blob_size ||
        found_size > s_blob_size - found_offset) {
        return false;
    }
    if (offset) {
        *offset = found_offset;
    }
    if (size) {
        *size = found_size;
    }
    return true;
}

static bool ps2RendererTraceWriteManifest(
    const char *directory, uint64_t forensic_end,
    bool monolithic_ok, bool split_ok)
{
    char path[320];
    if (!ps2RendererTraceBuildPath(path, sizeof(path), directory,
            "pdps2-gs-trace.manifest.txt")) {
        return false;
    }
    FILE *file = fopen(path, "w");
    if (!file) {
        return false;
    }

    const uint32_t event_bytes =
        s_header.event_count * (uint32_t)sizeof(*s_events);
    const uint32_t qword_bytes =
        s_header.qword_count * (uint32_t)sizeof(*s_qwords);
    const uint64_t event_hash =
        ps2RendererTraceHash(s_events, event_bytes);
    const uint64_t qword_hash =
        ps2RendererTraceHash(s_qwords, qword_bytes);
    const uint64_t blob_hash =
        ps2RendererTraceHash(s_blob, s_blob_size);

    const uint64_t measured_duration =
        s_header.end_microseconds >= s_header.start_microseconds
        ? s_header.end_microseconds - s_header.start_microseconds : 0u;
    const uint64_t forensic_duration =
        forensic_end >= s_header.end_microseconds
        ? forensic_end - s_header.end_microseconds : 0u;

    bool ok = fprintf(file,
        "format=PDGSTRC forensic bundle\n"
        "version=%u\n"
        "git_commit=%s\n"
        "stage=%u\n"
        "frame=%u\n"
        "profile=%u\n"
        "forensic_heavy=1\n"
        "measured_duration_us=%llu\n"
        "post_frame_forensic_us=%llu\n"
        "events_count=%u\n"
        "events_capacity=%u\n"
        "events_dropped=%u\n"
        "events_bytes=%u\n"
        "events_fnv1a64=%016llx\n"
        "qwords_count=%u\n"
        "qwords_capacity=%u\n"
        "qwords_dropped=%u\n"
        "qwords_bytes=%u\n"
        "qwords_fnv1a64=%016llx\n"
        "blob_bytes=%u\n"
        "blob_capacity=%u\n"
        "blob_dropped_bytes=%u\n"
        "blob_fnv1a64=%016llx\n"
        "monolithic_ok=%u\n"
        "split_ok=%u\n"
        "monolithic=pdps2-gs-trace.bin\n"
        "header=pdps2-gs-trace.header.bin\n"
        "events=pdps2-gs-trace.events.bin\n"
        "qwords=pdps2-gs-trace.qwords.bin\n"
        "blob=pdps2-gs-trace.blob.bin\n"
        "framebuffer_draw=pdps2-gs-trace.framebuffer.raw\n"
        "framebuffer_other=pdps2-gs-trace.framebuffer-other.raw\n"
        "gs_vram_ct32=pdps2-gs-trace.vram-ct32.bin\n",
        s_header.version,
        PD_PS2_GIT_COMMIT,
        s_header.stage,
        s_header.frame_number,
        s_capture_profile,
        (unsigned long long)measured_duration,
        (unsigned long long)forensic_duration,
        s_header.event_count,
        s_event_capacity,
        s_header.dropped_events,
        event_bytes,
        (unsigned long long)event_hash,
        s_header.qword_count,
        s_qword_capacity,
        s_header.dropped_qwords,
        qword_bytes,
        (unsigned long long)qword_hash,
        s_blob_size,
        s_blob_capacity,
        s_dropped_blob_bytes,
        (unsigned long long)blob_hash,
        monolithic_ok ? 1u : 0u,
        split_ok ? 1u : 0u) > 0;

    if (fflush(file) != 0) {
        ok = false;
    }
    if (fclose(file) != 0) {
        ok = false;
    }
    return ok;
}

extern "C" bool ps2RendererTraceEndFrameAndWrite(void)
{
    if (s_state != PS2_TRACE_CAPTURING) {
        return false;
    }
    const bool had_explicit_frame_end = s_frame_end_marked;
    if (!s_frame_end_marked) {
        ps2RendererTraceMarkFrameEnd();
    }
    const uint64_t forensic_end = sysGetMicroseconds();
    if (had_explicit_frame_end) {
        ps2RendererTraceRecord(PS2_TRACE_CAPTURE_INFO, 0u,
            forensic_end >= s_header.end_microseconds
                ? forensic_end - s_header.end_microseconds : 0u,
            s_blob_size, s_header.event_count, s_header.qword_count);
    }
    s_header.qword_offset = s_header.event_offset +
        s_header.event_count * sizeof(*s_events);
    const uint32_t blob_offset = s_header.qword_offset +
        s_header.qword_count * sizeof(*s_qwords);
    s_header.reserved[0] = s_blob_size;
    s_header.reserved[1] = s_blob_capacity;
    s_header.reserved[2] = s_dropped_blob_bytes;
    s_header.reserved[3] = blob_offset;
    s_header.reserved[4] = s_capture_profile;
    if (s_header.dropped_events != 0u ||
        s_header.dropped_qwords != 0u ||
        s_dropped_blob_bytes != 0u) {
        s_header.flags |= PS2_TRACE_FLAG_DROPPED;
    }

    char directory[256];
    char path[320];
    sysGetExecutablePath(directory, sizeof(directory));

    bool monolithic_ok = ps2RendererTraceBuildPath(
        path, sizeof(path), directory, "pdps2-gs-trace.bin");
    FILE *file = monolithic_ok ? fopen(path, "wb") : NULL;
    monolithic_ok = file != NULL;
    if (monolithic_ok) {
        monolithic_ok =
            fwrite(&s_header, sizeof(s_header), 1u, file) == 1u &&
            fwrite(s_events, sizeof(*s_events),
                s_header.event_count, file) == s_header.event_count &&
            fwrite(s_qwords, sizeof(*s_qwords),
                s_header.qword_count, file) == s_header.qword_count &&
            fwrite(s_blob, 1u, s_blob_size, file) == s_blob_size &&
            fflush(file) == 0;
        if (fclose(file) != 0) {
            monolithic_ok = false;
        }
    }

    const uint32_t event_bytes =
        s_header.event_count * (uint32_t)sizeof(*s_events);
    const uint32_t qword_bytes =
        s_header.qword_count * (uint32_t)sizeof(*s_qwords);
    bool split_ok =
        ps2RendererTraceWriteBytes(directory,
            "pdps2-gs-trace.header.bin",
            &s_header, sizeof(s_header)) &&
        ps2RendererTraceWriteBytes(directory,
            "pdps2-gs-trace.events.bin",
            s_events, event_bytes) &&
        ps2RendererTraceWriteBytes(directory,
            "pdps2-gs-trace.qwords.bin",
            s_qwords, qword_bytes) &&
        ps2RendererTraceWriteBytes(directory,
            "pdps2-gs-trace.blob.bin",
            s_blob, s_blob_size);

    uint32_t screenshot_offset = 0u;
    uint32_t screenshot_size = 0u;
    if (ps2RendererTraceFindScreenshot(
            0u, 1u, &screenshot_offset, &screenshot_size)) {
        split_ok = ps2RendererTraceWriteBytes(directory,
            "pdps2-gs-trace.framebuffer.raw",
            s_blob + screenshot_offset, screenshot_size) && split_ok;
    }
    uint32_t other_offset = 0u;
    uint32_t other_size = 0u;
    if (ps2RendererTraceFindScreenshot(
            2u, 3u, &other_offset, &other_size)) {
        split_ok = ps2RendererTraceWriteBytes(directory,
            "pdps2-gs-trace.framebuffer-other.raw",
            s_blob + other_offset, other_size) && split_ok;
    }

    const bool manifest_ok = ps2RendererTraceWriteManifest(
        directory, forensic_end, monolithic_ok, split_ok);
    const bool ok = monolithic_ok && split_ok && manifest_ok;

    printf("RendererTrace: %s %s events=%u/%u qwords=%u/%u "
           "blob=%u/%u dropped=%u/%u/%u profile=%u split=%s\n",
        ok ? "saved" : "PARTIAL", path,
        s_header.event_count, s_event_capacity,
        s_header.qword_count, s_qword_capacity,
        s_blob_size, s_blob_capacity,
        s_header.dropped_events, s_header.dropped_qwords,
        s_dropped_blob_bytes, s_capture_profile,
        split_ok ? "ok" : "FAILED");
    fflush(stdout);
    ps2RendererTraceFreeBuffers();
    s_state = PS2_TRACE_IDLE;
    return ok;
}
