/* dispatch.c — boundary trap + libc bridge set. See dispatch.h.
 * Each handler pulls its own args from the AAPCS32 marshal cursor (so 64-bit /
 * double / varargs marshal correctly) and returns a value written per its ret
 * class. This is the libc slice the init-time surface needs; the full per-symbol
 * descriptor table (Audit 01/09) generalises it for the whole libc/GL/JNI set. */
#include "dispatch.h"
#include "regions.h"
#include "marshal.h"
#include "format.h"
#include "bridge.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>

/* DIAG (temporary): reach logcat from dispatch.c (env vars don't reach Android apps). */
static void dbg_log(const char*fmt,...){
    static int (*rl)(int,const char*,const char*,...)=0; static int tried=0;
    if(!tried){ tried=1; rl=(int(*)(int,const char*,const char*,...))dlsym(RTLD_DEFAULT,"__android_log_print"); }
    char buf[256]; va_list ap; va_start(ap,fmt); vsnprintf(buf,sizeof buf,fmt,ap); va_end(ap);
    if(rl) rl(4,"abshim","%s",buf); else fprintf(stderr,"%s\n",buf);
}
#define RGE(x) ((x)>=RG_ENGINE?(unsigned)((x)-RG_ENGINE):(unsigned)(x))

/* ---- guest-memory helpers ---- */
static void em_copy(cpu_t *c, uint32_t d, uint32_t s, uint32_t n){
    uint8_t b[8192]; while (n){ uint32_t k=n<sizeof b?n:sizeof b; uc_mem_read(c->uc,s,b,k); uc_mem_write(c->uc,d,b,k); s+=k; d+=k; n-=k; }
}
static void em_move(cpu_t *c, uint32_t d, uint32_t s, uint32_t n){        /* overlap-correct (L2) */
    if (d <= s || d >= s+n){ em_copy(c,d,s,n); return; }
    uint8_t b[8192]; uint32_t rem=n; while (rem){ uint32_t k=rem<sizeof b?rem:sizeof b; rem-=k; uc_mem_read(c->uc,s+rem,b,k); uc_mem_write(c->uc,d+rem,b,k); }
}
static void em_set(cpu_t *c, uint32_t d, int v, uint32_t n){
    uint8_t b[4096]; memset(b,v,n<sizeof b?n:sizeof b); while (n){ uint32_t k=n<sizeof b?n:sizeof b; uc_mem_write(c->uc,d,b,k); d+=k; n-=k; }
}
static int em_str(cpu_t *c, uint32_t p, char *o, int max){ int i=0; for(;i<max-1;i++){ uint8_t ch; uc_mem_read(c->uc,p+i,&ch,1); if(!ch)break; o[i]=(char)ch; } o[i]=0; return i; }

#define M(d)  (&(d)->cpu->mem)
static uint32_t W(dispatch_t*d, mcur*c){ return marshal_pull_word(M(d), c); }
static uint64_t DW(dispatch_t*d, mcur*c){ return marshal_pull_dword(M(d), c); }
static double d_in(uint64_t bits){ double x; memcpy(&x,&bits,8); return x; }
static uint64_t d_out(double x){ uint64_t b; memcpy(&b,&x,8); return b; }

/* ---- handlers: pull args from the cursor, return value ---- */
/* DIAG (cont.78 track a): every 8192 malloc/free, validate the whole heap (boundary tags + free
 * list). galloc was 450k-op torture-tested clean, but if the ENGINE's churn hits an edge case that
 * corrupts a chunk header, the confirmed use-after-free would be a galloc bug, not the string logic.
 * The FIRST failure pins WHEN the heap goes bad (before any string/_Rep symptom). galloc_check: 0=OK.
 *
 * PERF (2026-07-27): this is now RELEASE-GATED, matching the WAF free-site diag in h_free
 * below. It was shipping in the arm64 release build (the [GALLOC-CORRUPT] string was present
 * in out/angrybirds-8.0.3-arm64.apk), on the hot h_malloc/h_free path. galloc_check walks
 * EVERY chunk plus the whole free list, and in the real runtime each header field is its own
 * 4-byte uc_mem_read (cpu.c:20) — not a memcpy. Measured on a Core Ultra 9 285K, Unicorn-
 * backed (i.e. the production path):
 *      5k live chunks (634KB)  -> 0.451 ms      100k (11.4MB) -> 4.134 ms
 *     20k live chunks (2.4MB)  -> 1.288 ms      200k (22.5MB) -> 7.912 ms
 * against a 16.7 ms frame budget, on a phone whose cores are ~2-3x slower. Purely diagnostic,
 * so it must not ship. The non-release build keeps it, which is where heap triage belongs. */
/* DIAG DETAIL (2026-07-27): the old logger capped at 8 lines, so every API-25 run reported
 * exactly "8" and the true frequency was unknowable — 8 could have meant 8 events or 80,000.
 * It also could not distinguish a heap that goes bad ONCE and stays bad from one that is
 * transiently inconsistent mid-operation. Now we log STATE TRANSITIONS (OK<->BAD) plus a
 * periodic census, which bounds log volume while answering both questions. Non-release only. */
#ifndef ABSHIM_RELEASE
static void heap_ck(dispatch_t*d){
    static unsigned long ops=0, checks=0, bad=0, first_bad_op=0;
    static int last_rc=0, transitions=0, first_rc=0;
    /* Sample densely early (every 256 ops for the first 64k) so the FIRST failure is pinned to a
     * narrow window, then back off to every 8192 so the rest of the run stays affordable. The
     * failure has been observed between op 8192 and 16384, which the coarse rate could not localise. */
    unsigned long mask = (ops < 65536UL) ? 0xffUL : 0x1fffUL;
    if((++ops & mask)!=0) return;
    checks++;
    uint32_t badchunk = 0;
    int rc = galloc_check_where(d->cpu->heap, &badchunk);
    if(rc) bad++;
    if(rc && !first_bad_op){
        first_bad_op = ops; first_rc = rc;
        dbg_log("[GALLOC-CORRUPT] FIRST failure rc=%d at ~%lu ops (check #%lu) -> heap chunk/free-list inconsistent", rc, ops, checks);
        if(badchunk){
            /* Dump the chunk and its neighbour so the shape of the damage is visible: for rc=-5
             * the question is whether CINUSE(cur) or PINUSE(next) is the bit that moved. */
            /* Chunk layout (galloc.c:97-103): prev_foot at c+0, HEAD at c+4, payload at c+8;
             * FLAGS=3, PINUSE=1, CINUSE=2. Reading c+0 gives prev_foot, which is meaningless for
             * an in-use chunk — an earlier version of this dump did exactly that and printed a
             * phantom hdr=0. Read c+4 and mask ~3. */
            uint32_t h  = gm_rd32(&d->cpu->mem, badchunk + 4);
            uint32_t sz = h & ~3u;
            uint32_t nx = badchunk + sz;
            uint32_t nh = gm_rd32(&d->cpu->mem, nx + 4);
            dbg_log("[GALLOC-CORRUPT] chunk=0x%08x head=0x%08x size=%u cinuse=%u pf=0x%08x | next=0x%08x head=0x%08x pinuse=%u",
                    badchunk, h, sz, (h & 2u) ? 1u : 0u, gm_rd32(&d->cpu->mem, badchunk),
                    nx, nh, nh & 1u);
            /* Ask the chunk-head write watchpoint (cpu.c) who last wrote this head word. Only
             * GUEST instruction writes are recorded there — galloc's own gm_wr32 goes through
             * uc_mem_write, which does not fire UC_HOOK_MEM_WRITE. So a hit here names the guest
             * code that clobbered PINUSE; a miss means no guest instruction touched it, which
             * would point back at galloc after all and is itself the useful answer. */
            uint32_t wa=0, wv=0, wpc=0, wlr=0;
            if (cpu_hw_find(nx + 4, &wa, &wv, &wpc, &wlr))
                dbg_log("[GALLOC-CORRUPT] WRITER FOUND: guest wrote 0x%08x val=0x%08x at pc=0x%08x lr=0x%08x (engine+0x%x) — %lu head-writes seen",
                        wa, wv, wpc, wlr, wpc >= 0x40000000u ? wpc - 0x40000000u : wpc, cpu_hw_count());
            else
                dbg_log("[GALLOC-CORRUPT] NO guest write recorded for 0x%08x (%lu head-writes seen) — if the ring did not wrap, the writer is NOT guest code",
                        nx + 4, cpu_hw_count());
        }
    }
    if(rc != last_rc){
        if(++transitions <= 64)
            dbg_log("[GALLOC-CORRUPT] %s rc=%d at ~%lu ops (checks=%lu bad=%lu transitions=%d)",
                    rc ? "OK->BAD" : "BAD->OK RECOVERED", rc, ops, checks, bad, transitions);
        last_rc = rc;
    }
    if((checks % 64) == 0)
        dbg_log("[GALLOC-STATS] checks=%lu bad=%lu (%lu%%) first_bad_op=%lu first_rc=%d transitions=%d cur_rc=%d inuse=%u",
                checks, bad, checks ? (bad*100UL)/checks : 0UL, first_bad_op, first_rc, transitions, rc,
                galloc_inuse_bytes(d->cpu->heap));
}
#else
static inline void heap_ck(dispatch_t*d){ (void)d; }
#endif
/* ---- PER-OPERATION PINPOINT (non-release) -----------------------------------------------------
 * The UC_HOOK_MEM_WRITE watchpoint works but instruments every guest write and defeats TCG block
 * chaining, so a run cannot even reach the failure op inside a normal test window. This is the
 * cheap alternative: one heap walk per allocator call (~0.45 ms at 5k live chunks, measured) with
 * NO codegen instrumentation, so emulation speed is unaffected apart from the walks themselves.
 * It stops at the first failure, and the failure has been observed by op ~12-26k, so the total
 * cost is bounded at a few tens of seconds.
 *
 * It is decisive about WHO. Checking immediately before and immediately after each galloc call:
 *   BAD after the call, OK before it   -> that galloc operation broke the invariant
 *   OK after the call, BAD before next -> nothing in galloc did it; guest code executing between
 *                                         the two allocator calls wrote the header
 * Either answer eliminates half the search space, which the sampled census cannot do. */
