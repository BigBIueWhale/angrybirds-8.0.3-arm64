/* galloc.c — the ONE guest-heap allocator (Audit 05).  See galloc.h.
 *
 * Boundary-tag coalescing allocator; all chunk metadata lives IN guest memory
 * (reached only through the mem-ops interface). Design is dlmalloc-lite:
 *
 *   chunk C:   [C+0] prev_foot  (u32) — size of the physically-previous chunk,
 *                                       valid only when PINUSE(C)==0
 *              [C+4] head       (u32) — this chunk's size | flags
 *              [C+8] payload = chunk2mem(C)   (16-byte aligned)
 *   flags in head:  PINUSE(bit0)=previous chunk in use ; CINUSE(bit1)=this in use
 *   a FREE chunk additionally stores { fd@C+8, bk@C+12 } free-list links and
 *   replicates its size in the NEXT chunk's prev_foot (the footer).
 *
 * Alignment: mem=C+8 is 16-aligned  <=>  C ≡ 8 (mod 16). All chunk sizes are
 * multiples of 16, so the invariant is preserved across every split/merge.
 * A left "fencepost" (PINUSE=1 on the first chunk) stops backward coalescing;
 * a right fencepost chunk (CINUSE=1, at hi-8) stops forward coalescing.
 * The wilderness "top" chunk always sits last, just below the right fencepost.
 *
 * Invariant enforced everywhere: no two physically-adjacent free chunks
 * (every free coalesces), and PINUSE(next) always mirrors CINUSE(cur). */
#include "galloc.h"
#include <stdlib.h>
#include <string.h>

#define ALIGN      16u
#define CHUNK_OVH  8u        /* prev_foot + head */
#define MIN_CHUNK  16u       /* header 8 + fd/bk 8 */
#define PINUSE     1u
#define CINUSE     2u
#define FLAGS      3u

#ifdef GALLOC_DEBUG
static int g_rpath;
#define RPATH(x) (g_rpath=(x))
#else
#define RPATH(x) ((void)0)
#endif

/* Use-after-free QUARANTINE. The engine's scene/registry build has a std::string COW _Rep that is freed
 * one refcount too early (a premature free under the shim's timing) while the scene tree still references
 * it; the classic dlmalloc intrusive free-list then writes fd/bk into the freed chunk's first 8 bytes =
 * the _Rep's [_M_length]/[_M_capacity] slots ("two heap pointers"), so a stale read sees a garbage length
 * -> cyclic std::_Rb_tree -> the nativeInit grind + registry loop + garbage strings. Deferring the REAL
 * free by QUARANTINE_N operations keeps the freed block CINUSE with its ORIGINAL bytes intact (no fd/bk
 * written, not reused) across the window in which the stale reference is read, so the UAF reads valid data
 * and the corruption never forms. 128MB arena => holding ~16K blocks (<1MB) is free.
 *   CRITICAL double-free interaction: the engine's refcount bug also produces DOUBLE-FREEs. Without the
 * quarantine, galloc's !CIN check catches a double-free at free-time (the block is already !CINUSE) and
 * no-ops it. WITH the quarantine a just-freed block stays CINUSE while held, so a second free of it is NOT
 * caught by !CIN -> it gets queued twice -> after the first eviction re-allocates it, the second eviction
 * really-frees an IN-USE block onto the free-list -> the new owner overwrites fd/bk -> a CYCLIC free-list ->
 * galloc_malloc's first-fit walk spins forever (observed: PC stuck in RG_STUB malloc). So we track the set
 * of currently-held ptrs in a host hash-set (qhash) and treat a repeat free of a held block as the
 * double-free it is (no-op) — restoring the pre-quarantine safety. */
#define QUARANTINE_N 131072u     /* held-block window. Bigger = longer UAF protection (fewer nondeterministic
                                  * stale-read failures) at the cost of held memory; the 256MB heap + the
                                  * QUARANTINE_MAX_SIZE cap (small _Reps only) keep the held high-water to ~25MB.
                                  * 65536 reached the main menu+level-load before a UAF fatal; widen to cover more. */
