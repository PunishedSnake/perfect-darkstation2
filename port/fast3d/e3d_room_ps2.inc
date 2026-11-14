/*
 * Emotion3D, stage 1: room geometry (docs/ps2-emotion3d.md).
 *
 * Included into the PS2 Fast3D frontend (gfx_pc.cpp, GFX_PS2_DIRECT only):
 * it drives the frontend's own object-batch machinery directly instead of
 * going through G_VTX/G_TRI decoding.
 *
 * Perfect Dark draws level geometry as rooms. A room is a tree of blocks;
 * each leaf block is a display list over the room's own vertex buffer (BG
 * vertex segment) and colour table (BG colour segment). The display lists
 * and vertex positions never change while the room is loaded; only the
 * colours do (room lighting). Fast3D still decoded every G_VTX and G_TRI4
 * of them every frame.
 *
 * bgRenderRoomPass emits G_E3D_ROOMLEAF_EXT(room, gdl) for each leaf. The
 * first time a leaf is seen it is compiled into ops:
 *   - STATE: runs of other commands (texture loads, combiner, render
 *     modes), copied into a compact display list Fast3D still interprets;
 *   - GEOM:  runs of G_COL/G_VTX/G_TRI1/G_TRI4, split into chunks: the
 *     triangles between two vertex loads, each with its bounding box, the
 *     vertex loads it depends on, and per corner the vertex index, colour
 *     index and original vertex slot.
 * On replay, each chunk is classified once against the frustum planes on
 * VU0:
 *   - inside the guard band: culled and packed straight into the VU1 object
 *     batch, exactly as gfx_obj_tri packs deferred vertices;
 *   - outside one exact plane: skipped (Fast3D rejects every triangle);
 *   - straddling, or a state the object path cannot draw: its vertex loads
 *     and triangles go to gfx_sp_vertex/gfx_sp_tri1 directly, which is what
 *     the display list would have done minus the command decoding.
 * A leaf that cannot be compiled (nested lists, unexpected segments, size
 * limits) runs its original display list.
 *
 * Programs are keyed by (display list, room, room generation). bg.c bumps
 * a room's generation when the room is loaded or unloaded, which are the
 * only times its lists are written.
 */

extern "C" bool gfxPs2Emotion3DEnabled(void);
extern "C" void gfxPs2E3dRoomChanged(int32_t room);
static void gfx_run_dl(Gfx* cmd, uint32_t ps2_trace_depth);
static inline void gfx_sp_tri1(uint8_t vtx1_idx, uint8_t vtx2_idx, uint8_t vtx3_idx, bool is_rect);

enum {
    E3D_OP_STATE = 0,
    E3D_OP_GEOM = 1,
    E3D_SEG_COL = 13, /* SPSEGMENT_BG_COL */
    E3D_SEG_VTX = 14, /* SPSEGMENT_BG_VTX */
    E3D_ROOM_GENS = 1024,
    E3D_BUCKETS = 512,
    E3D_ARENA_BYTES = 256 * 1024,
    E3D_MAX_CMDS = 2048,
    E3D_MAX_TRIS = 2048,
    E3D_MAX_OPS = 192,
    E3D_MAX_CHUNKS = 512,
    E3D_MAX_LOADS = 512,
    E3D_MAX_CHUNK_LOADS = 2048,
    E3D_SLOTS = 32,
};

struct E3dTri {
    uint16_t vtx[3];  /* index into the BG vertex segment */
    uint16_t col[3];  /* index into the BG colour segment */
    uint8_t slot[3];  /* the vertex slots the display list used */
    uint8_t pad;
};

struct E3dLoad {      /* one G_VTX */
    uint16_t first;   /* vertex index */
    uint8_t n;
    uint8_t dest;
    uint16_t colbase; /* colour table (index) current at the load */
    uint16_t pad;
};

struct E3dChunk {
    uint16_t tri0;
    uint16_t ntris;
    uint16_t ld0;     /* into the program's chunk-load list */
    uint16_t nld;
    float lo[3];
    float hi[3];
};

struct E3dOp {
    uint8_t kind;
    uint8_t pad;
    uint16_t nchunks;
    uint16_t chunk0;
    uint16_t pad2;
    Gfx* dl;          /* STATE only */
};

struct E3dProg {
    const Gfx* gdl;
    struct E3dProg* next;
    uint16_t room;
    uint16_t gen;
    uint16_t nops;
    uint8_t ok;
    uint8_t pad;
    struct E3dOp* ops;
    struct E3dChunk* chunks;
    struct E3dTri* tris;
    struct E3dLoad* loads;
    uint16_t* chunk_loads;
};

