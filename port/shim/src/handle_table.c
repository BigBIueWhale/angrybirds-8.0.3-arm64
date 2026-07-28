/* handle_table.c — JNI 32<->64 token table. See handle_table.h.
 * Per-kind slot pools with index reuse + an open-addressing dedup map for IDs
 * + a local-ref frame stack. Pure host bookkeeping; no real JNI calls. */
#include "handle_table.h"
#include <stdlib.h>
#include <string.h>

typedef struct { void *real; const void *desc; int used; } slot;
typedef struct {
    slot     *v; uint32_t cap, n;      /* v[0..n) are live-range slots */
    uint32_t *fl, fl_len, fl_cap;      /* free-list of reusable indices */
} pool;
typedef struct { void *key; uint32_t tok; } hent;

struct handle_table {
    pool      p[5];                    /* indexed by kind; 1..4 used */
    uint32_t *frame; uint32_t frame_len, frame_cap;   /* local-ref frame stack */
    hent     *idmap; uint32_t idmap_cap, idmap_len;   /* real_id -> id token */
};

/* POOL_FULL: no slot could be allocated. Callers map it to HT_NULL, which every consumer of this
 * table already handles (it is what a NULL object yields). Previously the realloc result was
 * assigned straight into p->v and then indexed on the next line, so an allocation failure meant a
 * NULL dereference AND the loss of the old buffer. This is the JNI token table - it grows with live
 * refs, so on a memory-pressured phone the failure is reachable rather than theoretical. */
#define POOL_FULL 0xffffffffu
static uint32_t pool_alloc(pool *p, void *real, const void *desc){
    uint32_t idx;
    if (p->fl_len) idx = p->fl[--p->fl_len];
    else {
        if (p->n >= p->cap){
            uint32_t nc = p->cap ? p->cap*2 : 16;
            slot *nv = (slot*)realloc(p->v, (size_t)nc*sizeof(slot));
            if (!nv) return POOL_FULL;          /* old p->v and p->cap stay valid */
            p->v = nv; p->cap = nc;
        }
        idx = p->n++;
    }
    p->v[idx].real = real; p->v[idx].desc = desc; p->v[idx].used = 1;
    return idx;
}
static void pool_free(pool *p, uint32_t idx){
    if (idx >= p->n || !p->v[idx].used) return;
    p->v[idx].used = 0; p->v[idx].real = NULL; p->v[idx].desc = NULL;
    /* If the free-list cannot grow, drop this index rather than dereference NULL. The slot is
     * already marked unused, so the only cost is that it is not reused - a leak of one table entry,
     * not a crash, and not a correctness error. */
    if (p->fl_len >= p->fl_cap){
        uint32_t nc = p->fl_cap ? p->fl_cap*2 : 16;
        uint32_t *nf = (uint32_t*)realloc(p->fl, (size_t)nc*sizeof(uint32_t));
        if (!nf) return;
        p->fl = nf; p->fl_cap = nc;
    }
    p->fl[p->fl_len++] = idx;
}

/* ---- ID dedup map (open addressing on the real pointer) ---- */
static uint32_t ptr_hash(void *p){
    uint64_t x = (uint64_t)(uintptr_t)p;
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
    return (uint32_t)x;
}
static uint32_t idmap_get(handle_table *t, void *key){
    if (!t->idmap_cap) return 0;
    uint32_t mask = t->idmap_cap - 1, i = ptr_hash(key) & mask;
    for (uint32_t probe = 0; probe < t->idmap_cap; probe++){
        hent *e = &t->idmap[i];
        if (!e->key) return 0;
        if (e->key == key) return e->tok;
        i = (i + 1) & mask;
    }
    return 0;
}
static void idmap_put_raw(handle_table *t, void *key, uint32_t tok){
    uint32_t mask = t->idmap_cap - 1, i = ptr_hash(key) & mask;
    for (;;){ hent *e = &t->idmap[i]; if (!e->key){ e->key = key; e->tok = tok; t->idmap_len++; return; } i = (i + 1) & mask; }
}
static void idmap_grow(handle_table *t){
    uint32_t oldcap = t->idmap_cap; hent *old = t->idmap;
    uint32_t newcap = oldcap ? oldcap*2 : 64;
    hent *nm = (hent*)calloc(newcap, sizeof(hent));
    /* Unchecked before: t->idmap was assigned straight from calloc, then idmap_put_raw wrote into
     * it on the next line and the OLD table was freed regardless - so an OOM here both dereferenced
     * NULL and destroyed the only copy of the map. Keep the old table intact and simply do not
     * grow: the map still works, just more densely, which is the correct degradation. */
    if (!nm) return;
    t->idmap = nm; t->idmap_cap = newcap; t->idmap_len = 0;
    for (uint32_t j = 0; j < oldcap; j++) if (old[j].key) idmap_put_raw(t, old[j].key, old[j].tok);
    free(old);
}