#define QHASH_SZ     262144u     /* 2*QUARANTINE_N, power of two: open-addressed set of held ptrs */
#define QUARANTINE_MAX_SIZE 0x100000u /* 1MB. Only skip the quarantine for HUGE blocks (audio-decode PCM etc., which
                                  * are multi-MB and never a std::string _Rep) so they don't inflate the held-memory
                                  * high-water and exhaust the heap. Must stay WELL ABOVE any _Rep: a 512-byte limit
                                  * wrongly excluded long-string _Reps and reintroduced the UAF (a stuck LDREX
                                  * refcount fault). In practice almost every held block is a small _Rep, so the cap
                                  * rarely triggers and held memory stays a few MB; the self-healing malloc backstops
                                  * any residual large-block free-list cycle. */
struct galloc {
    guest_mem *m;
    uint32_t lo, hi;         /* arena bounds [lo,hi) */
    uint32_t c0;             /* first chunk (= lo+8) */
    uint32_t rf;             /* right fencepost (= hi-8) */
    uint32_t top;            /* wilderness chunk */
    uint32_t topsize;
    uint32_t freelist;       /* head of the free list (0 = empty) */
    uint32_t qr[QUARANTINE_N];/* ring of deferred-free user ptrs (blocks stay CINUSE while held) */
    uint32_t qhash[QHASH_SZ];/* open-addressed set of held ptrs (0=empty; keys are 16-aligned >=RG_HEAP) -> O(1) double-free detection */
    uint32_t qr_can[QUARANTINE_N];/* WAF-DETECTOR: canary of each held block's first 16 bytes at free time */
    uint32_t qr_lr[QUARANTINE_N]; /* WAF-DETECTOR: engine-rel free-site (guest LR) for each held block */
    uint32_t qn;             /* active quarantine depth (0 = disabled; immediate free) */
    uint32_t qhead, qcount;  /* ring head index + occupancy */
};

/* --- host hash-set of quarantined user ptrs (linear probing; key 0 = empty) --- */
static uint32_t qh_hash(uint32_t p){ return (p >> 4) * 2654435761u; }
static int  qh_has(galloc*a, uint32_t p){ uint32_t m=QHASH_SZ-1u, i=qh_hash(p)&m; while(a->qhash[i]){ if(a->qhash[i]==p) return 1; i=(i+1)&m; } return 0; }
static void qh_ins(galloc*a, uint32_t p){ uint32_t m=QHASH_SZ-1u, i=qh_hash(p)&m; while(a->qhash[i]){ if(a->qhash[i]==p) return; i=(i+1)&m; } a->qhash[i]=p; }
static void qh_del(galloc*a, uint32_t p){ /* linear-probe backward-shift delete (no tombstones) */
    uint32_t m=QHASH_SZ-1u, i=qh_hash(p)&m;
    while(a->qhash[i]!=p){ if(!a->qhash[i]) return; i=(i+1)&m; }
    for(;;){ a->qhash[i]=0; uint32_t j=i,k;
        do { j=(j+1)&m; if(!a->qhash[j]) return; k=qh_hash(a->qhash[j])&m; } while ((i<=j) ? (i<k && k<=j) : (i<k || k<=j));
        a->qhash[i]=a->qhash[j]; i=j; }
}

/* ---- chunk field accessors (all via mem-ops) ---- */
static uint32_t H  (galloc*a,uint32_t c){ return gm_rd32(a->m,c+4); }
static void     SH (galloc*a,uint32_t c,uint32_t v){ gm_wr32(a->m,c+4,v); }
static uint32_t PF (galloc*a,uint32_t c){ return gm_rd32(a->m,c+0); }
static void     SPF(galloc*a,uint32_t c,uint32_t v){ gm_wr32(a->m,c+0,v); }
static uint32_t SZ (galloc*a,uint32_t c){ return H(a,c)&~FLAGS; }
static int      PIN(galloc*a,uint32_t c){ return H(a,c)&PINUSE; }
static int      CIN(galloc*a,uint32_t c){ return H(a,c)&CINUSE; }
static uint32_t FD (galloc*a,uint32_t c){ return gm_rd32(a->m,c+8); }
static void     SFD(galloc*a,uint32_t c,uint32_t v){ gm_wr32(a->m,c+8,v); }
static uint32_t BK (galloc*a,uint32_t c){ return gm_rd32(a->m,c+12); }
static void     SBK(galloc*a,uint32_t c,uint32_t v){ gm_wr32(a->m,c+12,v); }
#define MEM(c) ((c)+CHUNK_OVH)
#define CHK(p) ((p)-CHUNK_OVH)