#ifndef ABSHIM_RELEASE
static int  g_pin_done = 0;
static const char *g_pin_prev = "(none yet)";
static uint32_t g_pin_prev_lr = 0;      /* guest LR at the last CLEAN allocator call */
static uint32_t g_pin_args[4] = {0,0,0,0};   /* r0-r3 at the last CLEAN bridge entry */
static uint32_t g_pin_cur_args[4] = {0,0,0,0}; /* r0-r3 of the bridge currently running */
/* Recent allocations (returned payload ptr + requested size). At the failure we look up the
 * block that immediately PRECEDES the corrupted chunk: that is the object being overflowed, and
 * its requested size versus the distance to the clobbered header says whether the engine wrote
 * past what it asked for (engine bug) or past what we gave it (shim sizing bug). */
#define PIN_ALLOC_RING 256u
static struct { uint32_t ptr, req; } g_pin_alloc[PIN_ALLOC_RING];
static unsigned long g_pin_alloc_n = 0;
static void pin_note_alloc(uint32_t ptr, uint32_t req){
    if(!ptr) return;
    unsigned i = (unsigned)(g_pin_alloc_n++ % PIN_ALLOC_RING);
    g_pin_alloc[i].ptr = ptr; g_pin_alloc[i].req = req;
}
static int pin_find_alloc(uint32_t ptr, uint32_t *req){
    unsigned long n = g_pin_alloc_n < PIN_ALLOC_RING ? g_pin_alloc_n : PIN_ALLOC_RING;
    for(unsigned long j=1;j<=n;j++){
        unsigned i=(unsigned)((g_pin_alloc_n-j)%PIN_ALLOC_RING);
        if(g_pin_alloc[i].ptr==ptr){ if(req)*req=g_pin_alloc[i].req; return 1; }
    }
    return 0;
}
void dispatch_pin_note_args(const uint32_t *a){ for(int i=0;i<4;i++) g_pin_cur_args[i]=a[i]; }
static void heap_pin(dispatch_t*d, const char *when, const char *op){
    if (g_pin_done) return;
    /* BOUND (2026-07-27): g_pin_done previously latched only on FAILURE. Once the corruption was
     * actually fixed there was no failure, so this kept running two full heap walks per bridge
     * call for the entire session — the run never got past boot and screenshotted a blank frame,
     * which looks exactly like a rendering regression. Retire the probe after a budget of checks
     * so a clean run costs a bounded amount. The failure has always appeared well inside this. */
    {
        static unsigned long budget = 0;
        if (++budget > 200000UL){
            g_pin_done = 1;
            dbg_log("[HEAP-PINPOINT] no failure within %lu checks — probe retiring, heap looks clean", budget - 1);
            return;
        }
    }
    uint32_t badchunk = 0;
    int rc = galloc_check_where(d->cpu->heap, &badchunk);
    uint32_t lr = 0; uc_reg_read(d->cpu->uc, UC_ARM_REG_LR, &lr);
    if (!rc){ if (when[0]=='a'){ g_pin_prev = op; g_pin_prev_lr = lr;
                              for(int i=0;i<4;i++) g_pin_args[i]=g_pin_cur_args[i]; } return; }  /* remember last clean op */
    g_pin_done = 1;
    dbg_log("[HEAP-PINPOINT] FIRST failure rc=%d detected %s %s() — last clean point was after %s() — chunk=0x%08x",
            rc, when, op, g_pin_prev, badchunk);
    /* The guest code that did it ran between these two return addresses. Both are engine-relative,
     * so they can be looked up directly in reports/eng.dis. This costs one register read per
     * allocator call — vastly cheaper than a memory watchpoint, and it brackets the culprit to the
     * span of guest code executed between two consecutive allocator entries. */
    dbg_log("[HEAP-PINPOINT] GUEST WINDOW: from engine+0x%x (return of the last clean %s) to engine+0x%x (caller of the failing %s) — the write happened in this span",
            RGE(g_pin_prev_lr), g_pin_prev, RGE(lr), op);
    dbg_log("[HEAP-PINPOINT] last clean %s() args: r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x (for memset/memcpy that is dest, value/src, n) — corrupted chunk was 0x%08x, its payload 0x%08x",
            g_pin_prev, g_pin_args[0], g_pin_args[1], g_pin_args[2], g_pin_args[3],
            badchunk, badchunk + 8u);
    {   /* Which allocation sits immediately BEFORE the corrupted chunk? Walk back over the
         * recent-allocation ring looking for a payload whose chunk+size lands exactly here. */
        uint32_t req=0; int found=0;
        for(uint32_t back=16; back<=256 && !found; back+=16){
            uint32_t cand_chunk = badchunk - back;
            if(pin_find_alloc(cand_chunk + 8u, &req)){
                uint32_t chunksz = gm_rd32(&d->cpu->mem, cand_chunk+4) & ~3u;
                dbg_log("[HEAP-PINPOINT] OVERFLOWED BLOCK: payload 0x%08x requested %u bytes -> chunk 0x%08x size %u (payload capacity %u); the clobbered head is at 0x%08x = payload offset %u",
                        cand_chunk+8u, req, cand_chunk, chunksz, chunksz-8u, badchunk+4u, (badchunk+4u)-(cand_chunk+8u));
                found=1;
            }
        }
        if(!found) dbg_log("[HEAP-PINPOINT] (no recent allocation matches the block before 0x%08x)", badchunk);
    }
    /* The LR alone is not enough: it lands inside _Znwj (operator new) at engine+0x85a620, which
     * merely forwards to malloc. The interesting frame is whoever called operator new. Scan the
     * guest stack for plausible return addresses — words pointing into the engine image with the
     * Thumb bit set — to get a shallow backtrace. Crude (no unwind info, so some hits are stale
     * slots) but enough to name the calling subsystem, and it costs only a few reads. */
    {
        uint32_t sp = 0; uc_reg_read(d->cpu->uc, UC_ARM_REG_SP, &sp);
        dbg_log("[HEAP-PINPOINT] guest sp=0x%08x — scanning for engine return addresses:", sp);
        int shown = 0;
        for (uint32_t o = 0; o < 256u && shown < 12; o += 4){
            uint32_t w = gm_rd32(&d->cpu->mem, sp + o);
            if (w >= RG_ENGINE && w < RG_ENGINE + 0x1000000u && (w & 1u)){
                dbg_log("[HEAP-PINPOINT]   sp+%3u -> engine+0x%x", o, RGE(w & ~1u));
                shown++;
            }
        }
        if (!shown) dbg_log("[HEAP-PINPOINT]   (no engine-range return addresses found in the first 256 bytes)");
    }
    if (when[0]=='a')
        dbg_log("[HEAP-PINPOINT] VERDICT: %s() ITSELF broke the invariant (heap was clean on entry) -> the bug is INSIDE galloc", op);
    else
        dbg_log("[HEAP-PINPOINT] VERDICT: heap was clean after %s() and is broken on entry to %s() -> none of the INSTRUMENTED shim entry points (malloc/calloc/realloc/free + memcpy/memmove/memset/strcpy/strncpy/strcat/strdup) ran in between, so guest INSTRUCTIONS wrote the header",
                g_pin_prev, op);
}
#else
#define heap_pin(d,w,o) ((void)0)
#endif

