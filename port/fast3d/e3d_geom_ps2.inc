/*
 * Emotion3D geometry: room leaves (stage 1) and model display lists
 * (stage 3). See docs/ps2-emotion3d.md.
 *
 * Included into the PS2 Fast3D frontend (gfx_pc.cpp, GFX_PS2_DIRECT only):
 * it drives the frontend's own vertex, triangle and object-batch machinery
 * directly instead of through G_VTX/G_TRI decoding.
 *
 * A display list is compiled once into ops:
 *   - STATE: runs of state commands (texture loads, combiner, render modes),
 *     copied into a compact list that Fast3D still interprets;
 *   - GEOM:  runs of G_COL / G_VTX / G_TRI1 / G_TRI4 / G_MTX / G_POPMTX,
 *     turned into an item stream:
 *       LOAD  one vertex load (vertex index, slots, colour table, matrix
 *             epoch);
 *       CHUNK the triangles between two loads, per corner the vertex index,
 *             colour index and the slot the list used;
 *       MTX   a matrix load or pop.
 *
 * Replay, per matrix epoch (the items between two matrix commands):
 *   - chunked epochs: loads stay lazy; each chunk is classified against the
 *     frustum planes on VU0. Inside the guard band it is packed straight
 *     into the VU1 object batch (as gfx_obj_tri packs deferred vertices);
 *     outside one exact plane it is skipped; straddling, or in a state the
 *     object path cannot draw, its loads and triangles go to gfx_sp_vertex
 *     and gfx_sp_tri1 directly.
 *   - linear epochs: loads and triangles go to gfx_sp_vertex/gfx_sp_tri1
 *     in list order, minus the decoding. An epoch is linear when one of its
 *     loads is read by a triangle of a later epoch: Perfect Dark skins
 *     characters by loading one vertex block under the thigh matrix and the
 *     next under the shin matrix, then drawing triangles that join them.
 *     Those loads must be transformed under their own matrix, in order, as
 *     the list did. Object deferral is off while a linear epoch runs, so
 *     skinned joins no longer defer and replay.
 *
 * Rooms: bgRenderRoomPass emits G_E3D_ROOMLEAF_EXT(room, gdl); programs are
 * keyed by (list, room, room generation), which bg.c bumps on room load and
 * unload. Vertex positions are static, so chunk bounds are precomputed.
 *
 * Models: modelRenderNodeDl emits G_E3D_MODELDL_EXT(gdl). Model lists are
 * shared by every instance of a model definition, while vertices (segment
 * 4), colours (segments 5 and 6) and matrices (segment 3) are per instance,
 * so all of those are read live and chunk bounds are computed per use. A
 * few lists are built at run time (monitor screens), so every use checks a
 * checksum of the list's words; a list whose content keeps changing is
 * marked volatile and always runs as a plain display list.
 */

#include "gs_vu1_queue.h"
#include "gs_vu1_transform.h"
extern "C" bool gfxPs2Emotion3DEnabled(void);
extern "C" bool gfxPs2Emotion3DModelsEnabled(void);
extern "C" bool gfxPs2Emotion3DSkinEnabled(void);
extern "C" void gfxPs2E3dRoomChanged(int32_t room);
static void gfx_run_dl(Gfx* cmd, uint32_t ps2_trace_depth);
static inline void gfx_sp_tri1(uint8_t vtx1_idx, uint8_t vtx2_idx, uint8_t vtx3_idx, bool is_rect);
static inline void* seg_addr(uintptr_t w1);

enum {
    E3D_KIND_ROOM = 0,
    E3D_KIND_MODEL = 1,

    E3D_OP_STATE = 0,
    E3D_OP_GEOM = 1,

    E3D_ITEM_LOAD = 0,
    E3D_ITEM_CHUNK = 1,
    E3D_ITEM_MTX = 2,

    E3D_SEG_MODEL_MTX = 3,  /* SPSEGMENT_MODEL_MTX */
    E3D_SEG_MODEL_VTX = 4,  /* SPSEGMENT_MODEL_VTX */
    E3D_SEG_MODEL_COL1 = 5, /* SPSEGMENT_MODEL_COL1 */
    E3D_SEG_MODEL_COL2 = 6, /* SPSEGMENT_MODEL_COL2 */
    E3D_SEG_ROOM_COL = 13,  /* SPSEGMENT_BG_COL */
    E3D_SEG_ROOM_VTX = 14,  /* SPSEGMENT_BG_VTX */

    E3D_ROOM_GENS = 1024,
    E3D_BUCKETS = 512,
    E3D_ARENA_BYTES = 256 * 1024,
    E3D_MAX_CMDS = 2048,
    E3D_MAX_TRIS = 2048,
    E3D_MAX_OPS = 192,
    E3D_MAX_ITEMS = 1024,
    E3D_MAX_CHUNKS = 512,
    E3D_MAX_LOADS = 512,
    E3D_MAX_CHUNK_LOADS = 2048,
    E3D_MAX_MTX = 128,
    E3D_SLOTS = 32,
    E3D_VOLATILE_AFTER = 3,

    E3D_FLAG_LINEAR = 1,
};

struct E3dTri {
    uint16_t vtx[3];  /* index into the vertex segment */
    uint16_t col[3];  /* colour table of the corner's load (plus Vtx.colour/4, read live) */
    uint8_t slot[3];  /* the vertex slots the display list used */
    uint8_t colseg;   /* bit k: corner k uses the second colour segment */
    uint16_t ld[3];   /* the corner's load */
    uint16_t pad;
};

struct E3dLoad {      /* one G_VTX */
    uint16_t first;   /* vertex index */
    uint8_t n;
    uint8_t dest;
    uint16_t colbase; /* colour table (index) current at the load */
    uint8_t colseg;   /* 0: first colour segment, 1: second */
    uint8_t flags;
    uint16_t epoch;
    uint16_t pad;
};

struct E3dChunk {
    uint16_t tri0;
    uint16_t ntris;
    uint16_t ld0;     /* into the program's chunk-load list */
    uint16_t nld;
    uint16_t epoch;
    uint8_t flags;
    uint8_t pad;
    float lo[3];      /* rooms only: precomputed bounds */
    float hi[3];
};

struct E3dItem {
    uint8_t kind;
    uint8_t pad;
    uint16_t index;
};

struct E3dOp {
    uint8_t kind;
    uint8_t open;     /* GEOM: a triangle reads a load of an earlier op */
    uint16_t nitems;
    uint16_t item0;
    uint16_t load0;   /* GEOM: its first load */
    Gfx* dl;          /* STATE only */
};

struct E3dProg {
    const Gfx* gdl;
    struct E3dProg* next;
    uint16_t room;    /* 0xffff for models */
    uint16_t gen;
    uint16_t nops;
    uint8_t kind;
    uint8_t ok;
    uint8_t mismatches;
    uint8_t is_volatile;
    uint8_t skinned;  /* a load is read across a matrix change */
    uint8_t open;     /* some GEOM op is open (E3dOp.open) */
    uint32_t ncmds;
    uint32_t checksum;
    uint32_t checked_frame; /* num_dls when the checksum last matched */
    uint32_t plain_until;   /* num_dls: run as a plain list until then */
    struct E3dOp* ops;
    struct E3dItem* items;
    struct E3dChunk* chunks;
    struct E3dTri* tris;
    struct E3dLoad* loads;
    uint16_t* chunk_loads;
    Gfx* mtx;         /* the matrix commands, in item order */
};

static struct {
    uint8_t arena[E3D_ARENA_BYTES] __attribute__((aligned(16)));
    uint32_t used;
    struct E3dProg* buckets[E3D_BUCKETS];
    uint16_t room_gen[E3D_ROOM_GENS];
    /* model lists seen once: compiled only when seen again unchanged, so
     * lists built in per-frame memory never fill the arena */
    struct {
        const Gfx* gdl;
        uint32_t checksum;
    } seen[256];
    /* counters, logged per frame with PD_PS2_OBJECT_STATS */
    uint32_t n_leaves, n_models, n_skinned, n_fallback, n_compiles, n_resets, n_volatile;
    uint32_t n_inside, n_outside, n_mixed, n_unfit, n_tris, n_slow_tris, n_linear_tris;
} e3d;

extern "C" void gfxPs2E3dRoomChanged(int32_t room) {
    ++e3d.room_gen[(uint32_t)room % E3D_ROOM_GENS];
}

static void* e3d_alloc(uint32_t bytes) {
    bytes = (bytes + 15u) & ~15u;
    if (e3d.used + bytes > sizeof(e3d.arena)) {
        return nullptr;
    }
    void* p = &e3d.arena[e3d.used];
    e3d.used += bytes;
    return p;
}

/* Full arena: forget every program; lists recompile on their next use. */
static void e3d_reset(void) {
    e3d.used = 0;
    memset(e3d.buckets, 0, sizeof(e3d.buckets));
    ++e3d.n_resets;
}

static inline uint32_t e3d_bucket(const Gfx* gdl, uint32_t room) {
    const uint32_t h = (uint32_t)(uintptr_t)gdl * 2654435761u ^ room * 40503u;
    return (h >> 9) % E3D_BUCKETS;
}

static inline uint32_t e3d_seg_of(uintptr_t w1) {
    return (w1 & 1u) != 0 ? (uint32_t)((w1 >> 24) & 0x0fu) : 0u;
}

/* Words of the list up to and including G_ENDDL; 0 if longer than allowed. */
static uint32_t e3d_list_checksum(const Gfx* gdl, uint32_t* sum) {
    /* 32-bit arithmetic only: the R5900 has no 64-bit multiply */
    uint32_t h = 0x811c9dc5u;
    for (uint32_t i = 0; i < E3D_MAX_CMDS; ++i) {
        const uint32_t w0 = (uint32_t)gdl[i].words.w0;
        const uint32_t w1 = (uint32_t)gdl[i].words.w1;
        h = ((h << 5) | (h >> 27)) ^ w0;
        h = ((h << 5) | (h >> 27)) ^ (w1 * 0x9e3779b1u);
        if ((w0 >> 24) == (uint8_t)G_ENDDL) {
            *sum = h;
            return i + 1u;
        }
    }
    return 0;
}

/* Compile scratch, reused for every list. */
static struct {
    struct E3dOp ops[E3D_MAX_OPS];
    struct E3dItem items[E3D_MAX_ITEMS];
    struct E3dChunk chunks[E3D_MAX_CHUNKS];
    struct E3dTri tris[E3D_MAX_TRIS];
    struct E3dLoad loads[E3D_MAX_LOADS];
    uint16_t chunk_loads[E3D_MAX_CHUNK_LOADS];
    Gfx mtx[E3D_MAX_MTX];
    uint16_t op_first[E3D_MAX_OPS];
    uint16_t op_count[E3D_MAX_OPS];
    uint8_t epoch_linear[E3D_MAX_MTX + 1];
} e3d_tmp;

/* Builder state for one compile. */
struct E3dBuild {
    int16_t slot_load[E3D_SLOTS]; /* load index that filled each slot */
    uint32_t chunk_slots;         /* slots the open chunk's triangles read */
    uint32_t nops, nitems, nchunks, ntris, nloads, nchunk_loads, nmtx;
    int cur_op;
    int cur_chunk;
    uint16_t epoch;
};