/* Chunk sizing must be at least as generous as the allocator the ENGINE was built against.
 *
 * Android's malloc uses 16-granular small size classes (jemalloc on API 25, scudo on modern
 * releases), so malloc(n) yields align16(n) usable bytes, minimum 16. The old formula here gave
 * align16(n + 8) - 8, which is SHORT of that by 8 bytes for a whole family of sizes: n = 1, 8, 17,
 * 24, 33 ... For example malloc(17) gets 32 usable bytes on-device but only 24 from the old
 * formula.
 *
 * That difference was corrupting the heap. Traced 2026-07-27 with the per-op pinpoint: the engine
 * requests 17 bytes, memsets 17 into it (fits), then writes at payload offset 28 — which is inside
 * the 32 bytes a real device would have given it, but 4 bytes past our 24, landing exactly on the
 * SUCCESSOR chunk's head word and clearing PINUSE. Observed as galloc_check = -5, or -7 when the
 * successor happened to be the top chunk. Permanent from op ~12-26k, 383/384 checks failing.
 *
 * Writing past a 17-byte request is a latent bug in the engine, but it is invisible on a real
 * device because the size class absorbs it. We are emulating that device, so matching its
 * usable-size semantics is correctness, not a workaround — and it fixes the corruption at its
 * source rather than tolerating it.
 *
 * chunk = align16(n) + 16  =>  payload = align16(n) + 8  >=  align16(n) = what the device gives.
 * Costs 16 bytes more per small allocation than the old formula; the arena is 512MB.
 * MEASURED overhead (2026-07-27): +21% on the constructor phase (499768 -> 605096 bytes),
 * +11.7% and +6.5% at two later runtime samples. An earlier "+3.2%" note came from a
 * synthetic size mix and understated it — small-object-heavy phases pay the most. */
static uint32_t req2size(uint32_t n){
    uint32_t want = (n + (ALIGN-1)) & ~(ALIGN-1);     /* device-equivalent usable bytes */
    if (want < n) return 0;                           /* overflow guard (huge n) */
    if (want < ALIGN) want = ALIGN;                   /* device minimum class is 16 */
    uint32_t s = want + ALIGN;                        /* payload = want + 8 >= want */
    if (s < want) return 0;                           /* overflow guard */
    if (s < MIN_CHUNK) s = MIN_CHUNK;
    return s;
}

/* ---- free list (LIFO doubly-linked) ---- */
static void fl_insert(galloc*a,uint32_t c){
    SFD(a,c,a->freelist); SBK(a,c,0);
    if (a->freelist) SBK(a,a->freelist,c);
    a->freelist=c;
}
static void fl_unlink(galloc*a,uint32_t c){
    uint32_t f=FD(a,c), b=BK(a,c);
    if (b) SFD(a,b,f); else a->freelist=f;
    if (f) SBK(a,f,b);
}

galloc *galloc_create(guest_mem *m, uint32_t base, uint32_t size){
    if ((base & (ALIGN-1)) || (size & (ALIGN-1)) || size < 64) return 0;
    galloc *a = (galloc*)calloc(1,sizeof *a);
    if (!a) return 0;
    a->m=m; a->lo=base; a->hi=base+size;
    a->c0  = base + CHUNK_OVH;        /* MEM(c0)=base+16 is 16-aligned */
    a->rf  = a->hi - CHUNK_OVH;
    a->top = a->c0;
    a->topsize = a->rf - a->c0;       /* multiple of 16 */
    a->freelist = 0;
    SH (a, a->top, a->topsize | PINUSE);   /* top: PINUSE=1 (left fencepost), free */
    SPF(a, a->rf,  a->topsize);            /* top footer */
    SH (a, a->rf,  0 | CINUSE);            /* right fencepost: in-use, PINUSE=0 */
    return a;
}
void galloc_destroy(galloc *a){ free(a); }

