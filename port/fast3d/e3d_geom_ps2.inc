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

extern "C" bool gfxPs2Emotion3DEnabled(void);
extern "C" bool gfxPs2Emotion3DModelsEnabled(void);
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
    uint8_t pad;
    uint16_t nitems;
    uint16_t item0;
    uint16_t pad2;
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
    uint8_t pad;
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
            (struct Ps2GsVu1ObjectVertex*)&gfx_ps2_direct.batch[gfx_ps2_direct.batch_count];
        for (int i = 0; i < 3; ++i) {
            const Vtx* v = vs[i];
            const struct NormalColor col = G->cbase[(T->colseg >> i) & 1u][T->col[i] + (v->colour >> 2)];
            out[i].pos[0] = v->v[0];
            out[i].pos[1] = v->v[1];
            out[i].pos[2] = v->v[2];
            out[i].pos[3] = 0;
            out[i].uv[0] = (short)(v->s * ts >> 16);
            out[i].uv[1] = (short)(v->t * tt >> 16);
            out[i].uv[2] = 0;
            out[i].uv[3] = 0;
            out[i].rgba[0] = col.r;
            out[i].rgba[1] = col.g;
            out[i].rgba[2] = col.b;
            out[i].rgba[3] = col.a;
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

static void e3d_model_dl(Gfx* gdl, uint32_t depth) {
    ++e3d.n_models;
    if (!gdl) {
        return;
    }
    if (!e3d_usable() || !gfxPs2Emotion3DModelsEnabled() || !segmentPointers[E3D_SEG_MODEL_VTX] || !segmentPointers[E3D_SEG_MODEL_COL1] ||
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

#include "ps2_renderer_stats.h"
static void e3d_log_stats(void) {
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
                 "E3D leaves=%u models=%u plain=%u fallback=%u compiles=%u resets=%u volatile=%u chunks inside=%u "
                 "outside=%u mixed=%u unfit=%u tris fast=%u slow=%u linear=%u arena=%u KiB (per frame)",
                 e3d.n_leaves / 60, e3d.n_models / 60, e3d.n_skinned / 60, e3d.n_fallback / 60, e3d.n_compiles, e3d.n_resets,
                 e3d.n_volatile, e3d.n_inside / 60, e3d.n_outside / 60, e3d.n_mixed / 60, e3d.n_unfit / 60,
                 e3d.n_tris / 60, e3d.n_slow_tris / 60, e3d.n_linear_tris / 60, e3d.used >> 10);
    e3d.n_leaves = e3d.n_models = e3d.n_skinned = e3d.n_fallback = e3d.n_compiles = e3d.n_resets = e3d.n_volatile = 0;
    e3d.n_inside = e3d.n_outside = e3d.n_mixed = e3d.n_unfit = e3d.n_tris = e3d.n_slow_tris = 0;
    e3d.n_linear_tris = 0;
}