static bool e3d_add_item(struct E3dBuild* B, uint8_t kind, uint32_t index) {
    if (B->nitems >= E3D_MAX_ITEMS) {
        return false;
    }
    struct E3dItem* I = &e3d_tmp.items[B->nitems++];
    I->kind = kind;
    I->pad = 0;
    I->index = (uint16_t)index;
    ++e3d_tmp.ops[B->cur_op].nitems;
    return true;
}

static bool e3d_close_chunk(struct E3dBuild* B) {
    if (B->cur_chunk < 0) {
        return true;
    }
    struct E3dChunk* K = &e3d_tmp.chunks[B->cur_chunk];
    /* its loads, in list order (each the latest for the slots read) */
    K->ld0 = (uint16_t)B->nchunk_loads;
    int prev = -1;
    for (;;) {
        int next = 0x7fff;
        for (int s = 0; s < E3D_SLOTS; ++s) {
            const int l = B->slot_load[s];
            if ((B->chunk_slots & (1u << s)) && l > prev && l < next) {
                next = l;
            }
        }
        if (next == 0x7fff) {
            break;
        }
        if (B->nchunk_loads >= E3D_MAX_CHUNK_LOADS) {
            return false;
        }
        e3d_tmp.chunk_loads[B->nchunk_loads++] = (uint16_t)next;
        /* a triangle reading a load of an earlier matrix epoch: that
         * epoch must replay in order, under its own matrix */
        if (e3d_tmp.loads[next].epoch != K->epoch) {
            e3d_tmp.epoch_linear[e3d_tmp.loads[next].epoch] = 1;
            e3d_tmp.epoch_linear[K->epoch] = 1;
        }
        prev = next;
    }
    K->nld = (uint16_t)(B->nchunk_loads - K->ld0);
    B->cur_chunk = -1;
    B->chunk_slots = 0;
    return true;
}

static struct E3dProg* e3d_compile(const Gfx* gdl, uint32_t room, uint16_t gen, uint8_t kind,
                                   uint32_t ncmds_checked, uint32_t checksum) {
    const uint32_t seg_vtx = kind == E3D_KIND_ROOM ? E3D_SEG_ROOM_VTX : E3D_SEG_MODEL_VTX;
    const Vtx* vbase = (const Vtx*)segmentPointers[seg_vtx];
    struct E3dProg* P = (struct E3dProg*)e3d_alloc(sizeof(struct E3dProg));
    if (!P) {
        e3d_reset();
        P = (struct E3dProg*)e3d_alloc(sizeof(struct E3dProg));
    }
    memset(P, 0, sizeof(*P));
    P->gdl = gdl;
    P->room = (uint16_t)room;
    P->gen = gen;
    P->kind = kind;
    P->ncmds = ncmds_checked;
    P->checksum = checksum;
    const uint32_t b = e3d_bucket(gdl, room);
    P->next = e3d.buckets[b];
    e3d.buckets[b] = P;
    ++e3d.n_compiles;
    if (!vbase) {
        return P;
    }

    struct E3dBuild B;
    memset(&B, 0, sizeof(B));
    for (int i = 0; i < E3D_SLOTS; ++i) {
        B.slot_load[i] = -1;
    }
    B.cur_op = -1;
    B.cur_chunk = -1;
    memset(e3d_tmp.epoch_linear, 0, sizeof(e3d_tmp.epoch_linear));
    uint32_t colbase = 0;
    uint8_t colseg = 0;
    bool col_valid = false;
    bool chunk_has_tris = false;

    for (uint32_t ncmds = 0;; ++ncmds) {
        if (ncmds >= E3D_MAX_CMDS) {
            return P;
        }
        const Gfx* c = &gdl[ncmds];
        const uint32_t op = (uint32_t)(c->words.w0 >> 24);
        if (op == (uint8_t)G_ENDDL) {
            break;
        }
        if (op == (uint8_t)G_DL || op == (uint8_t)G_E3D_ROOMLEAF_EXT || op == (uint8_t)G_E3D_MODELDL_EXT) {
            return P; /* nested lists: keep the original */
        }
        const bool mtx = op == (uint8_t)G_MTX || op == (uint8_t)G_POPMTX;
        if (mtx && kind == E3D_KIND_ROOM) {
            return P;
        }
        const bool geom = mtx || op == (uint8_t)G_VTX || op == (uint8_t)G_COL || op == (uint8_t)G_TRI1 ||
                          op == (uint8_t)G_TRI4;
        const uint8_t okind = geom ? E3D_OP_GEOM : E3D_OP_STATE;
        if (B.cur_op < 0 || e3d_tmp.ops[B.cur_op].kind != okind) {
            if (!e3d_close_chunk(&B) || B.nops >= E3D_MAX_OPS) {
                return P;
            }
            chunk_has_tris = false;
            B.cur_op = (int)B.nops++;
            struct E3dOp* O = &e3d_tmp.ops[B.cur_op];
            memset(O, 0, sizeof(*O));
            O->kind = okind;
            O->item0 = (uint16_t)B.nitems;
            O->load0 = (uint16_t)B.nloads;
            e3d_tmp.op_first[B.cur_op] = (uint16_t)ncmds;
            e3d_tmp.op_count[B.cur_op] = 0;
        }
        ++e3d_tmp.op_count[B.cur_op];
        if (!geom) {
            continue;
        }

        const uintptr_t w0 = c->words.w0, w1 = c->words.w1;
        if (mtx) {
            if (!e3d_close_chunk(&B) || B.nmtx >= E3D_MAX_MTX || B.epoch + 1u > E3D_MAX_MTX) {
                return P;
            }
            chunk_has_tris = false;
            e3d_tmp.mtx[B.nmtx] = *c;
            if (!e3d_add_item(&B, E3D_ITEM_MTX, B.nmtx)) {
                return P;
            }
            ++B.nmtx;
            ++B.epoch;
            continue;
        }
        if (op == (uint8_t)G_COL) {
            const uint32_t seg = e3d_seg_of(w1);
            if ((w1 & 2u) != 0 ||
                (kind == E3D_KIND_ROOM ? seg != E3D_SEG_ROOM_COL
                                       : seg != E3D_SEG_MODEL_COL1 && seg != E3D_SEG_MODEL_COL2)) {
                return P;
            }
            colbase = (uint32_t)((w1 & 0x00fffffeu) >> 2);
            colseg = seg == E3D_SEG_MODEL_COL2 ? 1 : 0;
            col_valid = colbase <= 0xffffu;
            continue;
        }
        if (op == (uint8_t)G_VTX) {
            const uint32_t n = (uint32_t)(w0 & 0xffffu) / sizeof(Vtx);
            const uint32_t dest = (uint32_t)(w0 >> 16) & 0x0fu;
            const uint32_t ofs = (uint32_t)(w1 & 0x00fffffeu);
            if (e3d_seg_of(w1) != seg_vtx || ofs % sizeof(Vtx) != 0 || n == 0 || dest + n > E3D_SLOTS ||
                dest + n > MAX_VERTICES || !col_valid || ofs / sizeof(Vtx) + n > 0xffffu ||
                B.nloads >= E3D_MAX_LOADS) {
                return P;
            }
            /* a load after triangles starts a new chunk */
            if (chunk_has_tris) {
                if (!e3d_close_chunk(&B)) {
                    return P;
                }
                chunk_has_tris = false;
            }
            struct E3dLoad* L = &e3d_tmp.loads[B.nloads];
            memset(L, 0, sizeof(*L));
            L->first = (uint16_t)(ofs / sizeof(Vtx));
            L->n = (uint8_t)n;
            L->dest = (uint8_t)dest;
            L->colbase = (uint16_t)colbase;
            L->colseg = colseg;
            L->epoch = B.epoch;
            for (uint32_t i = 0; i < n; ++i) {
                B.slot_load[dest + i] = (int16_t)B.nloads;
            }
            if (!e3d_add_item(&B, E3D_ITEM_LOAD, B.nloads)) {
                return P;
            }
            ++B.nloads;
            continue;
        }

        uint8_t idx[4][3];
        int count = 0;
        if (op == (uint8_t)G_TRI1) {
            idx[0][0] = (uint8_t)(((w1 >> 16) & 0xffu) / 10u);
            idx[0][1] = (uint8_t)(((w1 >> 8) & 0xffu) / 10u);
            idx[0][2] = (uint8_t)((w1 & 0xffu) / 10u);
            count = 1;
        } else {
            for (int t = 0; t < 4; ++t) {
                const uint8_t x = (uint8_t)((w1 >> (8 * t)) & 0x0fu);
                const uint8_t y = (uint8_t)((w1 >> (8 * t + 4)) & 0x0fu);
                const uint8_t z = (uint8_t)((w0 >> (4 * t)) & 0x0fu);
                if (x | y | z) {
                    idx[count][0] = x;
                    idx[count][1] = y;
                    idx[count][2] = z;
                    ++count;
                }
            }
        }
        for (int t = 0; t < count; ++t) {
            if (B.ntris >= E3D_MAX_TRIS) {
                return P;
            }
            if (B.cur_chunk < 0) {
                if (B.nchunks >= E3D_MAX_CHUNKS) {
                    return P;
                }
                B.cur_chunk = (int)B.nchunks++;
                struct E3dChunk* K = &e3d_tmp.chunks[B.cur_chunk];
                memset(K, 0, sizeof(*K));
                K->tri0 = (uint16_t)B.ntris;
                K->epoch = B.epoch;
                for (int a = 0; a < 3; ++a) {
                    K->lo[a] = 32767.0f;
                    K->hi[a] = -32768.0f;
                }
                if (!e3d_add_item(&B, E3D_ITEM_CHUNK, (uint32_t)B.cur_chunk)) {
                    return P;
                }
            }
            struct E3dChunk* K = &e3d_tmp.chunks[B.cur_chunk];
            struct E3dTri* T = &e3d_tmp.tris[B.ntris++];
            T->colseg = 0;
            for (int k = 0; k < 3; ++k) {
                const uint8_t s = idx[t][k];
                if (s >= E3D_SLOTS || B.slot_load[s] < 0) {
                    return P;
                }
                const struct E3dLoad* L = &e3d_tmp.loads[B.slot_load[s]];
                const uint32_t vi = L->first + (s - L->dest);
                const Vtx* v = &vbase[vi];
                T->vtx[k] = (uint16_t)vi;
                T->col[k] = L->colbase;
                T->ld[k] = (uint16_t)B.slot_load[s];
                if (B.slot_load[s] < (int)e3d_tmp.ops[B.cur_op].load0) {
                    e3d_tmp.ops[B.cur_op].open = 1;
                }
                T->slot[k] = s;
                T->colseg |= (uint8_t)(L->colseg << k);
                B.chunk_slots |= 1u << s;
                for (int a = 0; a < 3; ++a) {
                    const float p = v->v[a];
                    K->lo[a] = p < K->lo[a] ? p : K->lo[a];
                    K->hi[a] = p > K->hi[a] ? p : K->hi[a];
                }
            }
            ++K->ntris;
            chunk_has_tris = true;
        }
    }
    if (!e3d_close_chunk(&B)) {
        return P;
    }
    /* An epoch with a load read across a matrix change replays in order. */
    for (uint32_t i = 0; i <= B.epoch; ++i) {
        P->skinned |= e3d_tmp.epoch_linear[i];
    }
    for (uint32_t i = 0; i < B.nops; ++i) {
        P->open |= e3d_tmp.ops[i].open;
    }
    for (uint32_t i = 0; i < B.nloads; ++i) {
        if (e3d_tmp.epoch_linear[e3d_tmp.loads[i].epoch]) {
            e3d_tmp.loads[i].flags |= E3D_FLAG_LINEAR;
        }
    }
    for (uint32_t i = 0; i < B.nchunks; ++i) {
        if (e3d_tmp.epoch_linear[e3d_tmp.chunks[i].epoch]) {
            e3d_tmp.chunks[i].flags |= E3D_FLAG_LINEAR;
        }
    }

    /* Commit into the arena. */
    struct E3dOp* ops = (struct E3dOp*)e3d_alloc(B.nops * sizeof(struct E3dOp) + 16u);
    struct E3dItem* items = (struct E3dItem*)e3d_alloc(B.nitems * sizeof(struct E3dItem) + 16u);
    struct E3dChunk* chunks = (struct E3dChunk*)e3d_alloc(B.nchunks * sizeof(struct E3dChunk) + 16u);
    struct E3dTri* tris = (struct E3dTri*)e3d_alloc(B.ntris * sizeof(struct E3dTri) + 16u);
    struct E3dLoad* loads = (struct E3dLoad*)e3d_alloc(B.nloads * sizeof(struct E3dLoad) + 16u);
    uint16_t* chunk_loads = (uint16_t*)e3d_alloc(B.nchunk_loads * sizeof(uint16_t) + 16u);
    Gfx* mtxs = (Gfx*)e3d_alloc(B.nmtx * sizeof(Gfx) + 16u);
    if (!ops || !items || !chunks || !tris || !loads || !chunk_loads || !mtxs) {
        return P; /* arena full: plain list until the next reset */
    }
    memcpy(ops, e3d_tmp.ops, B.nops * sizeof(struct E3dOp));
    memcpy(items, e3d_tmp.items, B.nitems * sizeof(struct E3dItem));
    memcpy(chunks, e3d_tmp.chunks, B.nchunks * sizeof(struct E3dChunk));
    memcpy(tris, e3d_tmp.tris, B.ntris * sizeof(struct E3dTri));
    memcpy(loads, e3d_tmp.loads, B.nloads * sizeof(struct E3dLoad));
    memcpy(chunk_loads, e3d_tmp.chunk_loads, B.nchunk_loads * sizeof(uint16_t));
    memcpy(mtxs, e3d_tmp.mtx, B.nmtx * sizeof(Gfx));
    for (uint32_t o = 0; o < B.nops; ++o) {
        if (ops[o].kind != E3D_OP_STATE) {
            continue;
        }
        const uint32_t n = e3d_tmp.op_count[o];
        Gfx* dl = (Gfx*)e3d_alloc((n + 1u) * sizeof(Gfx));
        if (!dl) {
            return P;
        }
        memcpy(dl, &gdl[e3d_tmp.op_first[o]], n * sizeof(Gfx));
        dl[n].words.w0 = (uintptr_t)(uint8_t)G_ENDDL << 24;
        dl[n].words.w1 = 0;
        ops[o].dl = dl;
    }
    P->ops = ops;
    P->items = items;
    P->chunks = chunks;
    P->tris = tris;
    P->loads = loads;
    P->chunk_loads = chunk_loads;
    P->mtx = mtxs;
    P->nops = (uint16_t)B.nops;
    P->ok = 1;
    return P;
}