/* ALLOC TRACE (env-gated, non-release): dump the guest's allocation SEQUENCE so two hosts can be
 * diffed against each other. The x86 and arm64 guest heaps differ by a near-constant ~64KB after
 * the 125 ctors (605096 vs 539536), deterministically on each host — so some allocation differs in
 * size or presence. Sizes alone, in order, localise it to the first divergent request. */
#ifndef ABSHIM_RELEASE
static FILE *g_atr = NULL; static int g_atr_init = 0;
static void alloc_trace_lr(dispatch_t*d, const char *what, uint32_t n){
    if(!g_atr_init){ g_atr_init=1; const char *p=getenv("ABSHIM_ALLOC_TRACE"); if(p&&*p) g_atr=fopen(p,"w"); }
    /* flush per record: without this the FILE* is block-buffered, so a `timeout` kill
     * truncates the trace and a live diff against another host reads a stale tail. */
    uint32_t lr=0; if(d) uc_reg_read(d->cpu->uc, UC_ARM_REG_LR, &lr);
    if(g_atr){ fprintf(g_atr, "%s %u lr=%x\n", what, n, RGE(lr & ~1u)); fflush(g_atr); }
}
#else
#define alloc_trace_lr(d,w,n) ((void)0)
#endif
static uint64_t h_malloc (dispatch_t*d,mcur*c){ heap_ck(d); heap_pin(d,"before","malloc");
    uint32_t n=W(d,c); alloc_trace_lr(d,"m",n); uint64_t r=galloc_malloc(d->cpu->heap,n?n:1);
#ifndef ABSHIM_RELEASE
    pin_note_alloc((uint32_t)r, n);
#endif
    heap_pin(d,"after","malloc"); return r; }
static uint64_t h_calloc (dispatch_t*d,mcur*c){ heap_pin(d,"before","calloc");
    uint32_t a=W(d,c),b=W(d,c); alloc_trace_lr(d,"c",a*b); uint64_t r=galloc_calloc(d->cpu->heap,a,b); heap_pin(d,"after","calloc"); return r; }
static uint64_t h_realloc(dispatch_t*d,mcur*c){ heap_pin(d,"before","realloc");
    uint32_t p=W(d,c),n=W(d,c); alloc_trace_lr(d,"r",n); uint64_t r=galloc_realloc(d->cpu->heap,p,n); heap_pin(d,"after","realloc"); return r; }
static uint64_t h_free   (dispatch_t*d,mcur*c){ heap_ck(d);
#ifndef ABSHIM_RELEASE
    uint32_t lr=0; uc_reg_read(d->cpu->uc,UC_ARM_REG_LR,&lr); galloc_note_free_lr(RGE(lr));   /* WAF free-site diag (per-free) — non-release only; the leak fix uses only the canary */
#endif
    heap_pin(d,"before","free"); galloc_free(d->cpu->heap,W(d,c)); heap_pin(d,"after","free"); return 0; }
static uint64_t h_memcpy (dispatch_t*d,mcur*c){ heap_pin(d,"before","memcpy"); uint32_t a=W(d,c),b=W(d,c),n=W(d,c); em_copy(d->cpu,a,b,n);  uint64_t _r_=a; heap_pin(d,"after","memcpy"); return _r_; }
static uint64_t h_memmove(dispatch_t*d,mcur*c){ heap_pin(d,"before","memmove"); uint32_t a=W(d,c),b=W(d,c),n=W(d,c); em_move(d->cpu,a,b,n);  uint64_t _r_=a; heap_pin(d,"after","memmove"); return _r_; }
static uint64_t h_ae_cpy (dispatch_t*d,mcur*c){ uint32_t a=W(d,c),b=W(d,c),n=W(d,c); em_copy(d->cpu,a,b,n); return 0; }
static uint64_t h_ae_move(dispatch_t*d,mcur*c){ uint32_t a=W(d,c),b=W(d,c),n=W(d,c); em_move(d->cpu,a,b,n); return 0; }
static uint64_t h_memset (dispatch_t*d,mcur*c){ heap_pin(d,"before","memset"); uint32_t a=W(d,c),v=W(d,c),n=W(d,c); em_set(d->cpu,a,(int)(v&0xff),n);  uint64_t _r_=a; heap_pin(d,"after","memset"); return _r_; }
static uint64_t h_ae_set (dispatch_t*d,mcur*c){ uint32_t a=W(d,c),n=W(d,c),v=W(d,c); em_set(d->cpu,a,(int)(v&0xff),n); return 0; } /* (dst,n,c) reversed L1 */
static uint64_t h_ae_clr (dispatch_t*d,mcur*c){ uint32_t a=W(d,c),n=W(d,c); em_set(d->cpu,a,0,n); return 0; }
static uint64_t h_memcmp (dispatch_t*d,mcur*c){ uint32_t a=W(d,c),b=W(d,c),n=W(d,c); for(uint32_t i=0;i<n;i++){ uint8_t x,y; uc_mem_read(d->cpu->uc,a+i,&x,1); uc_mem_read(d->cpu->uc,b+i,&y,1); if(x!=y) return (uint32_t)((int)x-(int)y); } return 0; }
static uint64_t h_memchr (dispatch_t*d,mcur*c){ uint32_t a=W(d,c),v=W(d,c),n=W(d,c); for(uint32_t i=0;i<n;i++){ uint8_t ch; uc_mem_read(d->cpu->uc,a+i,&ch,1); if(ch==(v&0xff)) return a+i; } return 0; }
static uint64_t h_strlen (dispatch_t*d,mcur*c){ char b[8192]; return em_str(d->cpu,W(d,c),b,sizeof b); }
static uint64_t h_strcmp (dispatch_t*d,mcur*c){ char x[4096],y[4096]; em_str(d->cpu,W(d,c),x,sizeof x); em_str(d->cpu,W(d,c),y,sizeof y); return (uint32_t)strcmp(x,y); }
static uint64_t h_strncmp(dispatch_t*d,mcur*c){ char x[4096],y[4096]; uint32_t a=W(d,c),b=W(d,c),n=W(d,c); em_str(d->cpu,a,x,sizeof x); em_str(d->cpu,b,y,sizeof y); return (uint32_t)(n?strncmp(x,y,n):0); }
static uint64_t h_strcpy (dispatch_t*d,mcur*c){ heap_pin(d,"before","strcpy"); uint32_t a=W(d,c),b=W(d,c); char t[8192]; int L=em_str(d->cpu,b,t,sizeof t); em_copy(d->cpu,a,b,L+1);  uint64_t _r_=a; heap_pin(d,"after","strcpy"); return _r_; }
static uint64_t h_strncpy(dispatch_t*d,mcur*c){ heap_pin(d,"before","strncpy"); uint32_t a=W(d,c),b=W(d,c),n=W(d,c); char t[8192]; int L=em_str(d->cpu,b,t,sizeof t); uint32_t k=((uint32_t)L<n)?(uint32_t)L:n; em_copy(d->cpu,a,b,k); if(k<n) em_set(d->cpu,a+k,0,n-k);  uint64_t _r_=a; heap_pin(d,"after","strncpy"); return _r_; }
static uint64_t h_strcat (dispatch_t*d,mcur*c){ heap_pin(d,"before","strcat"); uint32_t a=W(d,c),b=W(d,c); char dd[8192]; int dl=em_str(d->cpu,a,dd,sizeof dd); char s[8192]; int sl=em_str(d->cpu,b,s,sizeof s); em_copy(d->cpu,a+dl,b,sl+1);  uint64_t _r_=a; heap_pin(d,"after","strcat"); return _r_; }
static uint64_t h_strchr (dispatch_t*d,mcur*c){ uint32_t a=W(d,c),ch=W(d,c); char b[8192]; em_str(d->cpu,a,b,sizeof b); char*p=strchr(b,(int)ch); return p?a+(uint32_t)(p-b):0; }
static uint64_t h_strrchr(dispatch_t*d,mcur*c){ uint32_t a=W(d,c),ch=W(d,c); char b[8192]; em_str(d->cpu,a,b,sizeof b); char*p=strrchr(b,(int)ch); return p?a+(uint32_t)(p-b):0; }
static uint64_t h_strstr (dispatch_t*d,mcur*c){ uint32_t a=W(d,c),b=W(d,c); char h[8192],q[1024]; em_str(d->cpu,a,h,sizeof h); em_str(d->cpu,b,q,sizeof q); char*p=strstr(h,q); return p?a+(uint32_t)(p-h):0; }
static uint64_t h_strdup (dispatch_t*d,mcur*c){ heap_pin(d,"before","strdup"); uint32_t a=W(d,c); char b[8192]; int L=em_str(d->cpu,a,b,sizeof b); uint32_t p=galloc_malloc(d->cpu->heap,L+1); if(p) em_copy(d->cpu,p,a,L+1);  uint64_t _r_=p; heap_pin(d,"after","strdup"); return _r_; }
static uint64_t h_zero   (dispatch_t*d,mcur*c){ (void)d;(void)c; return 0; }
static uint64_t h_errno  (dispatch_t*d,mcur*c){ (void)c; return (d->sch && sched_current(d->sch)) ? sched_errno_addr(d->sch) : d->errno_slot; }
static uint64_t h_setloc (dispatch_t*d,mcur*c){ (void)c; return d->c_locale; }
static uint64_t h_pagesz (dispatch_t*d,mcur*c){ (void)d;(void)c; return 4096; }   /* getpagesize() */
/* sysconf(name): dispatch on the bionic _SC_ constant. Returning the pagesize for EVERY
 * name (the old behavior) is catastrophic for _SC_NPROCESSORS_ONLN -> the engine would size
 * a worker pool to 4096. Report a modest CPU count (the GEL serialises guest threads anyway),
 * the standard CLK_TCK, and a sane page/phys/fd budget. */