/* Self-heal: rebuild the free list from the AUTHORITATIVE heap chunk-chain (the CINUSE bits + sizes, which
 * are not part of the corruption we've seen — only the fd/bk links get clobbered by a residual UAF write /
 * post-eviction double-free). Walks c0..rf, re-links every free non-top chunk with fresh fd/bk. O(chunks),
 * only invoked when galloc_malloc detects a cyclic free list, so the allocator never infinite-loops. */
static void galloc_rebuild_freelist(galloc*a){
    a->freelist = 0;
    uint32_t c = a->c0;
    while (c < a->rf){
        uint32_t sz = SZ(a,c);
        if (sz < MIN_CHUNK || c + sz > a->rf) break;   /* corrupt size -> stop (don't run away) */
        if (c != a->top && !CIN(a,c)) fl_insert(a,c);  /* a free, non-top chunk */
        c += sz;
    }
}
/* a valid free list can't be longer than this; exceeding it means a cycle (arena/MIN_CHUNK is the absolute
 * ceiling, but a real boot free list is orders of magnitude smaller — 1M is safely above yet cheap to hit). */
#define GALLOC_WALK_BOUND 1048576u

uint32_t galloc_malloc(galloc*a, uint32_t n){
    uint32_t size = req2size(n);
    if (!size) return 0;
    /* first-fit over the free list — cycle-safe: bound the walk; on a cyclic/corrupt list rebuild it once from
     * the heap chunk-chain and retry, so a residual UAF-induced free-list cycle can never freeze malloc. */
    uint32_t c = a->freelist, guard = 0; int rebuilt = 0;
    while (c){
        if (++guard > GALLOC_WALK_BOUND){
            if (rebuilt) break;                      /* already rebuilt once and still huge -> give up, use top */
            galloc_rebuild_freelist(a); rebuilt = 1; c = a->freelist; guard = 0; continue;
        }
        uint32_t cs = SZ(a,c);
        if (cs >= size){
            fl_unlink(a,c);
            if (cs - size >= MIN_CHUNK){             /* split off remainder */
                uint32_t r = c + size, rem = cs - size;
                SH (a,c, size | PINUSE);             /* c keeps PINUSE, CINUSE set below */
                SH (a,r, rem  | PINUSE);             /* r: free, prev(c) in use */
                SPF(a, r+rem, rem);                  /* r footer */
                SH (a, r+rem, H(a,r+rem) & ~PINUSE); /* chunk after r: prev(r) free */
                fl_insert(a,r);
                cs = size;
            }
            SH(a,c, H(a,c) | CINUSE);                /* mark c in use */
            SH(a, c+cs, H(a,c+cs) | PINUSE);
            return MEM(c);
        }
        c = FD(a,c);
    }
    /* carve from top (always leave a valid >= MIN_CHUNK top) */
    if (a->topsize >= size + MIN_CHUNK){
        uint32_t c = a->top;
        SH(a,c, size | (H(a,c)&PINUSE) | CINUSE);
        a->top = c + size; a->topsize -= size;
        SH (a, a->top, a->topsize | PINUSE);
        SPF(a, a->rf, a->topsize);
        return MEM(c);
    }
    /* Under memory pressure, reclaim the UAF quarantine (drain the held blocks -> they coalesce back into
     * the free list) and retry ONCE. This trades UAF protection for memory exactly when a large allocation
     * would otherwise fail — e.g. the level-load LZMA decode dictionary: its C allocator returns 0 and the
     * engine throws an "LzmaDec_Allocate" IOException that goes UNCAUGHT -> fatal. After the flush qcount==0,
     * so the retried call can't recurse again (bounded). */
    if (a->qcount){ galloc_flush(a); return galloc_malloc(a, n); }
    return 0;                                         /* exhaustion (M9) */
}