static struct {
    uint8_t arena[E3D_ARENA_BYTES] __attribute__((aligned(16)));
    uint32_t used;
    struct E3dProg* buckets[E3D_BUCKETS];
    uint16_t room_gen[E3D_ROOM_GENS];
    /* counters, logged per frame with PD_PS2_OBJECT_STATS */
    uint32_t n_leaves, n_fallback_leaves, n_compiles, n_resets;
    uint32_t n_inside, n_outside, n_mixed, n_unfit, n_tris, n_slow_tris;
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

/* Full arena: forget every program; leaves recompile on their next use. */
static void e3d_reset(void) {
    e3d.used = 0;
    memset(e3d.buckets, 0, sizeof(e3d.buckets));
    ++e3d.n_resets;
}

static inline uint32_t e3d_bucket(const Gfx* gdl, uint32_t room) {
    const uint32_t h = (uint32_t)(uintptr_t)gdl * 2654435761u ^ room * 40503u;
    return (h >> 9) % E3D_BUCKETS;
}

static inline bool e3d_seg_is(uintptr_t w1, uint32_t seg) {
    return (w1 & 1u) != 0 && ((w1 >> 24) & 0x0fu) == seg;
}

/* Compile scratch, reused for every leaf. */
static struct {
    struct E3dOp ops[E3D_MAX_OPS];
    struct E3dChunk chunks[E3D_MAX_CHUNKS];
    struct E3dTri tris[E3D_MAX_TRIS];
    struct E3dLoad loads[E3D_MAX_LOADS];
    uint16_t chunk_loads[E3D_MAX_CHUNK_LOADS];
    uint16_t op_first[E3D_MAX_OPS];
    uint16_t op_count[E3D_MAX_OPS];
} e3d_tmp;

/* Builder state for one compile. */
struct E3dBuild {
    int16_t slot_load[E3D_SLOTS]; /* load index that filled each slot */
    uint32_t chunk_slots;         /* slots the open chunk's triangles read */
    uint32_t nops, nchunks, ntris, nloads, nchunk_loads;
    int cur_op;
    int cur_chunk;
};

static bool e3d_close_chunk(struct E3dBuild* B) {
    if (B->cur_chunk < 0) {
        return true;
    }
    struct E3dChunk* K = &e3d_tmp.chunks[B->cur_chunk];
    /* its loads, in program order (each the latest for the slots read) */
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
        prev = next;
    }
    K->nld = (uint16_t)(B->nchunk_loads - K->ld0);
    B->cur_chunk = -1;
    B->chunk_slots = 0;
    return true;
}

static struct E3dProg* e3d_compile(const Gfx* gdl, uint32_t room, uint16_t gen) {
    const Vtx* vbase = (const Vtx*)segmentPointers[E3D_SEG_VTX];
    struct E3dProg* P = (struct E3dProg*)e3d_alloc(sizeof(struct E3dProg));
    if (!P) {
        e3d_reset();
        P = (struct E3dProg*)e3d_alloc(sizeof(struct E3dProg));
    }
    memset(P, 0, sizeof(*P));
    P->gdl = gdl;
    P->room = (uint16_t)room;
    P->gen = gen;
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
    uint32_t colbase = 0;
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
        if (op == (uint8_t)G_DL || op == (uint8_t)G_E3D_ROOMLEAF_EXT) {
            return P; /* nested lists: keep the original */
        }
        const bool geom = op == (uint8_t)G_VTX || op == (uint8_t)G_COL || op == (uint8_t)G_TRI1 ||
                          op == (uint8_t)G_TRI4;
        const uint8_t kind = geom ? E3D_OP_GEOM : E3D_OP_STATE;
        if (B.cur_op < 0 || e3d_tmp.ops[B.cur_op].kind != kind) {
            if (!e3d_close_chunk(&B) || B.nops >= E3D_MAX_OPS) {
                return P;
            }
            chunk_has_tris = false;
            B.cur_op = (int)B.nops++;
            struct E3dOp* O = &e3d_tmp.ops[B.cur_op];
            memset(O, 0, sizeof(*O));
            O->kind = kind;
            O->chunk0 = (uint16_t)B.nchunks;
            e3d_tmp.op_first[B.cur_op] = (uint16_t)ncmds;
            e3d_tmp.op_count[B.cur_op] = 0;
        }
        ++e3d_tmp.op_count[B.cur_op];
        if (!geom) {
            continue;
        }