static uint64_t h_sysconf(dispatch_t*d,mcur*c){ (void)d; uint32_t name=W(d,c);
    switch(name){
    case 0x27u: case 0x28u: return 4096;      /* _SC_PAGESIZE / _SC_PAGE_SIZE */
    case 0x60u: case 0x61u: return 4;         /* _SC_NPROCESSORS_CONF / _ONLN */
    case 0x06u: return 100;                   /* _SC_CLK_TCK */
    case 0x62u: return 262144u;               /* _SC_PHYS_PAGES  (~1 GiB at 4K) */
    case 0x0bu: return 1024;                  /* _SC_OPEN_MAX */
    default: return 4096;                     /* conservative positive fallback */
    }
}
static uint64_t h_getaux (dispatch_t*d,mcur*c){ (void)d; return W(d,c)==16 ? (0x40u|0x2000u|0x1000u) : 0; }
static uint64_t h_syscall(dispatch_t*d,mcur*c){ (void)d; uint32_t nr=W(d,c); if(nr==240)return 0; if(nr==224)return 1234; return 0; }
/* __android_log_print(prio,tag,fmt,...): format it, then FORWARD to the real logger so the
 * engine's own diagnostics reach logcat on-device (else they are silently dropped — and the
 * engine logs the *reason* right before it aborts). ABSHIM_LOG=1 also echoes to stderr (host). */
static uint64_t h_log    (dispatch_t*d,mcur*c){ uint32_t pr=W(d,c); uint32_t tag=W(d,c); uint32_t fmt=W(d,c);
    char hb[512],tg[80]; uint32_t hl; fmt_to_host(M(d),hb,sizeof hb,fmt,*c,&hl); em_str(d->cpu,tag,tg,sizeof tg);
    static int (*rl)(int,const char*,const char*,...)=0; static int tried=0;
    if(!tried){ tried=1; rl=(int(*)(int,const char*,const char*,...))dlsym(RTLD_DEFAULT,"__android_log_print"); }
    if(rl) rl((int)pr,tg,"%s",hb);
    static int on=-1; if(on<0) on=getenv("ABSHIM_LOG")?1:0;
    if(on) fprintf(stderr,"[engine:%s] %s\n",tg,hb);
    return 0; }
static uint64_t h_logw   (dispatch_t*d,mcur*c){ (void)W(d,c);(void)W(d,c); uint32_t txt=W(d,c); char b[512]; em_str(d->cpu,txt,b,sizeof b); return 0; }
static uint64_t h_fatal  (dispatch_t*d,mcur*c){ (void)c;
    uint32_t pc=0,lr=0; uc_reg_read(d->cpu->uc,UC_ARM_REG_PC,&pc); uc_reg_read(d->cpu->uc,UC_ARM_REG_LR,&lr);
    { static int (*rl)(int,const char*,const char*,...)=0; static int tr=0,n=0;   /* DIAG: pin the stack-smash site */
      if(!tr){tr=1; rl=(int(*)(int,const char*,const char*,...))dlsym(RTLD_DEFAULT,"__android_log_print");}
      if(rl && n++<8) rl(6,"abshim","[h_fatal] %s @stub0x%x caller=engine+0x%x (the FAILING function)",
          pc==RG_STUB+0x50u?"__stack_chk_fail(STACK-SMASH)":"abort/exit", pc, RGE(lr)); }
    d->fatal=1; strncpy(d->fatal_name,"abort",sizeof d->fatal_name-1); uc_emu_stop(d->cpu->uc); return 0; }
/* __gnu_Unwind_Find_exidx(pc,int*nrec) / dl_unwind_find_exidx: hand the engine's static
 * C++ unwinder its own PT_ARM_EXIDX table for a PC inside the engine, with the entry count
 * in *nrec. Bionic supplies this on-device; a 0 here makes every throw std::terminate. */
static uint64_t h_find_exidx(dispatch_t*d,mcur*c){
    uint32_t pc=W(d,c), nrec=W(d,c);
    uint32_t va=d->ld?d->ld->img.exidx_va:0, sz=d->ld?d->ld->img.exidx_sz:0, imgsz=d->ld?d->ld->img.image_size:0;
    if(va && pc>=RG_ENGINE && pc<RG_ENGINE+imgsz){ if(nrec) gm_wr32(M(d),nrec,sz/8u); return RG_ENGINE+va; }
    if(nrec) gm_wr32(M(d),nrec,0);
    return 0;
}
/* stdio format */
static uint64_t h_printf (dispatch_t*d,mcur*c){ uint32_t fmt=W(d,c); char hb[1024]; uint32_t hl; return fmt_to_host(M(d),hb,sizeof hb,fmt,*c,&hl); }
static uint64_t h_sprintf(dispatch_t*d,mcur*c){ uint32_t buf=W(d,c),fmt=W(d,c); return fmt_to_guest(M(d),buf,0xFFFFFFFFu,fmt,*c); }
static uint64_t h_snprintf(dispatch_t*d,mcur*c){ uint32_t buf=W(d,c),sz=W(d,c),fmt=W(d,c); return fmt_to_guest(M(d),buf,sz,fmt,*c); }
/* libm (double in r0:r1) */
static uint64_t h_ceil (dispatch_t*d,mcur*c){ return d_out(ceil (d_in(DW(d,c)))); }
static uint64_t h_floor(dispatch_t*d,mcur*c){ return d_out(floor(d_in(DW(d,c)))); }
/* wide-char (C locale) */
static uint64_t h_btowc(dispatch_t*d,mcur*c){ return W(d,c); }
static uint64_t h_wctob(dispatch_t*d,mcur*c){ uint32_t x=W(d,c); return x<0x80?x:0xffffffffu; }
static uint64_t h_wctype(dispatch_t*d,mcur*c){ (void)W(d,c); return 1; }
/* single-GEC threading: uncontended -> success; keys allocate ids */
static uint64_t h_pmutex(dispatch_t*d,mcur*c){ (void)d;(void)c; return 0; }  /* attr/init/destroy no-op */
/* REAL time (Audit 06 L9 + H1): the guest's monotonic clock must advance with real
 * time so animation deltas AND pthread_cond_timedwait deadlines are in the same domain
 * the scheduler compares against (host CLOCK_MONOTONIC). The old +16ms/call fake made
 * every timed wait fire instantly and decoupled game timing from reality. */