static struct E3dProg* e3d_find(const Gfx* gdl, uint32_t room, uint8_t kind) {
    for (struct E3dProg* P = e3d.buckets[e3d_bucket(gdl, room)]; P; P = P->next) {
        if (P->gdl == gdl && P->room == room && P->kind == kind) {
            return P;
        }
    }
    return nullptr;
}

/* The object context for the current transform, as gfx_obj_try_defer picks it. */
static int e3d_obj_ctx(void) {
    int k = gfx_obj.last_ctx;
    if (k < 0 || !gfx_obj_ctx_current(&gfx_obj.ctxs[k])) {
        k = (int)(gfx_obj.next_ctx++ % GFX_OBJ_CTX);
        if (gfx_obj.batch_ctx == k) {
            gfx_ps2_direct_flush_records();
        }
        gfx_obj_materialize_ctx(k);
        gfx_obj_ctx_build(&gfx_obj.ctxs[k]);
        gfx_obj.last_ctx = k;
    }
    return k;
}

struct E3dSegs {
    const Vtx* vbase;
    const struct NormalColor* cbase[2];
    /* the load whose vertices each slot holds, during one GEOM op: a load
     * read by several chunks is transformed once, as the list did */
    int16_t resident[E3D_SLOTS];
};

static inline void e3d_load(const struct E3dProg* P, uint32_t li, struct E3dSegs* G) {
    const struct E3dLoad* L = &P->loads[li];
    rsp.vertex_colors = G->cbase[L->colseg] + L->colbase;
    gfx_sp_vertex(L->n, L->dest, G->vbase + L->first);
    for (uint32_t i = 0; i < L->n; ++i) {
        G->resident[L->dest + i] = (int16_t)li;
    }
}

static inline bool e3d_load_resident(const struct E3dProg* P, uint32_t li, const struct E3dSegs* G) {
    const struct E3dLoad* L = &P->loads[li];
    for (uint32_t i = 0; i < L->n; ++i) {
        if (G->resident[L->dest + i] != (int16_t)li) {
            return false;
        }
    }
    return true;
}

/* What the display list would have done for this chunk, minus decoding. */
static void e3d_chunk_slow(const struct E3dProg* P, const struct E3dChunk* K, struct E3dSegs* G) {
    for (uint32_t i = 0; i < K->nld; ++i) {
        const uint32_t li = P->chunk_loads[K->ld0 + i];
        if (!e3d_load_resident(P, li, G)) {
            e3d_load(P, li, G);
        }
    }
    for (uint32_t t = 0; t < K->ntris; ++t) {
        const struct E3dTri* T = &P->tris[K->tri0 + t];
        gfx_sp_tri1(T->slot[0], T->slot[1], T->slot[2], false);
    }
    e3d.n_slow_tris += K->ntris;
}

static void e3d_chunk_fast(const struct E3dProg* P, const struct E3dChunk* K, int k, const struct GfxObjCtx* X,
                           uint32_t cull_mode, const struct E3dSegs* G) {
    const int32_t ts = (int32_t)X->tex_s, tt = (int32_t)X->tex_t;
    for (uint32_t t = 0; t < K->ntris; ++t) {
        const struct E3dTri* T = &P->tris[K->tri0 + t];
        const Vtx* vs[3] = { &G->vbase[T->vtx[0]], &G->vbase[T->vtx[1]], &G->vbase[T->vtx[2]] };
#if !defined(PERFECT_DARK_PS2_DISABLE_CULLING)
        if (cull_mode != 0) {
            const float pa[3] = { (float)vs[0]->v[0], (float)vs[0]->v[1], (float)vs[0]->v[2] };
            const float pb[3] = { (float)vs[1]->v[0], (float)vs[1]->v[1], (float)vs[1]->v[2] };
            const float pc[3] = { (float)vs[2]->v[0], (float)vs[2]->v[1], (float)vs[2]->v[2] };
            const float cross = gfxPs2ObjectCullCross(X->alpha, pa, pb, pc);
            if (cull_mode == G_CULL_FRONT ? cross <= 0 : cross >= 0) {
                continue;
            }
        }
#endif
        if (gfx_ps2_direct.batch_count == 0 || gfx_obj.batch_ctx != k) {
            gfx_ps2_direct_begin_batch(k);
            gfx_obj_start_batch(k);
        }
        struct Ps2GsVu1ObjectVertex* out =
            (struct Ps2GsVu1ObjectVertex*)gfx_ps2_direct.batch + gfx_ps2_direct.batch_count;
        for (int i = 0; i < 3; ++i) {
            const Vtx* v = vs[i];
            gfx_obj_store(&out[i], v, ts, tt, G->cbase[(T->colseg >> i) & 1u][T->col[i] + (v->colour >> 2)]);
        }
        gfx_ps2_direct.batch_count += 3;
        ++e3d.n_tris;
        if (gfx_ps2_direct.batch_count + 3 > GFX_OBJ_MAX_VERTICES) {
            gfx_ps2_direct_flush();
        }
    }
}

/*
 * Fast3D builds the triangle setup (and imports textures) only when a
 * visible triangle needs it, so classification comes first: a chunk off
 * screen must not cost a setup.
 */
static bool e3d_fast_ok(const struct GfxObjCtx* X) {
    gfx_tri_ensure_setup();
    if (!gfx_obj.setup_ok) {
        return false;
    }
    const struct GfxTriSetup* S = &gfx_tri_setup_cache;
    const struct GfxPs2DirectContext* C = &gfx_ps2_direct.key.ctx;
    if (gfx_obj.fog_linear < 0) {
        gfx_obj.fog_linear = gfxPs2DirectFogLinear() ? 1 : 0;
    }
    if (gfx_ps2_direct.active && gfx_obj.prog_serial != gfx_ps2_direct.serial) {
        gfx_obj_update_prog(S);
    }
    return gfx_ps2_direct.active && gfx_obj.prog_ok && !gfx_ps2_direct.rec_lod && !C->two_pass &&
           S->z_is_from_0_to_1 == (X->z_is_from_0_to_1 != 0) && S->invert_y == (X->invert_y != 0) &&
           C->guard_x >= X->guard_x && C->guard_y >= X->guard_y && (!C->fog || gfx_obj.fog_linear == 1);
}

/*
 * Bounds of a model chunk from the instance's live vertices: the union of
 * its loads' bounds, each load's computed once per replay (every vertex
 * read once, in 16-bit integers).
 */
static struct {
    int16_t lo[E3D_MAX_LOADS][3];
    int16_t hi[E3D_MAX_LOADS][3];
    uint32_t stamp[E3D_MAX_LOADS];
    uint32_t serial;
} e3d_lb;