        const uintptr_t w0 = c->words.w0, w1 = c->words.w1;
        if (op == (uint8_t)G_COL) {
            if (!e3d_seg_is(w1, E3D_SEG_COL) || (w1 & 2u) != 0) {
                return P;
            }
            colbase = (uint32_t)((w1 & 0x00fffffeu) >> 2);
            col_valid = colbase <= 0xffffu;
            continue;
        }
        if (op == (uint8_t)G_VTX) {
            const uint32_t n = (uint32_t)(w0 & 0xffffu) / sizeof(Vtx);
            const uint32_t dest = (uint32_t)(w0 >> 16) & 0x0fu;
            const uint32_t ofs = (uint32_t)(w1 & 0x00fffffeu);
            if (!e3d_seg_is(w1, E3D_SEG_VTX) || ofs % sizeof(Vtx) != 0 || n == 0 || dest + n > E3D_SLOTS ||
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
            L->first = (uint16_t)(ofs / sizeof(Vtx));
            L->n = (uint8_t)n;
            L->dest = (uint8_t)dest;
            L->colbase = (uint16_t)colbase;
            L->pad = 0;
            for (uint32_t i = 0; i < n; ++i) {
                B.slot_load[dest + i] = (int16_t)B.nloads;
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
                for (int a = 0; a < 3; ++a) {
                    K->lo[a] = 32767.0f;
                    K->hi[a] = -32768.0f;
                }
                ++e3d_tmp.ops[B.cur_op].nchunks;
            }
            struct E3dChunk* K = &e3d_tmp.chunks[B.cur_chunk];
            struct E3dTri* T = &e3d_tmp.tris[B.ntris++];
            T->pad = 0;
            for (int k = 0; k < 3; ++k) {
                const uint8_t s = idx[t][k];
                if (s >= E3D_SLOTS || B.slot_load[s] < 0) {
                    return P;
                }
                const struct E3dLoad* L = &e3d_tmp.loads[B.slot_load[s]];
                const uint32_t vi = L->first + (s - L->dest);
                const Vtx* v = &vbase[vi];
                const uint32_t ci = L->colbase + (v->colour >> 2);
                if (ci > 0xffffu) {
                    return P;
                }
                T->vtx[k] = (uint16_t)vi;
                T->col[k] = (uint16_t)ci;
                T->slot[k] = s;
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

    /* Commit into the arena. */
    struct E3dOp* ops = (struct E3dOp*)e3d_alloc(B.nops * sizeof(struct E3dOp) + 16u);
    struct E3dChunk* chunks = (struct E3dChunk*)e3d_alloc(B.nchunks * sizeof(struct E3dChunk) + 16u);
    struct E3dTri* tris = (struct E3dTri*)e3d_alloc(B.ntris * sizeof(struct E3dTri) + 16u);
    struct E3dLoad* loads = (struct E3dLoad*)e3d_alloc(B.nloads * sizeof(struct E3dLoad) + 16u);
    uint16_t* chunk_loads = (uint16_t*)e3d_alloc(B.nchunk_loads * sizeof(uint16_t) + 16u);
    if (!ops || !chunks || !tris || !loads || !chunk_loads) {
        return P; /* arena full: plain list until the next reset */
    }
    memcpy(ops, e3d_tmp.ops, B.nops * sizeof(struct E3dOp));
    memcpy(chunks, e3d_tmp.chunks, B.nchunks * sizeof(struct E3dChunk));
    memcpy(tris, e3d_tmp.tris, B.ntris * sizeof(struct E3dTri));
    memcpy(loads, e3d_tmp.loads, B.nloads * sizeof(struct E3dLoad));
    memcpy(chunk_loads, e3d_tmp.chunk_loads, B.nchunk_loads * sizeof(uint16_t));
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
    P->chunks = chunks;
    P->tris = tris;
    P->loads = loads;
    P->chunk_loads = chunk_loads;
    P->nops = (uint16_t)B.nops;
    P->ok = 1;
    return P;
}

static struct E3dProg* e3d_lookup(const Gfx* gdl, uint32_t room) {
    const uint16_t gen = e3d.room_gen[room % E3D_ROOM_GENS];
    for (struct E3dProg* P = e3d.buckets[e3d_bucket(gdl, room)]; P; P = P->next) {
        if (P->gdl == gdl && P->room == room && P->gen == gen) {
            return P;
        }
    }
    return e3d_compile(gdl, room, gen);
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

/* What the display list would have done for this chunk, minus decoding. */
static void e3d_chunk_slow(const struct E3dProg* P, const struct E3dChunk* K, const Vtx* vbase,
                           const struct NormalColor* cbase) {
    const struct NormalColor* saved = rsp.vertex_colors;
    for (uint32_t i = 0; i < K->nld; ++i) {
        const struct E3dLoad* L = &P->loads[P->chunk_loads[K->ld0 + i]];
        rsp.vertex_colors = cbase + L->colbase;
        gfx_sp_vertex(L->n, L->dest, vbase + L->first);
    }
    rsp.vertex_colors = saved;
    for (uint32_t t = 0; t < K->ntris; ++t) {
        const struct E3dTri* T = &P->tris[K->tri0 + t];
        gfx_sp_tri1(T->slot[0], T->slot[1], T->slot[2], false);
    }
    e3d.n_slow_tris += K->ntris;
}

static void e3d_chunk_fast(const struct E3dProg* P, const struct E3dChunk* K, int k, const struct GfxObjCtx* X,
                           uint32_t cull_mode, const Vtx* vbase, const struct NormalColor* cbase) {
    const int32_t ts = (int32_t)X->tex_s, tt = (int32_t)X->tex_t;
    for (uint32_t t = 0; t < K->ntris; ++t) {
        const struct E3dTri* T = &P->tris[K->tri0 + t];
        const Vtx* vs[3] = { &vbase[T->vtx[0]], &vbase[T->vtx[1]], &vbase[T->vtx[2]] };
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
            const struct NormalColor col = cbase[T->col[i]];
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
 * visible triangle needs it, so classification comes first: a GEOM op whose
 * chunks are all off screen must not cost a setup.
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

static void e3d_geom(const struct E3dProg* P, const struct E3dOp* O) {
    const Vtx* vbase = (const Vtx*)segmentPointers[E3D_SEG_VTX];
    const struct NormalColor* cbase = (const struct NormalColor*)segmentPointers[E3D_SEG_COL];
    const uint32_t cull_mode = rsp.geometry_mode & G_CULL_BOTH;
    const int k = e3d_obj_ctx();
    const struct GfxObjCtx* X = &gfx_obj.ctxs[k];
    const bool lit = (rsp.geometry_mode & G_LIGHTING) != 0;
    int fast = -1; /* decided at the first visible chunk */

    for (uint32_t c = 0; c < O->nchunks; ++c) {
        const struct E3dChunk* K = &P->chunks[O->chunk0 + c];
        uint32_t mask = 0;
        const enum Ps2ObjectBoxClass cls = gfx_obj_classify(X, K->lo, K->hi, &mask);
        if (cls == PS2_OBJECT_BOX_OUTSIDE) {
            ++e3d.n_outside; /* Fast3D rejects every triangle of it */
            continue;
        }
        if (fast < 0) {
            fast = !lit && e3d_fast_ok(X) ? 1 : 0;
        }
        if (!fast) {
            ++e3d.n_unfit;
            e3d_chunk_slow(P, K, vbase, cbase);
            continue;
        }
        if (cls == PS2_OBJECT_BOX_MIXED) {
            ++e3d.n_mixed;
            e3d_chunk_slow(P, K, vbase, cbase);
            continue;
        }
        ++e3d.n_inside;
#if !defined(PERFECT_DARK_PS2_DISABLE_CULLING)
        if (cull_mode == G_CULL_BOTH) {
            continue;
        }
#endif
        if (buf_vbo_len > 0) {
            gfx_flush();
        }
        e3d_chunk_fast(P, K, k, X, cull_mode, vbase, cbase);
    }
}

static void e3d_room_leaf(uint32_t room, Gfx* gdl, uint32_t depth) {
    ++e3d.n_leaves;
    if (!gdl) {
        return;
    }
    if (!gfx_obj.enabled || !gfxPs2Emotion3DEnabled() || g_ps2RendererTraceCapturing ||
        !segmentPointers[E3D_SEG_VTX] || !segmentPointers[E3D_SEG_COL]) {
        ++e3d.n_fallback_leaves;
        gfx_run_dl(gdl, depth + 1u);
        return;
    }
    const struct E3dProg* P = e3d_lookup(gdl, room);
    if (!P->ok) {
        ++e3d.n_fallback_leaves;
        gfx_run_dl(gdl, depth + 1u);
        return;
    }
    for (uint32_t o = 0; o < P->nops; ++o) {
        const struct E3dOp* O = &P->ops[o];
        if (O->kind == E3D_OP_STATE) {
            gfx_run_dl(O->dl, depth + 1u);
        } else {
            e3d_geom(P, O);
        }
    }
}

static void e3d_log_stats(void) {
    sysLogPrintf(LOG_NOTE,
                 "E3D leaves=%u fallback=%u compiles=%u resets=%u chunks inside=%u outside=%u mixed=%u unfit=%u "
                 "tris fast=%u slow=%u arena=%u KiB (per frame)",
                 e3d.n_leaves / 60, e3d.n_fallback_leaves / 60, e3d.n_compiles, e3d.n_resets, e3d.n_inside / 60,
                 e3d.n_outside / 60, e3d.n_mixed / 60, e3d.n_unfit / 60, e3d.n_tris / 60, e3d.n_slow_tris / 60,
                 e3d.used >> 10);
    e3d.n_leaves = e3d.n_fallback_leaves = e3d.n_compiles = e3d.n_resets = e3d.n_inside = e3d.n_outside =
        e3d.n_mixed = e3d.n_unfit = e3d.n_tris = e3d.n_slow_tris = 0;
}