static uint64_t h_clockgt(dispatch_t*d,mcur*c){ uint32_t clk=W(d,c),ts=W(d,c);
    struct timespec t; clock_gettime(clk==0?CLOCK_REALTIME:CLOCK_MONOTONIC,&t);
    if(ts){ gm_wr32(M(d),ts,(uint32_t)t.tv_sec); gm_wr32(M(d),ts+4,(uint32_t)t.tv_nsec); } return 0; }
static uint64_t h_gettod (dispatch_t*d,mcur*c){ uint32_t tv=W(d,c);(void)W(d,c);
    struct timespec t; clock_gettime(CLOCK_REALTIME,&t);
    if(tv){ gm_wr32(M(d),tv,(uint32_t)t.tv_sec); gm_wr32(M(d),tv+4,(uint32_t)(t.tv_nsec/1000)); } return 0; }
/* file I/O (open/read/write/close/lseek/fstat) is handled real by bridge_file.c */
/* setjmp/longjmp: guest-state save/restore + stop/restart PC redirect (Audit 04 X2/D9).
 * jmp_buf layout (our own, fits bionic's 256-byte buffer): [0..28]=r4..r11,
 * [32]=sp, [36]=lr(resume PC), [40..103]=d8..d15. */
static uint64_t h_setjmp(dispatch_t*d, mcur*c){
    uint32_t jb=W(d,c); cpu_t*cc=d->cpu; uint32_t v;
    for(int r=0;r<8;r++){ uc_reg_read(cc->uc,UC_ARM_REG_R4+r,&v); gm_wr32(&cc->mem,jb+r*4,v); }
    uc_reg_read(cc->uc,UC_ARM_REG_SP,&v); gm_wr32(&cc->mem,jb+32,v);
    uc_reg_read(cc->uc,UC_ARM_REG_LR,&v); gm_wr32(&cc->mem,jb+36,v);
    for(int i=0;i<8;i++){ uint64_t dv=0; uc_reg_read(cc->uc,UC_ARM_REG_D8+i,&dv); gm_wr32(&cc->mem,jb+40+i*8,(uint32_t)dv); gm_wr32(&cc->mem,jb+44+i*8,(uint32_t)(dv>>32)); }
    { static int n=0; if(n++<64) dbg_log("setjmp jb=0x%x resume=eng+0x%x",jb,RGE(v)); }
    return 0;                                       /* setjmp returns 0 */
}
static uint64_t h_longjmp(dispatch_t*d, mcur*c){
    uint32_t jb=W(d,c), val=W(d,c); cpu_t*cc=d->cpu; uint32_t v;
    { static int n=0; uint32_t from=0; uc_reg_read(cc->uc,UC_ARM_REG_LR,&from);
      uint32_t tgt=gm_rd32(&cc->mem,jb+36);
      if(n++<64) dbg_log("LONGJMP from=eng+0x%x -> resume=eng+0x%x jb=0x%x val=%u",RGE(from),RGE(tgt),jb,val); }
    for(int r=0;r<8;r++){ v=gm_rd32(&cc->mem,jb+r*4); uc_reg_write(cc->uc,UC_ARM_REG_R4+r,&v); }
    v=gm_rd32(&cc->mem,jb+32); uc_reg_write(cc->uc,UC_ARM_REG_SP,&v);
    uint32_t lr=gm_rd32(&cc->mem,jb+36);
    for(int i=0;i<8;i++){ uint64_t lo=gm_rd32(&cc->mem,jb+40+i*8),hi=gm_rd32(&cc->mem,jb+44+i*8); uint64_t dv=lo|(hi<<32); uc_reg_write(cc->uc,UC_ARM_REG_D8+i,&dv); }
    uint32_t rv=val?val:1; uc_reg_write(cc->uc,UC_ARM_REG_R0,&rv);   /* longjmp -> setjmp returns val (or 1) */
    /* Don't fight the stub's `bx lr` — USE it: set LR to the setjmp resume addr
       (with its Thumb bit); the bx lr then jumps there with the correct mode. */
    uc_reg_write(cc->uc,UC_ARM_REG_LR,&lr);
    return 0;                                        /* ret=0: return-writer must NOT touch r0 */
}

/* ---- real guest threading: route pthread_* to the GEL scheduler (Audit 02) ----
 * A blocking primitive (mutex_lock contended / cond_wait / join) arms LR=RG_RET inside
 * sched_*, so uc_emu_start unwinds and the run-loop parks this gthread; the eventual
 * r0 is injected on resume. Pre-scheduler (init before sched_init) these degrade to the
 * uncontended single-thread answer. */
#define HAVE_SCH(d) ((d)->sch && sched_current((d)->sch))
static uint64_t h_pcreate (dispatch_t*d,mcur*c){ uint32_t th=W(d,c),attr=W(d,c),fn=W(d,c),arg=W(d,c); (void)attr; if(!d->sch) return 0; uint32_t tid=0; int r=sched_create(d->sch,fn,arg,&tid); if(!r&&th) gm_wr32(M(d),th,tid);
    if(getenv("ABSHIM_LOG")) fprintf(stderr,"[pthread_create] fn=0x%x tid=%u r=%d\n",fn>=RG_ENGINE?fn-RG_ENGINE:fn,tid,r);
    return (uint32_t)r; }
