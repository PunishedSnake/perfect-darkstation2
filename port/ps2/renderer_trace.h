#ifndef PERFECT_DARK_PS2_RENDERER_TRACE_H
#define PERFECT_DARK_PS2_RENDERER_TRACE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PS2_RENDERER_TRACE_VERSION 2u

/*
 * Capture buffers exist only while Select has armed a one-frame trace. The
 * implementation tries the largest profile first and falls back if the EE heap
 * cannot provide it.
 */
#define PS2_RENDERER_TRACE_EVENT_CAPACITY_MAX 32768u
#define PS2_RENDERER_TRACE_QWORD_CAPACITY_MAX 262144u
#define PS2_RENDERER_TRACE_BLOB_CAPACITY_MAX (4u * 1024u * 1024u)

enum Ps2RendererTraceEventType {
    PS2_TRACE_FRAME_BEGIN = 1,
    PS2_TRACE_FRAME_END,
    PS2_TRACE_SHADER,
    PS2_TRACE_TEXTURE_SELECT,
    PS2_TRACE_TEXTURE_UPLOAD,
    PS2_TRACE_SAMPLER,
    PS2_TRACE_DEPTH,
    PS2_TRACE_VIEWPORT,
    PS2_TRACE_SCISSOR,
    PS2_TRACE_ALPHA,
    PS2_TRACE_DRAW_INPUT,
    PS2_TRACE_DRAW_CLIPPED,
    PS2_TRACE_PATH1_SUBMIT,
    PS2_TRACE_PATH3_SUBMIT,
    PS2_TRACE_CORE_SUMMARY,
    PS2_TRACE_GS_REGISTER,
    PS2_TRACE_TEXTURE_RESOURCE,
    PS2_TRACE_TEXTURE_CLUT,
    PS2_TRACE_RENDER_TARGET,
    PS2_TRACE_VRAM,
    PS2_TRACE_RENDERER_STATS,
    PS2_TRACE_FRONTEND_STATE,
    PS2_TRACE_WARNING,
    PS2_TRACE_PASS_GRAPH_DRAW,
    PS2_TRACE_INDEPENDENT_ALPHA_DRAW,
    PS2_TRACE_CLIPPED_TRIANGLE_BOUNDS,
    PS2_TRACE_TEXTURE_COORD_RANGE,
    PS2_TRACE_DRAW_STATE,
    PS2_TRACE_DRAW_PAYLOAD,
    PS2_TRACE_TEXTURE_DETAIL,
    PS2_TRACE_BUILD_CONFIG,
    PS2_TRACE_QUEUE_WAIT,
    PS2_TRACE_GS_DRAW,
    PS2_TRACE_RESOURCE_OP,
};

enum Ps2RendererTraceFlags {
    PS2_TRACE_FLAG_PATH1 = 1u << 0,
    PS2_TRACE_FLAG_PATH3 = 1u << 1,
    PS2_TRACE_FLAG_TEXTURED = 1u << 2,
    PS2_TRACE_FLAG_SUPPORTED = 1u << 3,
    PS2_TRACE_FLAG_DROPPED = 1u << 15,
};

#pragma pack(push, 1)
struct Ps2RendererTraceHeader {
    uint8_t magic[8];
    uint32_t version;
    uint32_t header_size;
    uint32_t event_size;
    uint32_t qword_size;
    uint32_t stage;
    uint32_t frame_number;
    uint64_t start_microseconds;
    uint64_t end_microseconds;
    uint32_t event_count;
    uint32_t event_capacity;
    uint32_t dropped_events;
    uint32_t qword_count;
    uint32_t qword_capacity;
    uint32_t dropped_qwords;
    uint32_t event_offset;
    uint32_t qword_offset;
    uint32_t flags;
    uint32_t reserved[5];
};

struct Ps2RendererTraceEvent {
    uint32_t sequence;
    uint16_t type;
    uint16_t flags;
    uint64_t microseconds;
    uint64_t a;
    uint64_t b;
    uint64_t c;
    uint64_t d;
};

struct Ps2RendererTraceQword {
    uint64_t lo;
    uint64_t hi;
};
#pragma pack(pop)

/* Arm a one-frame capture. A later request replaces an earlier pending one. */
void ps2RendererTraceRequest(uint32_t stage, uint32_t warmup_frames);
void ps2RendererTraceBeginFrame(void);
bool ps2RendererTraceIsCapturing(void);
void ps2RendererTraceRecord(uint16_t type, uint16_t flags,
    uint64_t a, uint64_t b, uint64_t c, uint64_t d);
void ps2RendererTraceRecordPath3Qwords(const void *qwords, uint32_t count);
void ps2RendererTraceRecordPath1Qwords(const void *qwords,
    uint32_t chain_qwords,
    uint32_t register_count, uint32_t vertex_count, bool textured);

/*
 * Variable-size v2 payload storage. Offset is relative to the blob section.
 * Metadata events keep recording if this storage fills; dropped bytes are
 * reported in the header instead of corrupting the rest of the capture.
 */
void *ps2RendererTraceReserveBlob(
    uint32_t size, uint32_t alignment, uint32_t *offset);
bool ps2RendererTraceAppendBlob(const void *data, uint32_t size,
    uint32_t alignment, uint32_t *offset);
bool ps2RendererTraceEndFrameAndWrite(void);

#ifdef __cplusplus
}
#endif

#endif