/* the real free (coalesce + relink); clobbers the block's first 8 bytes with fd/bk and makes it reusable */
static void galloc_free_real(galloc*a, uint32_t p){
    if (!p) return;
    uint32_t c = CHK(p);
    if (!CIN(a,c)) return;                             /* double-free guard */
    uint32_t sz = SZ(a,c);
    if (!PIN(a,c)){                                    /* coalesce with previous */
        uint32_t P = c - PF(a,c);
        fl_unlink(a,P);
        sz += SZ(a,P);
        c = P;
    }
    uint32_t N = c + sz;
    if (N == a->top){                                 /* absorb into top */
        a->top = c; a->topsize += sz;
        SH (a,c, a->topsize | PINUSE);
        SPF(a, a->rf, a->topsize);
        return;
    }
    if (!CIN(a,N)){                                   /* coalesce with next */
        fl_unlink(a,N);
        sz += SZ(a,N);
    }
    SH (a,c, sz | PINUSE);                             /* now a free chunk */
    SH (a, c+sz, H(a,c+sz) & ~PINUSE);
    SPF(a, c+sz, sz);
    fl_insert(a,c);
}

/* ---- WAF (write-after-free) DETECTOR ----------------------------------------------------------
 * A block held in quarantine has been "freed" from the engine's view, so NOTHING should touch it.
 * We snapshot a canary over its first 16 bytes (the std::string _Rep header: length/capacity/refcount)
 * + the free-site (guest LR) at free time; if the canary differs when the ring finally evicts the
 * block, the engine wrote to it via a STALE pointer == the premature-free/UAF. Logging that free-site
 * pins the free that shouldn't have happened (cont.95's "only true fix"). Read-only + on-eviction. */
static uint32_t g_gl_free_lr = 0;   /* engine-rel free-site LR; the free bridge (h_free) sets it per call */
void galloc_note_free_lr(uint32_t lr){ g_gl_free_lr = lr; }
static uint32_t gl_can(galloc*a, uint32_t p){   /* weighted canary (detects field swaps, not just XOR-cancel) */
    return gm_rd32(a->m,p) ^ (gm_rd32(a->m,p+4)*3u) ^ (gm_rd32(a->m,p+8)*5u) ^ (gm_rd32(a->m,p+12)*7u); }
#if defined(__ANDROID__) && !defined(ABSHIM_RELEASE)   /* WAF diagnostic log: non-release only. The FIX (canary + targeted leak below) is always active. */
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
static void gl_waf_log(uint32_t blk, uint32_t lr, uint32_t oc, uint32_t nc){
    static int(*rl)(int,const char*,const char*,...)=0; static int t=0,n=0;
    if(!t){t=1; rl=(int(*)(int,const char*,const char*,...))dlsym(RTLD_DEFAULT,"__android_log_print");}
    if(rl && n++<64) rl(5,"abshim","[WAF] block@0x%x freed@engine+0x%x WRITTEN while quarantined (canary %08x->%08x) => premature-free/stale-ptr",blk,lr,oc,nc); }
#else
static void gl_waf_log(uint32_t blk, uint32_t lr, uint32_t oc, uint32_t nc){ (void)blk;(void)lr;(void)oc;(void)nc; }
#endif
/* Public free = QUARANTINE. Hold the freed ptr in a FIFO ring (block stays CINUSE, bytes untouched, not
 * reusable) for a->qn ops; only when the ring is full do we really free the OLDEST held block. This shifts
 * the actual reclamation/clobber well past the window in which a prematurely-freed _Rep is still read,
 * neutralizing the use-after-free generically (grind, registry loop, garbage strings) without needing to
 * identify every stale-read site. Held blocks read as normal in-use chunks to galloc_check. Quarantine is
 * OFF by default (a->qn==0 => immediate free, so every host allocator test keeps exact reclamation
 * semantics); the production shim turns it on via galloc_set_quarantine(). */