static uint64_t h_pjoin   (dispatch_t*d,mcur*c){ uint32_t tid=W(d,c),rp=W(d,c); if(!HAVE_SCH(d)){ if(rp)gm_wr32(M(d),rp,0); return 0;} return (uint32_t)sched_join(d->sch,tid,rp); }
static uint64_t h_pdetach (dispatch_t*d,mcur*c){ uint32_t t=W(d,c); return HAVE_SCH(d)?(uint32_t)sched_detach(d->sch,t):0; }
static uint64_t h_pexit   (dispatch_t*d,mcur*c){ uint32_t rv=W(d,c); if(HAVE_SCH(d)) sched_exit(d->sch,rv); return 0; }
static uint64_t h_pself   (dispatch_t*d,mcur*c){ (void)c; return HAVE_SCH(d)?sched_self(d->sch):1u; }
static uint64_t h_pequal  (dispatch_t*d,mcur*c){ (void)d; uint32_t a=W(d,c),b=W(d,c); return a==b?1u:0u; }
static uint64_t h_pyield  (dispatch_t*d,mcur*c){ (void)c; if(HAVE_SCH(d)) sched_yield_now(d->sch); return 0; }
static uint64_t h_mlock   (dispatch_t*d,mcur*c){ uint32_t m=W(d,c); return HAVE_SCH(d)?(uint32_t)sched_mutex_lock(d->sch,m,0):0; }
static uint64_t h_mtry    (dispatch_t*d,mcur*c){ uint32_t m=W(d,c); return HAVE_SCH(d)?(uint32_t)sched_mutex_lock(d->sch,m,1):0; }
static uint64_t h_munlock (dispatch_t*d,mcur*c){ uint32_t m=W(d,c); return HAVE_SCH(d)?(uint32_t)sched_mutex_unlock(d->sch,m):0; }
static uint64_t h_cwait   (dispatch_t*d,mcur*c){ uint32_t cv=W(d,c),m=W(d,c); return HAVE_SCH(d)?(uint32_t)sched_cond_wait(d->sch,cv,m,0):0; }
static uint64_t h_ctwait  (dispatch_t*d,mcur*c){ uint32_t cv=W(d,c),m=W(d,c),ts=W(d,c); uint64_t dl=0; if(ts){ uint64_t s=gm_rd32(M(d),ts),ns=gm_rd32(M(d),ts+4); dl=s*1000000000ull+ns; } return HAVE_SCH(d)?(uint32_t)sched_cond_wait(d->sch,cv,m,dl):0; }
static uint64_t h_csig    (dispatch_t*d,mcur*c){ uint32_t cv=W(d,c); return HAVE_SCH(d)?(uint32_t)sched_cond_wake(d->sch,cv,0):0; }
static uint64_t h_cbcast  (dispatch_t*d,mcur*c){ uint32_t cv=W(d,c); return HAVE_SCH(d)?(uint32_t)sched_cond_wake(d->sch,cv,1):0; }
static uint64_t h_rwr     (dispatch_t*d,mcur*c){ uint32_t o=W(d,c); return HAVE_SCH(d)?(uint32_t)sched_rwlock_lock(d->sch,o,1,0):0; }
static uint64_t h_rrd     (dispatch_t*d,mcur*c){ uint32_t o=W(d,c); return HAVE_SCH(d)?(uint32_t)sched_rwlock_lock(d->sch,o,0,0):0; }
static uint64_t h_rwtryw  (dispatch_t*d,mcur*c){ uint32_t o=W(d,c); return HAVE_SCH(d)?(uint32_t)sched_rwlock_lock(d->sch,o,1,1):0; }
static uint64_t h_rwtryr  (dispatch_t*d,mcur*c){ uint32_t o=W(d,c); return HAVE_SCH(d)?(uint32_t)sched_rwlock_lock(d->sch,o,0,1):0; }
static uint64_t h_rwunlk  (dispatch_t*d,mcur*c){ uint32_t o=W(d,c); return HAVE_SCH(d)?(uint32_t)sched_rwlock_unlock(d->sch,o):0; }
static uint64_t h_ponce2  (dispatch_t*d,mcur*c){ uint32_t o=W(d,c),r=W(d,c); if(HAVE_SCH(d)) return (uint32_t)sched_once(d->sch,o,r); /* pre-sched fallback: run inline once */ if(gm_rd32(M(d),o)==0){ gm_wr32(M(d),o,2); uc_context*x=NULL; uc_context_alloc(d->cpu->uc,&x); uc_context_save(d->cpu->uc,x); uint32_t lr=RG_RET; uc_reg_write(d->cpu->uc,UC_ARM_REG_LR,&lr); cpu_run(d->cpu,r,0); uc_context_restore(d->cpu->uc,x); uc_context_free(x); gm_wr32(M(d),o,1);} return 0; }
static uint64_t h_pkey2   (dispatch_t*d,mcur*c){ uint32_t key=W(d,c),destr=W(d,c); uint32_t k = d->sch? sched_key_create(d->sch,destr) : (d->tls_key_next?d->tls_key_next++:(d->tls_key_next=2,1)); if(key)gm_wr32(M(d),key,k); return 0; }
static uint64_t h_pkeydel (dispatch_t*d,mcur*c){ uint32_t k=W(d,c); return d->sch?(uint32_t)sched_key_delete(d->sch,k):0; }
static uint64_t h_getspec2(dispatch_t*d,mcur*c){ uint32_t k=W(d,c); return HAVE_SCH(d)?sched_getspecific(d->sch,k):d->tls_store[k&0xff]; }
static uint64_t h_setspec2(dispatch_t*d,mcur*c){ uint32_t k=W(d,c),v=W(d,c); if(HAVE_SCH(d)) return (uint32_t)sched_setspecific(d->sch,k,v); d->tls_store[k&0xff]=v; return 0; }

typedef uint64_t (*bfn)(dispatch_t*, mcur*);
typedef struct { const char*name; int ret; bfn fn; } bent;   /* ret: 0 void,1 word,2 i64/f64 */
static const bent BR[] = {
    {"malloc",1,h_malloc},{"valloc",1,h_malloc},{"calloc",1,h_calloc},{"realloc",1,h_realloc},{"free",0,h_free},
    {"memcpy",1,h_memcpy},{"memmove",1,h_memmove},{"memset",1,h_memset},{"memcmp",1,h_memcmp},{"memchr",1,h_memchr},
    {"__aeabi_memcpy",0,h_ae_cpy},{"__aeabi_memcpy4",0,h_ae_cpy},{"__aeabi_memcpy8",0,h_ae_cpy},
    {"__aeabi_memmove",0,h_ae_move},{"__aeabi_memmove4",0,h_ae_move},{"__aeabi_memmove8",0,h_ae_move},
    {"__aeabi_memset",0,h_ae_set},{"__aeabi_memset4",0,h_ae_set},{"__aeabi_memset8",0,h_ae_set},
    {"__aeabi_memclr",0,h_ae_clr},{"__aeabi_memclr4",0,h_ae_clr},{"__aeabi_memclr8",0,h_ae_clr},
    {"strlen",1,h_strlen},{"strcmp",1,h_strcmp},{"strncmp",1,h_strncmp},{"strcpy",1,h_strcpy},{"stpcpy",1,h_strcpy},
    {"strncpy",1,h_strncpy},{"strcat",1,h_strcat},{"strchr",1,h_strchr},{"strrchr",1,h_strrchr},{"strstr",1,h_strstr},{"strdup",1,h_strdup},
    {"__cxa_atexit",1,h_zero},{"atexit",1,h_zero},{"__cxa_finalize",0,h_zero},{"__register_atfork",1,h_zero},
    {"__gnu_Unwind_Find_exidx",2,h_find_exidx},{"dl_unwind_find_exidx",2,h_find_exidx},   /* C++ exidx finder */
    {"__errno",1,h_errno},{"setlocale",1,h_setloc},{"sysconf",1,h_sysconf},{"getpagesize",1,h_pagesz},{"getauxval",1,h_getaux},{"syscall",1,h_syscall},
    {"__android_log_print",1,h_log},{"__android_log_write",1,h_logw},
    {"abort",0,h_fatal},{"__stack_chk_fail",0,h_fatal},{"exit",0,h_fatal},{"_exit",0,h_fatal},{"raise",0,h_fatal},
    {"printf",1,h_printf},{"sprintf",1,h_sprintf},{"snprintf",1,h_snprintf},{"vsnprintf",1,h_snprintf},
    {"ceil",2,h_ceil},{"floor",2,h_floor},
    {"btowc",1,h_btowc},{"wctob",1,h_wctob},{"wctype",1,h_wctype},
    {"pthread_create",1,h_pcreate},{"pthread_join",1,h_pjoin},{"pthread_detach",1,h_pdetach},
    {"pthread_exit",0,h_pexit},{"pthread_self",1,h_pself},{"pthread_equal",1,h_pequal},{"sched_yield",1,h_pyield},
    {"pthread_mutex_lock",1,h_mlock},{"pthread_mutex_trylock",1,h_mtry},{"pthread_mutex_unlock",1,h_munlock},
    {"pthread_mutex_init",1,h_pmutex},{"pthread_mutex_destroy",1,h_pmutex},
    {"pthread_mutexattr_init",1,h_pmutex},{"pthread_mutexattr_settype",1,h_pmutex},{"pthread_mutexattr_destroy",1,h_pmutex},{"pthread_mutexattr_setpshared",1,h_pmutex},
    {"pthread_cond_wait",1,h_cwait},{"pthread_cond_timedwait",1,h_ctwait},{"pthread_cond_timedwait_monotonic",1,h_ctwait},
    {"pthread_cond_timedwait_monotonic_np",1,h_ctwait},{"pthread_cond_timedwait_relative_np",1,h_ctwait},
    {"pthread_cond_signal",1,h_csig},{"pthread_cond_broadcast",1,h_cbcast},
    {"pthread_cond_init",1,h_pmutex},{"pthread_cond_destroy",1,h_pmutex},
    {"pthread_condattr_init",1,h_pmutex},{"pthread_condattr_destroy",1,h_pmutex},{"pthread_condattr_setclock",1,h_pmutex},{"pthread_condattr_setpshared",1,h_pmutex},
    {"pthread_rwlock_wrlock",1,h_rwr},{"pthread_rwlock_rdlock",1,h_rrd},{"pthread_rwlock_trywrlock",1,h_rwtryw},
    {"pthread_rwlock_tryrdlock",1,h_rwtryr},{"pthread_rwlock_unlock",1,h_rwunlk},
    {"pthread_rwlock_init",1,h_pmutex},{"pthread_rwlock_destroy",1,h_pmutex},
    {"pthread_key_create",1,h_pkey2},{"pthread_key_delete",1,h_pkeydel},{"pthread_once",1,h_ponce2},
    {"pthread_getspecific",1,h_getspec2},{"pthread_setspecific",1,h_setspec2},
    {"pthread_getschedparam",1,h_zero},{"pthread_setschedparam",1,h_zero},{"pthread_setname_np",1,h_zero},{"pthread_getname_np",1,h_zero},
    {"clock_gettime",1,h_clockgt},{"gettimeofday",1,h_gettod},
    {"setjmp",1,h_setjmp},{"_setjmp",1,h_setjmp},{"sigsetjmp",1,h_setjmp},
    {"longjmp",0,h_longjmp},{"_longjmp",0,h_longjmp},{"siglongjmp",0,h_longjmp},
};
/* Choke-point pinpoint: EVERY bridge call becomes a heap checkpoint, labelled with the bridge's
 * own name. This narrows the corruption window from "between two mallocs" (which spanned an
 * unbounded amount of engine code) to "between two adjacent bridge calls", and names the bridge
 * if one of them is the culprit. Cost is two heap walks per bridge call, but the pinpoint stops
 * at the first failure and the failure happens early, so it is bounded. Non-release only. */