static void e3d_chunk_bounds(const struct E3dProg* P, const struct E3dChunk* K, const struct E3dSegs* G,
                             float lo[3], float hi[3]) {
    int32_t l0 = 32767, l1 = 32767, l2 = 32767, h0 = -32768, h1 = -32768, h2 = -32768;
    for (uint32_t i = 0; i < K->nld; ++i) {
        const uint32_t li = P->chunk_loads[K->ld0 + i];
        if (e3d_lb.stamp[li] != e3d_lb.serial) {
            const struct E3dLoad* L = &P->loads[li];
            const Vtx* v = &G->vbase[L->first];
            int32_t a0 = 32767, a1 = 32767, a2 = 32767, b0 = -32768, b1 = -32768, b2 = -32768;
            for (uint32_t n = 0; n < L->n; ++n, ++v) {
                const int32_t x = v->v[0], y = v->v[1], z = v->v[2];
                a0 = x < a0 ? x : a0;
                b0 = x > b0 ? x : b0;
                a1 = y < a1 ? y : a1;
                b1 = y > b1 ? y : b1;
                a2 = z < a2 ? z : a2;
                b2 = z > b2 ? z : b2;
            }
            e3d_lb.lo[li][0] = (int16_t)a0;
            e3d_lb.lo[li][1] = (int16_t)a1;
            e3d_lb.lo[li][2] = (int16_t)a2;
            e3d_lb.hi[li][0] = (int16_t)b0;
            e3d_lb.hi[li][1] = (int16_t)b1;
            e3d_lb.hi[li][2] = (int16_t)b2;
            e3d_lb.stamp[li] = e3d_lb.serial;
        }
        l0 = e3d_lb.lo[li][0] < l0 ? e3d_lb.lo[li][0] : l0;
        l1 = e3d_lb.lo[li][1] < l1 ? e3d_lb.lo[li][1] : l1;
        l2 = e3d_lb.lo[li][2] < l2 ? e3d_lb.lo[li][2] : l2;
        h0 = e3d_lb.hi[li][0] > h0 ? e3d_lb.hi[li][0] : h0;
        h1 = e3d_lb.hi[li][1] > h1 ? e3d_lb.hi[li][1] : h1;
        h2 = e3d_lb.hi[li][2] > h2 ? e3d_lb.hi[li][2] : h2;
    }
    lo[0] = (float)l0;
    lo[1] = (float)l1;
    lo[2] = (float)l2;
    hi[0] = (float)h0;
    hi[1] = (float)h1;
    hi[2] = (float)h2;
}

/* Per-op replay state that carries across chunks of one matrix epoch. */
struct E3dGeomState {
    int k;               /* object context, -1 until needed */
    int fast;            /* -1 undecided, 0/1 */
    bool lit;
    uint32_t cull_mode;
};

static void e3d_chunk(const struct E3dProg* P, const struct E3dChunk* K, struct E3dSegs* G,
                      struct E3dGeomState* St) {
    if (K->flags & E3D_FLAG_LINEAR) {
        /* its loads already ran in order: just the triangles */
        for (uint32_t t = 0; t < K->ntris; ++t) {
            const struct E3dTri* T = &P->tris[K->tri0 + t];
            gfx_sp_tri1(T->slot[0], T->slot[1], T->slot[2], false);
        }
        e3d.n_linear_tris += K->ntris;
        return;
    }
    if (St->k < 0) {
        St->k = e3d_obj_ctx();
    }
    const struct GfxObjCtx* X = &gfx_obj.ctxs[St->k];
    float blo[3], bhi[3];
    const float* lo = K->lo;
    const float* hi = K->hi;
    if (P->kind == E3D_KIND_MODEL) {
        e3d_chunk_bounds(P, K, G, blo, bhi);
        lo = blo;
        hi = bhi;
    }
    uint32_t mask = 0;
    const enum Ps2ObjectBoxClass cls = gfx_obj_classify(X, lo, hi, &mask);
    if (cls == PS2_OBJECT_BOX_OUTSIDE) {
        ++e3d.n_outside; /* Fast3D rejects every triangle of it */
        return;
    }
    if (St->fast < 0) {
        St->fast = !St->lit && e3d_fast_ok(X) ? 1 : 0;
    }
    if (!St->fast) {
        ++e3d.n_unfit;
        e3d_chunk_slow(P, K, G);
        return;
    }
    if (cls == PS2_OBJECT_BOX_MIXED) {
        ++e3d.n_mixed;
        e3d_chunk_slow(P, K, G);
        return;
    }
    ++e3d.n_inside;
#if !defined(PERFECT_DARK_PS2_DISABLE_CULLING)
    if (St->cull_mode == G_CULL_BOTH) {
        return;
    }
#endif
    if (buf_vbo_len > 0) {
        gfx_flush();
    }
    e3d_chunk_fast(P, K, St->k, X, St->cull_mode, G);
}

static void e3d_geom(const struct E3dProg* P, const struct E3dOp* O, struct E3dSegs* G) {
    memset(G->resident, 0xff, sizeof(G->resident));
    struct E3dGeomState St;
    St.k = -1;
    St.fast = -1;
    St.lit = (rsp.geometry_mode & G_LIGHTING) != 0;
    St.cull_mode = rsp.geometry_mode & G_CULL_BOTH;
    const struct NormalColor* saved_cols = rsp.vertex_colors;
    for (uint32_t i = 0; i < O->nitems; ++i) {
        const struct E3dItem* I = &P->items[O->item0 + i];
        if (I->kind == E3D_ITEM_LOAD) {
            const struct E3dLoad* L = &P->loads[I->index];
            if (L->flags & E3D_FLAG_LINEAR) {
                /* skinned joins replay in order: no deferral */
                g_e3d_no_defer = true;
                e3d_load(P, I->index, G);
                g_e3d_no_defer = false;
            }
        } else if (I->kind == E3D_ITEM_CHUNK) {
            const struct E3dChunk* K = &P->chunks[I->index];
            if (K->ntris != 0) {
                e3d_chunk(P, K, G, &St);
            }
        } else {
            const Gfx* cmd = &P->mtx[I->index];
            if ((uint32_t)(cmd->words.w0 >> 24) == (uint8_t)G_MTX) {
                gfx_sp_matrix(C0(16, 8), (const int32_t*)seg_addr(cmd->words.w1));
            } else {
                gfx_sp_pop_matrix(1);
            }
            St.k = -1; /* the transform changed */
            memset(G->resident, 0xff, sizeof(G->resident));
        }
    }
    rsp.vertex_colors = saved_cols;
}

/*
 * Stage 5: skinned and lit model geometry on VU1 (gs_vu1_skin.vsm).
 *
 * A GEOM op of a model list is a run of loads, triangles and matrix
 * commands. Every matrix epoch (the items between two matrix commands)
 * becomes a palette entry: its clip transform, and, for lit geometry, the
 * light and lookat directions in that matrix's object space, which is what
 * Fast3D computes per G_MTX. Each corner of each triangle is sent raw (24
 * bytes) with the entry of the load it came from, so a triangle that joins
 * a thigh block and a shin block is transformed exactly as Fast3D does it,
 * by VU1. Lighting, texture generation and backface culling run there too;
 * the EE classifies one box per load and packs corners.
 *
 * The op runs this way when every load's box lies inside the guard band
 * and in front of the near plane (nothing to clip), and the render state
 * is one the object batches take. Otherwise the op replays its commands in
 * list order through Fast3D (e3d_linear_geom), as the plain list would.
 */
enum {
    E3D_SKIN_CTX = -2,
    E3D_SKIN_MAX_EPOCHS = 64,
};

static struct {
    /* per matrix epoch of the op being run */
    struct Ps2GsVu1SkinEntry entry[E3D_SKIN_MAX_EPOCHS] __attribute__((aligned(16)));
    float mv[E3D_SKIN_MAX_EPOCHS][4][4]; /* modelview of lit epochs */
    float vpl[E3D_SKIN_MAX_EPOCHS][3][7][4] __attribute__((aligned(16)));
    /* per load: its epoch within the op, and the exact planes it is outside */
    uint8_t lepoch[E3D_MAX_LOADS];
    uint8_t lmask[E3D_MAX_LOADS];
    /* the batch being filled */
    struct Ps2GsVu1SkinConst k __attribute__((aligned(16)));
    /* two-texture trilerp: the second pass's constants, the tile clamps */
    struct Ps2GsVu1SkinConst k1 __attribute__((aligned(16)));
    struct Ps2GsVu1SkinBatch desc1;
    bool trilerp;
    float clamp_bounds[2][2];
    uint8_t clamp_flags[2][2];
    struct Ps2GsVu1SkinEntry bentry[PS2_GS_VU1_SKIN_MAX_ENTRIES] __attribute__((aligned(16)));
    struct Ps2GsVu1SkinBatch desc;
    int8_t slot_of[E3D_SKIN_MAX_EPOCHS];
    uint8_t epoch_of[PS2_GS_VU1_SKIN_MAX_ENTRIES];
    uint32_t nentries;
    uint32_t maxv;
    /* counters (PD_PS2_OBJECT_STATS) */
    uint32_t n_ops, n_hidden, n_tris, n_skipped, n_batches, n_fail[8], n_state[6], n_dwhy[8];
    uint32_t cyc_run, cyc_skin, cyc_linear, cyc_failed, cyc_total, cyc_pass1, cyc_mtx, cyc_snap, n_epochs, n_loads, cyc_bounds, cyc_box, n_lverts;
} e3d_sk;

/* Draw the skin batch in gfx_ps2_direct.batch (called by the flush). */
static void e3d_skin_flush(void) {
    e3d_sk.desc.k = &e3d_sk.k;
    e3d_sk.desc.entries = e3d_sk.bentry;
    e3d_sk.desc.entry_count = e3d_sk.nentries;
    if (e3d_sk.trilerp) {
        e3d_sk.desc1 = e3d_sk.desc;
        e3d_sk.desc1.k = &e3d_sk.k1;
        e3d_sk.desc1.flags = e3d_sk.desc.flags | PS2_GS_VU1_TRANSFORM_FLAG_LOD;
        gfxPs2DirectDrawSkinTrilerp((const struct Ps2GsVu1ObjectVertex*)gfx_ps2_direct.batch,
                                    gfx_ps2_direct.batch_count, gfx_ps2_direct.batch_fog, &e3d_sk.desc, &e3d_sk.desc1,
                                    e3d_sk.clamp_bounds, e3d_sk.clamp_flags, &gfx_obj.batch_xf);
    } else {
        ps2GsVu1QueueSetSkin(&e3d_sk.desc);
        gfxPs2DirectDrawObject((const struct Ps2GsVu1ObjectVertex*)gfx_ps2_direct.batch, gfx_ps2_direct.batch_count,
                               gfx_ps2_direct.batch_trilerp, gfx_ps2_direct.batch_fog, &gfx_obj.batch_xf);
        ps2GsVu1QueueSetSkin(nullptr);
    }
    ++e3d_sk.n_batches;
    for (uint32_t i = 0; i < e3d_sk.nentries; ++i) {
        e3d_sk.slot_of[e3d_sk.epoch_of[i]] = -1;
    }
    e3d_sk.nentries = 0;
}


/*
 * Palette entry for the transform current now: the clip matrix only, as
 * gfx_obj_ctx_build derives it (everything else comes from the op's base
 * context X0). The light and lookat directions need this epoch's modelview
 * matrix, which is kept and only used if the op is drawn (e3d_skin_lights).
 */