void galloc_free(galloc*a, uint32_t p){
    if (!p) return;
    uint32_t c = CHK(p);
    if (!CIN(a,c)) return;                             /* double-free guard (block already reclaimed) */
    uint32_t qn = a->qn;
    if (qn == 0){ galloc_free_real(a, p); return; }    /* quarantine disabled: free immediately */
    /* Only skip the quarantine for VERY large blocks (audio-decode PCM etc., which are MBs and never a
     * std::string _Rep). A modest limit like 512 wrongly excluded long-string _Reps -> reintroduced the UAF
     * (observed as a stuck LDREX refcount fault); keep the threshold well above any _Rep. */
    if (SZ(a,c) - CHUNK_OVH > QUARANTINE_MAX_SIZE){ galloc_free_real(a, p); return; }  /* huge block: reclaim now (not a _Rep) */
    if (qh_has(a, p)) return;                           /* already HELD in quarantine => double-free; no-op (else it corrupts the free-list) */
    if (a->qcount < qn){                               /* still room: just defer */
        uint32_t slot=(a->qhead + a->qcount) % qn;
        a->qr[slot] = p; a->qr_can[slot]=gl_can(a,p); a->qr_lr[slot]=g_gl_free_lr;   /* WAF snapshot */
        a->qcount++; qh_ins(a, p);
        return;
    }
    uint32_t oldest = a->qr[a->qhead];                 /* ring full: evict the oldest, enqueue p */
    uint32_t oc=a->qr_can[a->qhead], nc=gl_can(a,oldest);
    int waf = (nc!=oc);                                /* canary changed while "freed"+held => a live STALE pointer aliases it */
    if (waf) gl_waf_log(oldest, a->qr_lr[a->qhead], oc, nc);
    a->qr[a->qhead] = p; a->qr_can[a->qhead]=gl_can(a,p); a->qr_lr[a->qhead]=g_gl_free_lr;   /* WAF snapshot for the new held block */
    a->qhead = (a->qhead + 1) % qn;
    qh_del(a, oldest); qh_ins(a, p);
    /* *** THE UAF FIX (cont.104) ***: if the block was written after free (WAF), the engine still holds a
     * stale pointer to it, so RECLAIMING it (galloc_free_real) would let a later alloc reuse the address and
     * the stale write would corrupt that new object (the level-end crash). Instead LEAK it — keep it CINUSE
     * forever so its address is never reused; the stale write then lands harmlessly on the abandoned block.
     * A precise, bounded leak (only genuinely-corrupted _Reps, a few dozen tiny blocks) that eliminates the
     * residual std::string UAF at its source rather than merely deferring it. Clean blocks reclaim normally. */
    if (!waf) galloc_free_real(a, oldest);
}
/* Drain all held blocks (real-free them) — restores immediate-reclamation semantics for leak/coalesce checks. */
void galloc_flush(galloc*a){
    while (a->qcount){                                 /* qcount>0 implies qn>0 (only enqueued when qn>0) */
        uint32_t idx = a->qhead;
        uint32_t p = a->qr[idx];
        a->qhead = (a->qhead + 1) % a->qn;
        a->qcount--;
        uint32_t nc = gl_can(a,p);                     /* UAF FIX: leak WAF'd blocks here too (stale alias) */
        if (nc == a->qr_can[idx]) galloc_free_real(a, p);
        else gl_waf_log(p, a->qr_lr[idx], a->qr_can[idx], nc);
    }
    a->qhead = 0;
    memset(a->qhash, 0, sizeof a->qhash);              /* held-set is now empty */
}
/* Set the quarantine depth (clamped to ring capacity). Drains any current holds first. n==0 disables it. */
void galloc_set_quarantine(galloc*a, uint32_t n){
    if (n > QUARANTINE_N) n = QUARANTINE_N;
    galloc_flush(a);
    a->qn = n;
}