#ifndef ABSHIM_RELEASE
static uint64_t br_call_pinned(dispatch_t*d, const bent*b, mcur*cur){
    {   /* AAPCS32: first four args live in r0-r3 on entry, before the cursor is pulled */
        uint32_t a[4];
        uc_reg_read(d->cpu->uc, UC_ARM_REG_R0, &a[0]); uc_reg_read(d->cpu->uc, UC_ARM_REG_R1, &a[1]);
        uc_reg_read(d->cpu->uc, UC_ARM_REG_R2, &a[2]); uc_reg_read(d->cpu->uc, UC_ARM_REG_R3, &a[3]);
        dispatch_pin_note_args(a);
    }
    heap_pin(d, "before", b->name);
    uint64_t r = b->fn(d, cur);
    heap_pin(d, "after", b->name);
    return r;
}
#else
#define br_call_pinned(d,b,cur) ((b)->fn((d),(cur)))
#endif
static const bent *find_bridge(const char*name){ for(size_t i=0;i<sizeof BR/sizeof BR[0];i++) if(!strcmp(BR[i].name,name)) return &BR[i]; return NULL; }

enum { SC_UNRES=0, SC_GL, SC_ASSET, SC_BR, SC_LIBC, SC_FILE, SC_UNIMPL };
static void br_ret(dispatch_t*d, const bent*b, uint64_t rv){
    if (b->ret>=1){ uint32_t lo=(uint32_t)rv; uc_reg_write(d->cpu->uc,UC_ARM_REG_R0,&lo); }
    if (b->ret==2){ uint32_t hi=(uint32_t)(rv>>32); uc_reg_write(d->cpu->uc,UC_ARM_REG_R1,&hi); }
}
/* hang-watchdog op marker: STRONG def in jni_entry.c (device); this WEAK no-op keeps the host
 * test builds (which link dispatch.c but not jni_entry.c) resolving — overridden on device. */
__attribute__((weak)) void abshim_mark_op(const char *bridge, int slot){ (void)bridge; (void)slot; }
static void stub_cb(uc_engine *uc, uint64_t address, uint32_t size, void *user){
    (void)uc;(void)size;
    dispatch_t *d=(dispatch_t*)user;
    uint32_t slot=((uint32_t)address - RG_STUB) >> 2;
    /* watchdog op-marker: mark bridge calls CHEAPLY (no per-call loader_stub_name lookup — that
     * linear scan over 344 stubs, run on every memcpy/str/fopen bridge, slowed the grind ~15x). The
     * JNI mark in env_dispatch_real is what pins the deadlocks; here just bump the progress seq. */
    abshim_mark_op(NULL, -1);
    /* DIAG: hot-stub detector. A guest that spins calling a bridged import (observed: libcurl's
     * connect/poll retry loop after we cut the network) hammers one stub millions of times; log
     * its NAME every 2^19 calls so the spin is named without guessing the resolution order. */
    { static unsigned long hot[LOADER_MAX_STUBS];
      if(slot<LOADER_MAX_STUBS && ((++hot[slot])&0x7ffffUL)==0UL){
          const char*hn=loader_stub_name(d->ld,(uint32_t)address);
          dbg_log("[hot-stub] slot=%u name=%s calls=%lu",slot,hn?hn:"?",hot[slot]); } }
    uint8_t kind = (slot<LOADER_MAX_STUBS) ? d->scache_kind[slot] : SC_UNRES;

    /* ---- fast path: this stub was resolved on a prior call; skip the strcmp chains ---- */
    if (kind==SC_BR){ const bent*b=(const bent*)d->scache_br[slot]; mcur cur; marshal_cur_init(&d->cpu->mem,&cur); br_ret(d,b,br_call_pinned(d,b,&cur)); return; }
    if (kind==SC_UNIMPL){ uint32_t z=0; uc_reg_write(d->cpu->uc,UC_ARM_REG_R0,&z); return; }
    if (kind){
        const char *nm=loader_stub_name(d->ld,(uint32_t)address); if(!nm) return;
        mcur c; marshal_cur_init(&d->cpu->mem,&c);
        if (kind==SC_GL) gl_try(d->cpu,nm,&c);
        else if (kind==SC_ASSET) asset_try(d->cpu,d->jni,nm,&c);
        else if (kind==SC_LIBC) libc_try(d->cpu,nm,&c);
        else file_try(d->cpu,nm,&c);                 /* SC_FILE */
        return;
    }

    /* ---- slow path: resolve once (walk the tables), execute, and cache the outcome ---- */
    const char *name=loader_stub_name(d->ld,(uint32_t)address);
    if (!name) return;
    uint8_t rk=SC_UNIMPL;
    if (name[0]=='g' && name[1]=='l'){ mcur gc; marshal_cur_init(&d->cpu->mem,&gc); if(gl_try(d->cpu,name,&gc)){ rk=SC_GL; goto cache; } }
    if (!strncmp(name,"AAsset",6) && d->jni){ mcur ac; marshal_cur_init(&d->cpu->mem,&ac); if(asset_try(d->cpu,d->jni,name,&ac)){ rk=SC_ASSET; goto cache; } }
    { const bent *b=find_bridge(name);
      if (b){ mcur cur; marshal_cur_init(&d->cpu->mem,&cur); br_ret(d,b,br_call_pinned(d,b,&cur));
              if(slot<LOADER_MAX_STUBS){ d->scache_kind[slot]=SC_BR; d->scache_br[slot]=b; } return; } }
    { mcur lc; marshal_cur_init(&d->cpu->mem,&lc); if(libc_try(d->cpu,name,&lc)){ rk=SC_LIBC; goto cache; } }
    { mcur fc; marshal_cur_init(&d->cpu->mem,&fc); if(file_try(d->cpu,name,&fc)){ rk=SC_FILE; goto cache; } }
    /* unimplemented (should not happen in production: coverage_check.py gates 0) */
    d->unimpl_count++; strncpy(d->last_unimpl,name,sizeof d->last_unimpl-1);
    { int seen=0; for(int i=0;i<d->unimpl_distinct;i++) if(!strcmp(d->unimpl_names[i],name)){seen=1;break;}
      if(!seen && d->unimpl_distinct<96){ strncpy(d->unimpl_names[d->unimpl_distinct],name,47); d->unimpl_distinct++; } }
    { uint32_t z=0; uc_reg_write(d->cpu->uc,UC_ARM_REG_R0,&z); }
cache:
    if (slot<LOADER_MAX_STUBS) d->scache_kind[slot]=rk;
}