static void e3d_skin_snap(uint32_t e, bool lit, const struct GfxObjCtx* X0) {
    struct Ps2GsVu1SkinEntry* E = &e3d_sk.entry[e];
    const float k = X0->aspect_scale / X0->aspect_ratio;
    for (int i = 0; i < 4; ++i) {
        const float* mp = rsp.MP_matrix[i];
        E->m[i][0] = X0->fb_active ? mp[0] : (X0->aspect_ofs * mp[3] + mp[0]) * k;
        E->m[i][1] = X0->invert_y ? -mp[1] : mp[1];
        E->m[i][2] = X0->z_is_from_0_to_1 ? (mp[2] + mp[3]) / 2.0f : mp[2];
        E->m[i][3] = mp[3];
    }
    /*
     * gfxPs2ObjectBuildPlanes, already in the four-lane groups the box test
     * reads: each plane is linear in a matrix row (x, y, z, w), so group g
     * of row i is x * Cx + y * Cy + z * Cz + w * Cw with constant columns.
     */
    float(*vpl)[7][4] = e3d_sk.vpl[e];
#if defined(_EE)
    {
        const float gx = X0->guard_x, gy = X0->guard_y;
        static float cols[3][4][4] __attribute__((aligned(16)));
        static float cgx, cgy;
        if (cgx != gx || cgy != gy) {
            const float c[3][4][4] = {
                { { 1, -1, 0, 0 }, { 0, 0, 1, -1 }, { 0, 0, 0, 0 }, { gx, gx, gy, gy } },
                { { 0, 0, 0, 1 }, { 0, 0, 0, 0 }, { 1, -1, 0, 0 }, { 0, 1, 1, 1 } },
                { { -1, 0, 0, 0 }, { 0, 1, -1, 0 }, { 0, 0, 0, 0 }, { 1, 1, 1, 0 } },
            };
            memcpy(cols, c, sizeof(cols));
            cgx = gx;
            cgy = gy;
        }
        float scratch[4] __attribute__((aligned(16)));
        for (int g = 0; g < 3; ++g) {
            __asm__ __volatile__("lqc2    $vf4, 0(%0)\n\t"
                                 "lqc2    $vf5, 16(%0)\n\t"
                                 "lqc2    $vf6, 32(%0)\n\t"
                                 "lqc2    $vf7, 48(%0)\n\t"
                                 :
                                 : "r"(cols[g])
                                 : "memory");
            for (int i = 0; i < 4; ++i) {
                __asm__ __volatile__("lqc2    $vf1, 0(%0)\n\t"
                                     "vmulax.xyzw  $ACC, $vf4, $vf1x\n\t"
                                     "vmadday.xyzw $ACC, $vf5, $vf1y\n\t"
                                     "vmaddaz.xyzw $ACC, $vf6, $vf1z\n\t"
                                     "vmaddw.xyzw  $vf2, $vf7, $vf1w\n\t"
                                     "vabs.xyzw    $vf3, $vf2\n\t"
                                     "sqc2    $vf2, 0(%1)\n\t"
                                     "sqc2    $vf3, 0(%2)\n\t"
                                     :
                                     : "r"(E->m[i]), "r"(vpl[g][i]), "r"(i < 3 ? vpl[g][4 + i] : scratch)
                                     : "memory");
            }
            if (g == 1) {
                vpl[1][3][2] -= 1e-3f; /* w >= min_w */
            }
        }
    }
#else
    float pl[4][PS2_OBJECT_PLANES], pa[3][PS2_OBJECT_PLANES];
    gfxPs2ObjectBuildPlanes(E->m, X0->guard_x, X0->guard_y, 1e-3f, pl, pa);
    for (int g = 0; g < 3; ++g) {
        for (int lane = 0; lane < 4; ++lane) {
            const int j = g * 4 + lane;
            for (int r = 0; r < 4; ++r) {
                vpl[g][r][lane] = pl[r][j];
            }
            for (int r = 0; r < 3; ++r) {
                vpl[g][4 + r][lane] = pa[r][j];
            }
        }
    }
#endif
    if (lit) {
        memcpy(e3d_sk.mv[e], rsp.modelview_matrix_stack[rsp.modelview_matrix_stack_size - 1], sizeof(e3d_sk.mv[e]));
    }
}

static void e3d_skin_dir(const float mv[4][4], const int8_t dir[3], float out[3]) {
    const float d[3] = { dir[0] / 127.f, dir[1] / 127.f, dir[2] / 127.f };
    gfx_transposed_matrix_mul(out, d, mv);
    gfx_normalize_vector(out);
}

/* Light and lookat directions of epoch e, in its object space, / 127. */
static void e3d_skin_lights(struct Ps2GsVu1SkinEntry* E, uint32_t e, uint32_t nl, bool texgen) {
    memset(E->light_x, 0, sizeof(float) * 4 * 6);
    for (uint32_t i = 0; i < nl; ++i) {
        float c[3];
        e3d_skin_dir(e3d_sk.mv[e], (const int8_t*)rsp.current_lights[i].dir, c);
        E->light_x[i] = c[0] * (1.0f / 127.0f);
        E->light_y[i] = c[1] * (1.0f / 127.0f);
        E->light_z[i] = c[2] * (1.0f / 127.0f);
    }
    if (texgen) {
        for (int i = 0; i < 2; ++i) {
            float c[3];
            e3d_skin_dir(e3d_sk.mv[e], (const int8_t*)rsp.lookat[i].dir, c);
            E->look_x[i] = c[0] * (1.0f / 127.0f);
            E->look_y[i] = c[1] * (1.0f / 127.0f);
            E->look_z[i] = c[2] * (1.0f / 127.0f);
        }
    }
}

/*
 * One load's box against epoch e's planes (gfxPs2ObjectBuildPlanes, built
 * by e3d_skin_snap; four lanes at a time on VU0 as gfx_obj_classify does):
 * true when no point needs clipping (guard band, 0 <= z <= w, w >= 1e-3);
 * *mask gets the exact planes the whole box is outside of.
 */