#ifdef GALLOC_DEBUG
#include <stdio.h>
static uint32_t _realloc(galloc*a,uint32_t p,uint32_t n);
uint32_t galloc_realloc(galloc*a,uint32_t p,uint32_t n){
    uint32_t rv=_realloc(a,p,n);
    if (rv && !(gm_rd32(a->m,rv-4)&CINUSE)){
        fprintf(stderr,"REALLOC path=%d returned FREE chunk: p=0x%08x n=%u rv=0x%08x hd=0x%08x\n",
                g_rpath,p,n,rv,gm_rd32(a->m,rv-4)); abort(); }
    return rv;
}
int galloc_dbg_rpath(void){ return g_rpath; }
static uint32_t _realloc(galloc*a, uint32_t p, uint32_t n){
    if (!p) { g_rpath=0; return galloc_malloc(a,n); }
#else
uint32_t galloc_realloc(galloc*a, uint32_t p, uint32_t n){
    if (!p) return galloc_malloc(a,n);
#endif
    if (n==0){ galloc_free(a,p); return 0; }          /* free + NULL (glibc-style) */
    uint32_t c = CHK(p);
#ifdef GALLOC_DEBUG
    if(!(H(a,c)&CINUSE)){ fprintf(stderr,"_realloc ENTRY p=0x%08x already FREE hd=0x%08x\n",p,H(a,c)); abort(); }
#endif
    uint32_t oldsize = SZ(a,c), oldpay = oldsize - CHUNK_OVH;
    uint32_t newsize = req2size(n);
    if (!newsize) return 0;

    if (newsize <= oldsize){                          /* shrink in place */
        if (oldsize - newsize >= MIN_CHUNK){
            uint32_t r = c + newsize, rem = oldsize - newsize;
            SH(a,c, newsize | (H(a,c)&(PINUSE|CINUSE)));
            SH(a,r, rem | PINUSE | CINUSE);           /* mark then free() to coalesce fwd */
            SH(a, r+rem, H(a,r+rem) | PINUSE);
            galloc_free(a, MEM(r));
        }
        RPATH(2); return p;
    }
    uint32_t N = c + oldsize;
    /* grow into an adjacent free (non-top) chunk */
    if (N != a->top && !CIN(a,N) && oldsize + SZ(a,N) >= newsize){
        uint32_t merged = oldsize + SZ(a,N);
        fl_unlink(a,N);
        SH(a,c, merged | (H(a,c)&(PINUSE|CINUSE)));
        SH(a, c+merged, H(a,c+merged) | PINUSE);
        if (merged - newsize >= MIN_CHUNK){           /* trim the surplus */
            uint32_t r = c + newsize, rem = merged - newsize;
            SH(a,c, newsize | (H(a,c)&(PINUSE|CINUSE)));
            SH(a,r, rem | PINUSE | CINUSE);
            SH(a, r+rem, H(a,r+rem) | PINUSE);
            galloc_free(a, MEM(r));
        }
        RPATH(3); return p;
    }
    /* grow into top */
    if (N == a->top && oldsize + a->topsize >= newsize + MIN_CHUNK){
        uint32_t need = newsize - oldsize;
        SH(a,c, newsize | (H(a,c)&(PINUSE|CINUSE)));
        a->top = c + newsize; a->topsize -= need;
        SH (a, a->top, a->topsize | PINUSE);
        SPF(a, a->rf, a->topsize);
        RPATH(4); return p;
    }
    /* relocate: malloc new, copy min(old,new) (M2), free old */
    uint32_t np = galloc_malloc(a,n);
    if (!np) {
#ifdef GALLOC_DEBUG
        if(!(H(a,c)&CINUSE)){ fprintf(stderr,"_realloc PATH6 p=0x%08x FREED by malloc(n=%u) hd=0x%08x\n",p,n,H(a,c)); abort(); }
#endif
        RPATH(6); return 0; }                           /* old p stays valid */
    uint32_t copy = oldpay < n ? oldpay : n;
    uint8_t buf[4096]; uint32_t off=0;
    while (off < copy){ uint32_t k = copy-off; if (k>sizeof buf) k=sizeof buf;
        a->m->read(a->m,buf,p+off,k); a->m->write(a->m,np+off,buf,k); off+=k; }
    galloc_free(a,p);
    RPATH(5); return np;
}

uint32_t galloc_calloc(galloc*a, uint32_t nmemb, uint32_t size){
    uint64_t tot = (uint64_t)nmemb * size;
    if (tot > 0xFFFFFFFFu) return 0;                   /* overflow (M10) */
    uint32_t t = (uint32_t)tot;
    uint32_t p = galloc_malloc(a,t);
    if (p && t){
        uint8_t z[4096]; memset(z,0,sizeof z); uint32_t off=0;
        while (off < t){ uint32_t k=t-off; if (k>sizeof z) k=sizeof z;
            a->m->write(a->m,p+off,z,k); off+=k; }
    }
    return p;
}

uint32_t galloc_usable(galloc*a, uint32_t p){
    if (!p) return 0;
    return SZ(a,CHK(p)) - CHUNK_OVH;
}

uint32_t galloc_inuse_bytes(galloc*a){
    uint32_t sum=0, c=a->c0;
    while (c != a->top && c < a->rf){
        if (CIN(a,c)) sum += SZ(a,c) - CHUNK_OVH;
        c += SZ(a,c);
    }
    return sum;
}

/* galloc_check_where — same walk as galloc_check, but reports WHICH chunk failed.
 * Added 2026-07-27: the live shim reports rc=-5 permanently from op 16384 and
 * test_galloc_quarantine.c has cleared the allocator itself, so the writer is external.
 * Finding it needs the address to put a Unicorn write-watchpoint on. `bad` receives the
 * offending chunk (0 if the failure is not chunk-local, e.g. a free-list defect). */
int galloc_check_where(galloc*a, uint32_t *bad){
    if (bad) *bad = 0;
#define BAD(c_) do{ if(bad) *bad=(c_); }while(0)
    if (!(H(a,a->c0) & PINUSE)) { BAD(a->c0); return -1; }   /* left fencepost */
    uint32_t c = a->c0; int prev_free = 0; uint32_t walkfree = 0;
    while (c < a->rf){
        if ((c & 0xF) != 8) { BAD(c); return -2; }      /* mem must be 16-aligned */
        uint32_t sz = SZ(a,c);
        if (sz == 0) { BAD(c); return -3; }
        if (sz < MIN_CHUNK && c != a->top) { BAD(c); return -3; }
        if (c + sz > a->rf) { BAD(c); return -4; }      /* overruns fencepost */
        uint32_t nx = c + sz;
        int cin = CIN(a,c) ? 1 : 0;
        int npin = (H(a,nx) & PINUSE) ? 1 : 0;
        if (cin != npin) { BAD(c); return -5; }         /* PINUSE(next)==CINUSE(cur) */
        if (c == a->top){
            if (cin) { BAD(c); return -6; }             /* top must be free */
            if (sz != a->topsize) { BAD(c); return -7; }
            if (prev_free) { BAD(c); return -18; }      /* free chunk adjacent to top */
        } else if (!cin){
            if (PF(a,nx) != sz) { BAD(c); return -8; }  /* footer must equal size */
            if (prev_free) { BAD(c); return -9; }       /* two adjacent free chunks */
            walkfree++; prev_free = 1;
        } else {
            prev_free = 0;
        }
        c = nx;
    }
    if (c != a->rf) { BAD(c); return -10; }             /* must land on fencepost */
    /* free-list validity + count */
    if (a->freelist && BK(a,a->freelist) != 0) { BAD(a->freelist); return -11; }
    uint32_t fl=0, x=a->freelist, guard=0;
    while (x){
        if (x < a->c0 || x >= a->rf) { BAD(x); return -12; }
        if (CIN(a,x)) { BAD(x); return -13; }
        if (x == a->top) { BAD(x); return -14; }
        uint32_t f = FD(a,x);
        if (f && BK(a,f) != x) { BAD(x); return -15; }
        fl++; x=f;
        if (++guard > 10000000u) { BAD(x); return -16; }
    }
    if (fl != walkfree) return -17;
    return 0;
#undef BAD
}

int galloc_check(galloc*a){ return galloc_check_where(a, 0); }