/* ---- raw SVC (`svc #0`) syscall emulation --------------------------------------------------
 * The engine is statically linked with bionic, whose syscall stubs issue `svc #0` directly
 * (ARM EABI: r7 = number, args r0..r6, return in r0). Unicorn raises UC_ERR_EXCEPTION on `svc`
 * unless a UC_HOOK_INTR is installed. Function-level libc is bridged (stub_cb) so the HOT paths
 * never reach here; the COLD ones that do (altstack/signal setup inside pthread & breakpad, a
 * few proc/time queries) MUST be emulated or they abort the enclosing JNI call. Observed:
 * sigaltstack(186) fires inside the scene ctor => nativeInit aborts => nothing renders.
 * Unemulated numbers are logged once and return -ENOSYS so the next gap is visible. */
static void h_svc(uc_engine*uc, uint32_t intno, void*user){
    dispatch_t*d=(dispatch_t*)user; cpu_t*c=d->cpu;
    uint32_t nr=0,a0=0,a1=0,a2=0;
    uc_reg_read(uc,UC_ARM_REG_R7,&nr);
    uc_reg_read(uc,UC_ARM_REG_R0,&a0); uc_reg_read(uc,UC_ARM_REG_R1,&a1); uc_reg_read(uc,UC_ARM_REG_R2,&a2);
    /* UC_HOOK_INTR delivers ALL CPU exceptions, not just svc. intno 2 = EXCP_SWI (a real syscall);
     * 1=undef, 3=prefetch-abort, 4=data-abort. A non-SWI here means the guest hit a real fault (e.g.
     * an UNALIGNED access to a corrupt pointer -> alignment data-abort); reading r7 as a syscall # is
     * nonsense. Log the PC+regs (bounded) so the fault is diagnosable instead of a garbage-nr ENOSYS. */
    if(intno!=2u){
        uint32_t pc=0,r3=0,sp=0,lr=0; uc_reg_read(uc,UC_ARM_REG_PC,&pc); uc_reg_read(uc,UC_ARM_REG_R3,&r3);
        uc_reg_read(uc,UC_ARM_REG_SP,&sp); uc_reg_read(uc,UC_ARM_REG_LR,&lr);
        static int seen=0; if(seen++<12)
            dbg_log("[EXC] intno=%u pc=engine+0x%x lr=+0x%x r0=0x%x r1=0x%x r2=0x%x r3=0x%x sp=0x%x",
                intno, pc>=RG_ENGINE?pc-RG_ENGINE:pc, lr>=RG_ENGINE?lr-RG_ENGINE:lr, a0,a1,a2,r3,sp);
        return;
    }
    uint32_t ret=(uint32_t)-38;                          /* -ENOSYS */
    switch(nr){
      /* signal / thread-state / vm-advice — safe no-ops under the single-process GEL model */
      case 73: case 119: case 125: case 126: case 172: case 173: case 174:
      case 175: case 186: case 220: case 256: case 260: case 338:
        ret=0; break;                                    /* rt_sigpending/sigreturn/mprotect/
                                                          * sigprocmask/prctl/rt_sigreturn/rt_sigaction/
                                                          * rt_sigprocmask/sigaltstack/madvise/
                                                          * set_tid_address/timerfd?/set_robust_list */
      case 20: case 64: ret=(uint32_t)getpid(); break;                    /* getpid/getppid */
      case 224: ret=(d->sch&&sched_current(d->sch))?sched_self(d->sch):1000u; break; /* gettid */
      case 78: { struct timeval tv; gettimeofday(&tv,0);                   /* gettimeofday(tv,tz) */
          if(a0){ gm_wr32(&c->mem,a0,(uint32_t)tv.tv_sec); gm_wr32(&c->mem,a0+4,(uint32_t)tv.tv_usec); } ret=0; } break;
      case 263: case 266: { struct timespec ts; if(clock_gettime((clockid_t)a0,&ts)){ ts.tv_sec=0; ts.tv_nsec=0; } /* clock_gettime/clock_getres */
          if(a1){ gm_wr32(&c->mem,a1,(uint32_t)ts.tv_sec); gm_wr32(&c->mem,a1+4,(uint32_t)ts.tv_nsec); } ret=0; } break;
      case 4: { if((a0==1u||a0==2u)&&a1&&a2){ char b[513]; uint32_t k=a2<512u?a2:512u;      /* write: route stdout/stderr to logcat */
          uc_mem_read(uc,a1,b,k); b[k]=0; dbg_log("[guest fd%u] %.*s",a0,(int)k,b); } ret=a2; } break;
      default: { static uint32_t seen[96]; static int ns=0; int known=0;
          for(int i=0;i<ns;i++) if(seen[i]==nr){ known=1; break; }
          if(!known && ns<96){ seen[ns++]=nr; dbg_log("[SVC] UNHANDLED intno=%u nr=%u (r0=0x%x r1=0x%x r2=0x%x) -> ENOSYS",intno,nr,a0,a1,a2); }
          ret=(uint32_t)-38; } break;
    }
    uc_reg_write(uc,UC_ARM_REG_R0,&ret);
}
/* DIAG: global block sampler. The scheduler-loop PC samplers only fire BETWEEN quanta, so a
 * thread stuck INSIDE one uc_emu_start quantum — a tight loop or a pathologically slow parse
 * that never yields — is invisible to them (observed: engine freezes for minutes right after
 * cacert.pem load, no sched-sample, no sched-dump). This fires per basic block; every 2^18
 * blocks it logs the guest PC, so a stuck quantum shows as a PC cycling in a SMALL range
 * (loop) vs sweeping over a large range (real progress). Soft-capped to bound logcat volume. */
static void h_blk(uc_engine*uc, uint64_t addr, uint32_t size, void*user){
    (void)uc;(void)size;(void)user;
    static unsigned long n=0; static uint32_t prev=0; static int cap=0;
    if(((++n)&0x3ffffUL)==0UL && cap<600){ cap++;
        uint32_t a=(uint32_t)addr, rel=(a>=RG_ENGINE)?a-RG_ENGINE:a;
        dbg_log("[blk-sample] n=%lu pc=+0x%x d=%d",n,rel,(int)(a-prev)); prev=a;
    }
}
int dispatch_install(dispatch_t *d, cpu_t *cpu, loader_t *ld){
    memset(d,0,sizeof *d); d->cpu=cpu; d->ld=ld;
    d->errno_slot=RG_GUESTDATA+0xF000u; gm_wr32(&cpu->mem,d->errno_slot,0);
    d->c_locale =RG_GUESTDATA+0xF010u; uint8_t cl[2]={'C',0}; cpu->mem.write(&cpu->mem,d->c_locale,cl,2);
    static uc_hook svc_hook;                              /* raw-syscall (svc #0) emulation — see h_svc */
    uc_hook_add(cpu->uc,&svc_hook,UC_HOOK_INTR,(void*)h_svc,d,(uint64_t)1,(uint64_t)0);
    /* NOTE: the h_blk UC_HOOK_BLOCK PC sampler is DISABLED — a global block hook breaks Unicorn's
     * LDREX/STREX exclusive monitor (STREX perpetually fails), which turned a refcount-increment
     * atomic loop @0x88e752 into an infinite spin (a Heisenbug). Re-enable only for targeted PC
     * tracing when atomics aren't on the hot path. (void)h_blk; keeps it compiled under -Wno-unused. */
    (void)h_blk;
    return uc_hook_add(cpu->uc,&d->stub_hook,UC_HOOK_CODE,(void*)stub_cb,d,RG_STUB,RG_STUB+RG_STUB_SZ);
}

int dispatch_run_init_array(dispatch_t *d, int *total){
    int ran=0,tot=0;
    for (uint32_t i=0;i<d->ld->init_count;i++){
        uint32_t fn=gm_rd32(&d->cpu->mem, RG_ENGINE+d->ld->init_va+i*4);
        if (fn==0||fn==0xffffffffu) continue;
        tot++; d->fatal=0;
        uc_err e=cpu_call(d->cpu,fn,0,0,0,0,0);
        if (e==UC_ERR_OK && !d->fatal) ran++;
        else { uint32_t pc=0; uc_reg_read(d->cpu->uc,UC_ARM_REG_PC,&pc);
            fprintf(stderr,"  ctor %u @0x%x FAILED: %s pc=0x%x fatal=%d last_unimpl=%s\n",i,fn,uc_strerror(e),pc,d->fatal,d->last_unimpl); break; }
    }
    if (total) *total=tot;
    return ran;
}