static bool e3d_skin_box(uint32_t e, const int32_t lo[3], const int32_t hi[3], uint8_t* mask) {
    const float(*vpl)[7][4] = e3d_sk.vpl[e];
    float vmin[12] __attribute__((aligned(16)));
    float vmax[12] __attribute__((aligned(16)));
    float ce[2][4] __attribute__((aligned(16))) = {
        { (lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f, 1.0f },
        { (hi[0] - lo[0]) * 0.5f, (hi[1] - lo[1]) * 0.5f, (hi[2] - lo[2]) * 0.5f, 0.0f },
    };
#if defined(_EE)
    __asm__ __volatile__("lqc2    $vf1, 0(%0)\n\t"
                         "lqc2    $vf2, 16(%0)\n\t"
                         :
                         : "r"(ce)
                         : "memory");
    for (int g = 0; g < 3; ++g) {
        __asm__ __volatile__("lqc2    $vf3, 0(%0)\n\t"
                             "lqc2    $vf4, 16(%0)\n\t"
                             "lqc2    $vf5, 32(%0)\n\t"
                             "lqc2    $vf6, 48(%0)\n\t"
                             "lqc2    $vf7, 64(%0)\n\t"
                             "lqc2    $vf8, 80(%0)\n\t"
                             "lqc2    $vf9, 96(%0)\n\t"
                             "vmulax.xyzw  $ACC, $vf3, $vf1x\n\t"
                             "vmadday.xyzw $ACC, $vf4, $vf1y\n\t"
                             "vmaddaz.xyzw $ACC, $vf5, $vf1z\n\t"
                             "vmaddw.xyzw  $vf10, $vf6, $vf0w\n\t"
                             "vmulax.xyzw  $ACC, $vf7, $vf2x\n\t"
                             "vmadday.xyzw $ACC, $vf8, $vf2y\n\t"
                             "vmaddz.xyzw  $vf11, $vf9, $vf2z\n\t"
                             "vsub.xyzw    $vf12, $vf10, $vf11\n\t"
                             "vadd.xyzw    $vf13, $vf10, $vf11\n\t"
                             "sqc2    $vf12, 0(%1)\n\t"
                             "sqc2    $vf13, 0(%2)\n\t"
                             :
                             : "r"(vpl[g]), "r"(&vmin[g * 4]), "r"(&vmax[g * 4])
                             : "memory");
    }
#else
    for (int g = 0; g < 3; ++g) {
        for (int l = 0; l < 4; ++l) {
            const float d = ce[0][0] * vpl[g][0][l] + ce[0][1] * vpl[g][1][l] + ce[0][2] * vpl[g][2][l] + vpl[g][3][l];
            const float r = ce[1][0] * vpl[g][4][l] + ce[1][1] * vpl[g][5][l] + ce[1][2] * vpl[g][6][l];
            vmin[g * 4 + l] = d - r;
            vmax[g * 4 + l] = d + r;
        }
    }
#endif
    /* sign bits instead of FPU compares and branches */
    uint32_t neg_max = 0, neg_min = 0;
    for (int j = 0; j < 12; ++j) {
        uint32_t a, b;
        memcpy(&a, &vmax[j], 4);
        memcpy(&b, &vmin[j], 4);
        neg_max |= (a >> 31) << j;
        neg_min |= (b >> 31) << j;
    }
    /* planes: 0..3 guard, 4 z >= 0, 5 z <= w, 6 w >= min, 7..10 exact sides */
    *mask = (uint8_t)(((neg_max >> 7) & 15u) | ((neg_max >> 4) & 1u) << 4 | ((neg_max >> 5) & 1u) << 5);
    return (neg_min & 0x7fu) == 0;
}


/*
 * The op's frame of reference: everything of an object context except the
 * matrices, rebuilt only when one of those inputs changes (rarely within a
 * frame). Private: borrowing one of gfx_obj's contexts would recycle it and
 * force its deferred blocks through the EE transform.
 */
static struct GfxObjCtx* e3d_skin_base(void) {
    static struct GfxObjCtx x0 __attribute__((aligned(16)));
    static bool valid;
    const struct GfxObjCtx* X = &x0;
    if (!valid || X->geometry_mode != rsp.geometry_mode || X->fb_active != fbActive ||
        X->aspect_ofs != rsp.aspect_ofs || X->aspect_scale != rsp.aspect_scale ||
        X->aspect_ratio != gfx_current_dimensions.aspect_ratio || X->tex_s != rsp.texture_scaling_factor.s ||
        X->tex_t != rsp.texture_scaling_factor.t || X->fog_mul != rsp.fog_mul || X->fog_offset != rsp.fog_offset ||
        X->fog_alpha != rdp.fog_color.a || X->viewport.x != rdp.viewport.x || X->viewport.y != rdp.viewport.y ||
        X->viewport.width != rdp.viewport.width || X->viewport.height != rdp.viewport.height) {
        gfx_obj_ctx_build(&x0);
        valid = true;
    }
    return &x0;
}

static struct {
    float stack[11][4][4];
    float mp[4][4];
    float p[4][4];
    uint8_t size;
} e3d_sk_saved;

/* EE cycle counter for the stats build; constant zero otherwise. */
static inline uint32_t e3d_cycles(void) {
#if defined(_EE) && defined(PD_PS2_OBJECT_STATS)
    uint32_t c;
    __asm__ __volatile__("mfc0 %0, $9" : "=r"(c));
    return c;
#else
    return 0;
#endif
}

static bool e3d_skin_fail(uint32_t why) {
    ++e3d_sk.n_fail[why & 7u];
    return false;
}

static bool e3d_skin_geom(const struct E3dProg* P, const struct E3dOp* O, struct E3dSegs* G) {
    const uint32_t gm = rsp.geometry_mode;
    const bool lit = (gm & G_LIGHTING) != 0;
    const bool texgen = lit && (gm & G_TEXTURE_GEN) != 0;
    const uint32_t cull = gm & G_CULL_BOTH;
    const uint32_t nl = lit ? (uint32_t)rsp.current_num_lights - 1u : 0u;
    if (texgen && ((gm & G_TEXTURE_GEN_LINEAR) != 0 || !rsp.lookat_enabled)) {
        return e3d_skin_fail(1);
    }
    if (cull == G_CULL_BOTH) {
        return e3d_skin_fail(2);
    }
    if (lit && (rsp.current_num_lights < 1 || nl > PS2_GS_VU1_SKIN_MAX_LIGHTS)) {
        return e3d_skin_fail(3);
    }
    uint32_t nep = 1;
    for (uint32_t i = 0; i < O->nitems; ++i) {
        nep += P->items[O->item0 + i].kind == E3D_ITEM_MTX;
    }
    if (nep > E3D_SKIN_MAX_EPOCHS) {
        return e3d_skin_fail(4);
    }
    /*
     * Pass 1, before any state work: the matrix commands run for real, each
     * epoch gets its palette entry and planes, each load its box test. An
     * op wholly outside one plane draws nothing (the plain list would have
     * rejected every triangle, without even building the triangle setup).
     */
    const uint32_t t0 = e3d_cycles();
    struct GfxObjCtx* X0 = e3d_skin_base();
    const uint8_t size0 = rsp.modelview_matrix_stack_size;
    memcpy(e3d_sk_saved.stack, rsp.modelview_matrix_stack, sizeof(e3d_sk_saved.stack));
    memcpy(e3d_sk_saved.mp, rsp.MP_matrix, sizeof(e3d_sk_saved.mp));
    memcpy(e3d_sk_saved.p, rsp.P_matrix, sizeof(e3d_sk_saved.p));
    const struct GfxObjCtx& X = *X0;
    uint32_t e = 0;
    e3d_skin_snap(0, lit, X0);
    bool safe = true;
    uint8_t all_out = 0xff;
    for (uint32_t i = 0; i < O->nitems; ++i) {
        const struct E3dItem* I = &P->items[O->item0 + i];
        if (I->kind == E3D_ITEM_MTX) {
            const Gfx* cmd = &P->mtx[I->index];
            const uint32_t m0 = e3d_cycles();
            if ((uint32_t)(cmd->words.w0 >> 24) == (uint8_t)G_MTX) {
                gfx_sp_matrix(C0(16, 8), (const int32_t*)seg_addr(cmd->words.w1));
            } else {
                gfx_sp_pop_matrix(1);
            }
            const uint32_t m1 = e3d_cycles();
            e3d_skin_snap(++e, lit, X0);
            e3d_sk.cyc_mtx += m1 - m0;
            e3d_sk.cyc_snap += e3d_cycles() - m1;
            ++e3d_sk.n_epochs;
        } else if (I->kind == E3D_ITEM_LOAD) {
            ++e3d_sk.n_loads;
            const uint32_t b0c = e3d_cycles();
            const struct E3dLoad* L = &P->loads[I->index];
            const Vtx* v = &G->vbase[L->first];
            int32_t a0 = 32767, a1 = 32767, a2 = 32767, b0 = -32768, b1 = -32768, b2 = -32768;
            for (uint32_t n = 0; n < L->n; ++n, ++v) {
                const int32_t x = v->v[0], y = v->v[1], z = v->v[2];
                a0 = x < a0 ? x : a0;
                b0 = x > b0 ? x : b0;
                a1 = y < a1 ? y : a1;
                b1 = y > b1 ? y : b1;
                a2 = z < a2 ? z : a2;
                b2 = z > b2 ? z : b2;
            }
            const int32_t lo[3] = { a0, a1, a2 };
            const int32_t hi[3] = { b0, b1, b2 };
            e3d_sk.lepoch[I->index] = (uint8_t)e;
            const uint32_t b1c = e3d_cycles();
            safe &= e3d_skin_box(e, lo, hi, &e3d_sk.lmask[I->index]);
            e3d_sk.cyc_bounds += b1c - b0c;
            e3d_sk.cyc_box += e3d_cycles() - b1c;
            e3d_sk.n_lverts += L->n;
            all_out &= e3d_sk.lmask[I->index];
        }
    }
    e3d_sk.cyc_pass1 += e3d_cycles() - t0;
    if (all_out != 0) {
        ++e3d_sk.n_hidden;
        return true;
    }
    bool trilerp = false;
    float tsc_s[2], tsc_t[2];
    bool modulate = false;
    const struct GfxTriSetup* S = &gfx_tri_setup_cache;
    bool state_ok = safe;
    if (state_ok) {
        gfx_tri_ensure_setup();
        if (!e3d_fast_ok(X0)) {
            trilerp = gfxPs2SkinTrilerpQuery(tsc_s, tsc_t, &modulate) && S->used_textures[0] &&
                      S->used_textures[1] && S->num_inputs >= (modulate ? 2 : 1) &&
                      (S->in_kind[0][0] == GFX_TRI_IN_LOD || S->in_kind[0][0] == GFX_TRI_IN_CONST) &&
                      !(modulate && S->in_kind[0][1] == GFX_TRI_IN_SHADE_ALPHA &&
                        S->in_kind[0][0] == GFX_TRI_IN_LOD) &&
                      !(modulate && S->in_kind[0][1] == GFX_TRI_IN_LOD);
            state_ok = trilerp;
#if defined(PD_PS2_OBJECT_STATS)
            if (!trilerp) {
                ++e3d_sk.n_state[!gfx_ps2_direct.active ? 1u : gfx_ps2_direct.key.ctx.two_pass ? 4u : 5u];
            }
#endif
        }
    }
    if (!state_ok) {
        rsp.modelview_matrix_stack_size = size0;
        memcpy(rsp.modelview_matrix_stack, e3d_sk_saved.stack, sizeof(e3d_sk_saved.stack));
        memcpy(rsp.MP_matrix, e3d_sk_saved.mp, sizeof(e3d_sk_saved.mp));
        memcpy(rsp.P_matrix, e3d_sk_saved.p, sizeof(e3d_sk_saved.p));
        rsp.lights_changed = true;
        return e3d_skin_fail(safe ? 5 : 6);
    }

    /* the batch constants: the record program, fog, lights, cull */
    struct Ps2GsVu1SkinConst* K = &e3d_sk.k;
    const struct Ps2GsVu1ObjectTransform* prog = &gfx_obj.prog;
    memcpy(K->fog, X.xf.fog, sizeof(K->fog));
    if (!gfx_ps2_direct.key.ctx.fog) {
        K->fog[1] = 255.0f;
        K->fog[3] = 0.0f;
    }
    memcpy(K->tex_mul, prog->tex_mul, sizeof(K->tex_mul));
    memcpy(K->tex_add, prog->tex_add, sizeof(K->tex_add));
    memcpy(K->tex_scale, prog->tex_scale, sizeof(K->tex_scale));
    memcpy(K->color, prog->color, sizeof(K->color));
    memcpy(K->color_add, prog->color_add, sizeof(K->color_add));
    memset(K->light, 0, sizeof(K->light));
    memset(K->ambient, 0, sizeof(K->ambient));
    if (lit) {
        for (uint32_t i = 0; i < nl; ++i) {
            for (int c = 0; c < 3; ++c) {
                K->light[i][c] = rsp.current_lights[i].col[c];
            }
        }
        for (int c = 0; c < 3; ++c) {
            K->ambient[c] = rsp.current_lights[nl].col[c];
        }
    }
    const int32_t ts = rsp.texture_scaling_factor.s, tt = rsp.texture_scaling_factor.t;
    K->misc[0] = (float)ts * 0.25f;
    K->misc[1] = (float)tt * 0.25f;
    /* the microprogram keeps a triangle iff its screen-space cross product
     * times this is negative (checked against Fast3D in CI and Defection:
     * back-face culling keeps sign -1 with no y flip) */
    K->misc[2] = (cull == G_CULL_FRONT ? 1.0f : -1.0f) * (X.invert_y ? -1.0f : 1.0f);
    K->misc[3] = -18446744073709551616.0f;
    K->lod[0] = 1.0f / 1024.0f;
    K->lod[1] = 0.3f * 255.0f;
    K->lod[2] = 0.7f * 255.0f;
    K->lod[3] = 0.0f;
    e3d_sk.trilerp = trilerp;
    if (trilerp) {
        /* pass 0: T0 * shade, alpha 0x80; pass 1: T1 * shade, alpha the
         * LOD fraction (the translator's opaque trilerp vertices) */
        if (!S->use_fog) {
            K->fog[1] = 255.0f;
            K->fog[3] = 0.0f;
        } else {
            memcpy(K->fog, X.xf.fog, sizeof(K->fog));
        }
        memset(K->tex_mul, 0, sizeof(float) * 12);
        memset(K->color, 0, sizeof(K->color));
        const float m = 128.0f / 255.0f;
        if (!modulate) {
            K->color_add[0] = K->color_add[1] = K->color_add[2] = 128.25f;
        } else if (S->in_kind[0][1] == GFX_TRI_IN_SHADE) {
            K->color[0][0] = K->color[1][1] = K->color[2][2] = m;
            K->color_add[0] = K->color_add[1] = K->color_add[2] = 0.5f;
        } else if (S->in_kind[0][1] == GFX_TRI_IN_SHADE_ALPHA) {
            K->color[3][0] = K->color[3][1] = K->color[3][2] = m;
            K->color_add[0] = K->color_add[1] = K->color_add[2] = 0.5f;
        } else {
            for (int c = 0; c < 3; ++c) {
                K->color_add[c] = (float)(int)(S->in_const_u8[1][c] * m + 0.5f) + 0.25f;
            }
        }
        K->color_add[3] = 128.25f;
        e3d_sk.k1 = *K;
        for (int t = 0; t < 2; ++t) {
            struct Ps2GsVu1SkinConst* Kt = t == 0 ? K : &e3d_sk.k1;
            Kt->tex_mul[0] = S->u_scale[t];
            Kt->tex_mul[1] = S->v_scale[t];
            Kt->tex_add[0] = S->u_bias[t];
            Kt->tex_add[1] = S->v_bias[t];
            Kt->tex_scale[0] = tsc_s[t];
            Kt->tex_scale[1] = tsc_t[t];
            e3d_sk.clamp_flags[t][0] = S->clamp_s[t];
            e3d_sk.clamp_flags[t][1] = S->clamp_t[t];
            e3d_sk.clamp_bounds[t][0] = S->tex_clamp_s[t];
            e3d_sk.clamp_bounds[t][1] = S->tex_clamp_t[t];
        }
        if (S->in_kind[0][0] == GFX_TRI_IN_LOD) {
            e3d_sk.k1.color[3][3] = m;
            e3d_sk.k1.color_add[3] = 0.5f;
        } else {
            e3d_sk.k1.color_add[3] = (float)(int)(S->in_const_u8[0][0] * m + 0.5f) + 0.25f;
        }
    }
    e3d_sk.desc.flags = (lit ? PS2_GS_VU1_TRANSFORM_FLAG_LIGHT : 0u) |
                        (texgen ? PS2_GS_VU1_TRANSFORM_FLAG_TEXGEN : 0u) |
                        (cull != 0 ? PS2_GS_VU1_TRANSFORM_FLAG_CULL : 0u);
    /* pass 2: the corners */
    if (buf_vbo_len > 0) {
        gfx_flush();
    }
    if (gfx_ps2_direct.batch_count != 0) {
        gfx_ps2_direct_flush_records();
    }
    /* object-batch constants: the backend's xform argument (unused by the
     * skin program and its EE fallback, but it must be a valid one) */
    gfx_obj.batch_xf = gfx_obj.prog;
    memcpy(gfx_obj.batch_xf.m, X0->xf.m, sizeof(gfx_obj.batch_xf.m));
    memcpy(gfx_obj.batch_xf.fog, K->fog, sizeof(gfx_obj.batch_xf.fog));
    memset(e3d_sk.slot_of, 0xff, nep);
    e3d_sk.nentries = 0;
    for (uint32_t i = 0; i < O->nitems; ++i) {
        const struct E3dItem* I = &P->items[O->item0 + i];
        if (I->kind != E3D_ITEM_CHUNK) {
            continue;
        }
        const struct E3dChunk* C = &P->chunks[I->index];
        for (uint32_t t = 0; t < C->ntris; ++t) {
            const struct E3dTri* T = &P->tris[C->tri0 + t];
            if ((e3d_sk.lmask[T->ld[0]] & e3d_sk.lmask[T->ld[1]] & e3d_sk.lmask[T->ld[2]]) != 0) {
                ++e3d_sk.n_skipped; /* wholly outside one plane: Fast3D rejects it */
                continue;
            }
            /* the palette entries it needs, a new batch when they do not fit */
            uint32_t need = 0;
            for (int c = 0; c < 3; ++c) {
                const uint8_t ep = e3d_sk.lepoch[T->ld[c]];
                bool dup = false;
                for (int d = 0; d < c; ++d) {
                    dup |= e3d_sk.lepoch[T->ld[d]] == ep;
                }
                need += !dup && e3d_sk.slot_of[ep] < 0;
            }
            if (gfx_ps2_direct.batch_count != 0 &&
                (gfx_obj.batch_ctx != E3D_SKIN_CTX || e3d_sk.nentries + need > PS2_GS_VU1_SKIN_MAX_ENTRIES ||
                 gfx_ps2_direct.batch_count + 3 > ps2GsVu1SkinMaxVertices(e3d_sk.nentries + need))) {
                gfx_ps2_direct_flush_records();
            }
            if (gfx_ps2_direct.batch_count == 0) {
                gfx_ps2_direct_begin_batch(E3D_SKIN_CTX);
            }
            struct Ps2GsVu1ObjectVertex* out =
                (struct Ps2GsVu1ObjectVertex*)gfx_ps2_direct.batch + gfx_ps2_direct.batch_count;
            for (int c = 0; c < 3; ++c) {
                const uint32_t li = T->ld[c];
                const uint8_t ep = e3d_sk.lepoch[li];
                if (e3d_sk.slot_of[ep] < 0) {
                    const uint32_t sl = e3d_sk.nentries++;
                    e3d_sk.slot_of[ep] = (int8_t)sl;
                    e3d_sk.epoch_of[sl] = ep;
                    memcpy(e3d_sk.bentry[sl].m, e3d_sk.entry[ep].m, sizeof(e3d_sk.bentry[sl].m));
                    if (lit) {
                        e3d_skin_lights(&e3d_sk.bentry[sl], ep, nl, texgen);
                    }
                }
                const struct E3dLoad* L = &P->loads[li];
                const Vtx* v = &G->vbase[T->vtx[c]];
                const struct NormalColor col = G->cbase[L->colseg][L->colbase + (v->colour >> 2)];
                uint64_t* q = (uint64_t*)&out[c];
                const uint64_t off = PS2_GS_VU1_SKIN_HEAD_QW + PS2_GS_VU1_SKIN_ENTRY_QW * (uint32_t)e3d_sk.slot_of[ep];
                q[0] = (uint64_t)(uint16_t)v->v[0] | (uint64_t)(uint16_t)v->v[1] << 16 |
                       (uint64_t)(uint16_t)v->v[2] << 32 | off << 48;
                q[1] = (uint64_t)(uint16_t)(short)(v->s * ts >> 16) | (uint64_t)(uint16_t)(short)(v->t * tt >> 16) << 16;
                if (lit) {
                    q[2] = (uint64_t)(uint16_t)(int16_t)col.x | (uint64_t)(uint16_t)(int16_t)col.y << 16 |
                           (uint64_t)(uint16_t)(int16_t)col.z << 32 | (uint64_t)col.a << 48;
                } else {
                    q[2] = (uint64_t)col.r | (uint64_t)col.g << 16 | (uint64_t)col.b << 32 | (uint64_t)col.a << 48;
                }
            }
            gfx_ps2_direct.batch_count += 3;
            ++e3d_sk.n_tris;
        }
    }
    if (gfx_ps2_direct.batch_count != 0) {
        gfx_ps2_direct_flush_records();
    }
    ++e3d_sk.n_ops;
    return true;
}

/* The op's commands in list order through Fast3D: what the plain list does. */
static void e3d_linear_geom(const struct E3dProg* P, const struct E3dOp* O, struct E3dSegs* G) {
    const struct NormalColor* saved_cols = rsp.vertex_colors;
    for (uint32_t i = 0; i < O->nitems; ++i) {
        const struct E3dItem* I = &P->items[O->item0 + i];
        if (I->kind == E3D_ITEM_LOAD) {
            e3d_load(P, I->index, G);
        } else if (I->kind == E3D_ITEM_CHUNK) {
            const struct E3dChunk* C = &P->chunks[I->index];
            for (uint32_t t = 0; t < C->ntris; ++t) {
                const struct E3dTri* T = &P->tris[C->tri0 + t];
                gfx_sp_tri1(T->slot[0], T->slot[1], T->slot[2], false);
            }
        } else {
            const Gfx* cmd = &P->mtx[I->index];
            if ((uint32_t)(cmd->words.w0 >> 24) == (uint8_t)G_MTX) {
                gfx_sp_matrix(C0(16, 8), (const int32_t*)seg_addr(cmd->words.w1));
            } else {
                gfx_sp_pop_matrix(1);
            }
        }
    }
    rsp.vertex_colors = saved_cols;
}

static void e3d_skin_run(const struct E3dProg* P, uint32_t depth) {
    const uint32_t c0 = e3d_cycles();
    struct E3dSegs G;
    G.vbase = (const Vtx*)segmentPointers[E3D_SEG_MODEL_VTX];
    G.cbase[0] = (const struct NormalColor*)segmentPointers[E3D_SEG_MODEL_COL1];
    G.cbase[1] = (const struct NormalColor*)segmentPointers[E3D_SEG_MODEL_COL2];
    for (uint32_t o = 0; o < P->nops; ++o) {
        const struct E3dOp* O = &P->ops[o];
        if (O->kind == E3D_OP_STATE) {
            gfx_run_dl(O->dl, depth + 1u);
        } else {
            const uint32_t g0 = e3d_cycles();
            if (!e3d_skin_geom(P, O, &G)) {
                const uint32_t l0 = e3d_cycles();
                e3d_linear_geom(P, O, &G);
                e3d_sk.cyc_linear += e3d_cycles() - l0;
                e3d_sk.cyc_failed += l0 - g0;
            } else {
                e3d_sk.cyc_skin += e3d_cycles() - g0;
            }
        }
    }
    e3d_sk.cyc_run += e3d_cycles() - c0;
}

static void e3d_run(const struct E3dProg* P, uint32_t depth) {
    struct E3dSegs G;
    ++e3d_lb.serial; /* per-load bounds are per instance */
    if (P->kind == E3D_KIND_ROOM) {
        G.vbase = (const Vtx*)segmentPointers[E3D_SEG_ROOM_VTX];
        G.cbase[0] = G.cbase[1] = (const struct NormalColor*)segmentPointers[E3D_SEG_ROOM_COL];
    } else {
        G.vbase = (const Vtx*)segmentPointers[E3D_SEG_MODEL_VTX];
        G.cbase[0] = (const struct NormalColor*)segmentPointers[E3D_SEG_MODEL_COL1];
        G.cbase[1] = (const struct NormalColor*)segmentPointers[E3D_SEG_MODEL_COL2];
    }
    for (uint32_t o = 0; o < P->nops; ++o) {
        const struct E3dOp* O = &P->ops[o];
        if (O->kind == E3D_OP_STATE) {
            gfx_run_dl(O->dl, depth + 1u);
        } else {
            e3d_geom(P, O, &G);
        }
    }
}

static inline bool e3d_usable(void) {
    return gfx_obj.enabled && gfxPs2Emotion3DEnabled() && !g_ps2RendererTraceCapturing;
}

static void e3d_room_leaf(uint32_t room, Gfx* gdl, uint32_t depth) {
    ++e3d.n_leaves;
    if (!gdl) {
        return;
    }
    if (!e3d_usable() || !segmentPointers[E3D_SEG_ROOM_VTX] || !segmentPointers[E3D_SEG_ROOM_COL]) {
        ++e3d.n_fallback;
        gfx_run_dl(gdl, depth + 1u);
        return;
    }
    const uint16_t gen = e3d.room_gen[room % E3D_ROOM_GENS];
    struct E3dProg* P = e3d_find(gdl, room, E3D_KIND_ROOM);
    if (!P || P->gen != gen) {
        P = e3d_compile(gdl, room, gen, E3D_KIND_ROOM, 0, 0);
    }
    if (!P->ok) {
        ++e3d.n_fallback;
        gfx_run_dl(gdl, depth + 1u);
        return;
    }
    e3d_run(P, depth);
}

/*
 * Skinned and lit lists cannot use the object batches: every vertex needs
 * the EE transform (or lighting) in list order anyway, and the plain list
 * with per-G_VTX deferral does that with less overhead than a replay that
 * classifies chunks it then cannot draw fast. The same holds for a list
 * whose last replay drew nothing through the batches (a render state the
 * object path does not handle, or straddling the frustum): it runs plain
 * for a few frames before Emotion3D tries it again.
 */
static void e3d_model_run(struct E3dProg* P, Gfx* gdl, uint32_t depth) {
    if (gfxPs2Emotion3DSkinEnabled() && !P->open) {
        e3d_skin_run(P, depth);
        return;
    }
    if (!gfxPs2Emotion3DModelsEnabled()) {
        gfx_run_dl(gdl, depth + 1u);
        return;
    }
    if (P->skinned || (rsp.geometry_mode & G_LIGHTING) != 0 || (int32_t)(P->plain_until - num_dls) > 0) {
        ++e3d.n_skinned;
        gfx_run_dl(gdl, depth + 1u);
        return;
    }
    const uint32_t fast0 = e3d.n_tris, slow0 = e3d.n_slow_tris;
    e3d_run(P, depth);
    if (e3d.n_tris == fast0 && e3d.n_slow_tris != slow0) {
        P->plain_until = num_dls + 16u;
    }
}

static void e3d_model_dl_inner(Gfx* gdl, uint32_t depth) {
    ++e3d.n_models;
    if (!gdl) {
        return;
    }
    const bool skin_on = gfxPs2Emotion3DSkinEnabled();
    if (!e3d_usable() || !(gfxPs2Emotion3DModelsEnabled() || skin_on) || !segmentPointers[E3D_SEG_MODEL_VTX] || !segmentPointers[E3D_SEG_MODEL_COL1] ||
        !segmentPointers[E3D_SEG_MODEL_COL2]) {
        ++e3d.n_fallback;
        gfx_run_dl(gdl, depth + 1u);
        return;
    }
    struct E3dProg* P = e3d_find(gdl, 0xffffu, E3D_KIND_MODEL);
    if (P && P->checked_frame == num_dls && !P->is_volatile) {
        /* checked earlier this frame: lists change between frames, not
         * between the instances drawn in one */
        if (!P->ok) {
            ++e3d.n_fallback;
            gfx_run_dl(gdl, depth + 1u);
        } else {
            e3d_model_run(P, gdl, depth);
        }
        return;
    }
    uint32_t sum = 0;
    const uint32_t ncmds = e3d_list_checksum(gdl, &sum);
    if (P && (P->ncmds != ncmds || P->checksum != sum)) {
        /* the list at this address changed (rebuilt, or memory reused) */
        if (P->is_volatile || ++P->mismatches >= E3D_VOLATILE_AFTER) {
            if (!P->is_volatile) {
                ++e3d.n_volatile;
            }
            P->is_volatile = 1;
            ++e3d.n_fallback;
            gfx_run_dl(gdl, depth + 1u);
            return;
        }
        const uint8_t mismatches = P->mismatches;
        P->gdl = nullptr; /* retire it; the new program takes the key */
        P = e3d_compile(gdl, 0xffffu, 0, E3D_KIND_MODEL, ncmds, sum);
        P->mismatches = mismatches;
    } else if (!P && ncmds != 0) {
        const uint32_t h = ((uint32_t)(uintptr_t)gdl * 2654435761u) >> 24;
        if (e3d.seen[h].gdl == gdl && e3d.seen[h].checksum == sum) {
            P = e3d_compile(gdl, 0xffffu, 0, E3D_KIND_MODEL, ncmds, sum);
        } else {
            e3d.seen[h].gdl = gdl;
            e3d.seen[h].checksum = sum;
        }
    }
    if (P && !P->is_volatile) {
        P->checked_frame = num_dls;
    }
    if (!P || !P->ok || P->is_volatile) {
        ++e3d.n_fallback;
        gfx_run_dl(gdl, depth + 1u);
        return;
    }
    /*
     * Skinned and lit lists cannot use the object batches: every vertex
     * needs the EE transform (or lighting) in list order anyway, and the
     * plain list with per-G_VTX deferral does that with less overhead
     * than a replay that classifies chunks it then cannot draw fast.
     */
    e3d_model_run(P, gdl, depth);
}

static void e3d_model_dl(Gfx* gdl, uint32_t depth) {
    const uint32_t c0 = e3d_cycles();
    e3d_model_dl_inner(gdl, depth);
    e3d_sk.cyc_total += e3d_cycles() - c0;
}

#include "ps2_renderer_stats.h"
#include "gs_native_queue.h"
static void e3d_log_stats(void) {
    {
        struct Ps2GsNativeQueueStats q;
        ps2GsNativeQueueTakeStats(&q);
        sysLogPrintf(LOG_NOTE, "E3D gsq submits=%u spills=%u qw=%u maxchain=%u uploads=%u upKB=%u finishes=%u (per second)"
                     " waits us/frame submit=%u upload=%u present=%u",
                     q.submits, q.spills, q.qwords, q.max_chain_qw, q.uploads, q.upload_bytes >> 10, q.finishes,
                     q.submit_wait_us / 60, q.upload_wait_us / 60, q.finish_wait_us / 60);
        sysLogPrintf(LOG_NOTE, "E3D gsq ad=%u qw/%u pk vtx=%u qw/%u pk vu1=%u qw/%u batches (obj %u tex %u) ref=%u qw (per frame)",
                     q.gif_ad_qw / 60, q.gif_ad_packets / 60, q.gif_vtx_qw / 60, q.gif_vtx_packets / 60,
                     q.vu1_qw / 60, q.vu1_batches / 60, q.vu1_obj_qw / 60, q.vu1_tex_qw / 60, q.ref_qw / 60);
    }
    {
        static struct Ps2RendererStats prevr;
        struct Ps2RendererStats r;
        ps2RendererStatsGet(&r);
        sysLogPrintf(LOG_NOTE, "E3D vu1 batches=%u verts=%u path1=%u path3=%u (per frame)",
                     (unsigned)((r.vu1_transform_batches - prevr.vu1_transform_batches) / 60),
                     (unsigned)((r.vu1_transform_vertices - prevr.vu1_transform_vertices) / 60),
                     (unsigned)((r.path1_vertices - prevr.path1_vertices) / 60),
                     (unsigned)((r.path3_vertices - prevr.path3_vertices) / 60));
        prevr = r;
    }
    {
        static struct GfxRdpTmemLiveStats prev;
        struct GfxRdpTmemLiveStats now;
        gfxRdpTmemLiveGetStats(&now);
        sysLogPrintf(LOG_NOTE, "E3D tlut deferred=%u flushed=%u elided=%u source_ids=%u fingerprints=%u (per frame)",
                     (now.load_tlut_deferred - prev.load_tlut_deferred) / 60,
                     (now.load_tlut_flushed - prev.load_tlut_flushed) / 60,
                     (now.load_tlut_elided - prev.load_tlut_elided) / 60,
                     (now.fingerprint_tlut_source - prev.fingerprint_tlut_source) / 60,
                     (now.fingerprint_exact - prev.fingerprint_exact) / 60);
        prev = now;
    }
    sysLogPrintf(LOG_NOTE,
                 "E3D skin ops=%u hidden=%u tris=%u skipped=%u batches=%u fail texgen=%u cullboth=%u lights=%u epochs=%u state=%u clip=%u (per frame)",
                 e3d_sk.n_ops / 60, e3d_sk.n_hidden / 60, e3d_sk.n_tris / 60, e3d_sk.n_skipped / 60, e3d_sk.n_batches / 60,
                 e3d_sk.n_fail[1] / 60, e3d_sk.n_fail[2] / 60, e3d_sk.n_fail[3] / 60, e3d_sk.n_fail[4] / 60,
                 e3d_sk.n_fail[5] / 60, e3d_sk.n_fail[6] / 60);
    sysLogPrintf(LOG_NOTE, "E3D skin state: at=%u inactive=%u prog=%u lod=%u twopass=%u other=%u (per frame)",
                 e3d_sk.n_state[0] / 60, e3d_sk.n_state[1] / 60, e3d_sk.n_state[2] / 60, e3d_sk.n_state[3] / 60,
                 e3d_sk.n_state[4] / 60, e3d_sk.n_state[5] / 60);
    memset(e3d_sk.n_state, 0, sizeof(e3d_sk.n_state));
    sysLogPrintf(LOG_NOTE, "E3D skin inactive: ok=%u zprim=%u gray=%u clamp=%u inputs=%u query=%u (per frame)",
                 e3d_sk.n_dwhy[0] / 60, e3d_sk.n_dwhy[1] / 60, e3d_sk.n_dwhy[2] / 60, e3d_sk.n_dwhy[3] / 60,
                 e3d_sk.n_dwhy[4] / 60, e3d_sk.n_dwhy[5] / 60);
    memset(e3d_sk.n_dwhy, 0, sizeof(e3d_sk.n_dwhy));
    sysLogPrintf(LOG_NOTE, "E3D skin us/frame: models=%u", e3d_sk.cyc_total / 295u / 60u);
    e3d_sk.cyc_total = 0;
    sysLogPrintf(LOG_NOTE, "E3D skin pass1 us/frame=%u mtx=%u snap=%u epochs=%u loads=%u",
                 e3d_sk.cyc_pass1 / 295u / 60u, e3d_sk.cyc_mtx / 295u / 60u, e3d_sk.cyc_snap / 295u / 60u,
                 e3d_sk.n_epochs / 60, e3d_sk.n_loads / 60);
    sysLogPrintf(LOG_NOTE, "E3D skin pass1 bounds=%u box=%u verts=%u (cycles per load)",
                 e3d_sk.n_loads ? e3d_sk.cyc_bounds / e3d_sk.n_loads : 0, e3d_sk.n_loads ? e3d_sk.cyc_box / e3d_sk.n_loads : 0,
                 e3d_sk.n_loads ? e3d_sk.n_lverts / e3d_sk.n_loads : 0);
    e3d_sk.cyc_bounds = e3d_sk.cyc_box = e3d_sk.n_lverts = 0;
    e3d_sk.cyc_pass1 = e3d_sk.cyc_mtx = e3d_sk.cyc_snap = e3d_sk.n_epochs = e3d_sk.n_loads = 0;
    sysLogPrintf(LOG_NOTE, "E3D skin us/frame: run=%u skin=%u linear=%u failcheck=%u",
                 e3d_sk.cyc_run / 295u / 60u,
                 e3d_sk.cyc_skin / 295u / 60u, e3d_sk.cyc_linear / 295u / 60u, e3d_sk.cyc_failed / 295u / 60u);
    e3d_sk.cyc_run = e3d_sk.cyc_skin = e3d_sk.cyc_linear = e3d_sk.cyc_failed = 0;
    e3d_sk.n_ops = e3d_sk.n_hidden = e3d_sk.n_tris = e3d_sk.n_skipped = e3d_sk.n_batches = 0;
    memset(e3d_sk.n_fail, 0, sizeof(e3d_sk.n_fail));
    sysLogPrintf(LOG_NOTE,
                 "E3D leaves=%u models=%u plain=%u fallback=%u compiles=%u resets=%u volatile=%u chunks inside=%u "
                 "outside=%u mixed=%u unfit=%u tris fast=%u slow=%u linear=%u arena=%u KiB (per frame)",
                 e3d.n_leaves / 60, e3d.n_models / 60, e3d.n_skinned / 60, e3d.n_fallback / 60, e3d.n_compiles, e3d.n_resets,
                 e3d.n_volatile, e3d.n_inside / 60, e3d.n_outside / 60, e3d.n_mixed / 60, e3d.n_unfit / 60,
                 e3d.n_tris / 60, e3d.n_slow_tris / 60, e3d.n_linear_tris / 60, e3d.used >> 10);
    e3d.n_leaves = e3d.n_models = e3d.n_skinned = e3d.n_fallback = e3d.n_compiles = e3d.n_resets = e3d.n_volatile = 0;
    e3d.n_inside = e3d.n_outside = e3d.n_mixed = e3d.n_unfit = e3d.n_tris = e3d.n_slow_tris = 0;
    e3d.n_linear_tris = 0;
}