handle_table *ht_create(void){ return (handle_table*)calloc(1, sizeof(handle_table)); }
void ht_destroy(handle_table *t){
    if (!t) return;
    for (int k = 1; k <= 4; k++){ free(t->p[k].v); free(t->p[k].fl); }
    free(t->frame); free(t->idmap); free(t);
}

uint32_t ht_new_ref(handle_table *t, int kind, void *real){
    if (!real) return HT_NULL;
    if (kind < HK_LOCAL || kind > HK_WEAK) return HT_NULL;
    uint32_t idx = pool_alloc(&t->p[kind], real, NULL);
    if (idx == POOL_FULL) return HT_NULL;      /* out of memory -> behave as a null ref */
    uint32_t tok = ((uint32_t)kind << 28) | idx;
    if (kind == HK_LOCAL){
        /* The local-ref frame records tokens so they can be released together. If it cannot grow,
         * the ref stays valid but will not be auto-released on frame pop - a bounded leak, chosen
         * over dereferencing a NULL frame array. */
        if (t->frame_len >= t->frame_cap){
            uint32_t nc = t->frame_cap ? t->frame_cap*2 : 32;
            uint32_t *nf = (uint32_t*)realloc(t->frame, (size_t)nc*sizeof(uint32_t));
            if (nf){ t->frame = nf; t->frame_cap = nc; }
        }
        if (t->frame_len < t->frame_cap) t->frame[t->frame_len++] = tok;
    }
    return tok;
}

void *ht_resolve(handle_table *t, uint32_t tok){
    if (tok == HT_NULL) return NULL;
    uint32_t k = ht_kind(tok), idx = ht_index(tok);
    if (k < HK_LOCAL || k > HK_ID) return NULL;
    pool *p = &t->p[k];
    if (idx >= p->n || !p->v[idx].used) return NULL;
    return p->v[idx].real;
}

int ht_delete_ref(handle_table *t, uint32_t tok){
    if (tok == HT_NULL) return 0;
    uint32_t k = ht_kind(tok), idx = ht_index(tok);
    if (k < HK_LOCAL || k > HK_WEAK) return 0;     /* IDs are never deleted */
    pool *p = &t->p[k];
    if (idx >= p->n || !p->v[idx].used) return 0;
    pool_free(p, idx);
    return 1;
}

uint32_t ht_intern_id(handle_table *t, void *real_id, const void *desc){
    if (!real_id) return HT_NULL;
    uint32_t existing = idmap_get(t, real_id);
    if (existing) return existing;
    uint32_t idx = pool_alloc(&t->p[HK_ID], real_id, desc);
    if (idx == POOL_FULL) return HT_NULL;
    uint32_t tok = ((uint32_t)HK_ID << 28) | idx;
    if (t->idmap_cap == 0 || t->idmap_len*4 >= t->idmap_cap*3) idmap_grow(t);
    idmap_put_raw(t, real_id, tok);
    return tok;
}

const void *ht_id_desc(handle_table *t, uint32_t tok){
    if (ht_kind(tok) != HK_ID) return NULL;
    uint32_t idx = ht_index(tok); pool *p = &t->p[HK_ID];
    if (idx >= p->n || !p->v[idx].used) return NULL;
    return p->v[idx].desc;
}

uint32_t ht_frame_mark(handle_table *t){ return t->frame_len; }
void ht_frame_pop(handle_table *t, uint32_t mark){
    while (t->frame_len > mark){
        uint32_t tok = t->frame[--t->frame_len];
        uint32_t idx = ht_index(tok); pool *p = &t->p[HK_LOCAL];
        if (idx < p->n && p->v[idx].used) pool_free(p, idx);
    }
}

uint32_t ht_live(handle_table *t, int kind){
    if (kind < 1 || kind > 4) return 0;
    pool *p = &t->p[kind]; uint32_t c = 0;
    for (uint32_t i = 0; i < p->n; i++) if (p->v[i].used) c++;
    return c;
}
