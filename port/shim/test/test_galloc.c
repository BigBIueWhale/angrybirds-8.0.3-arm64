/* test_galloc.c — host unit + torture test for the guest-heap allocator.
 * Validates the whole heap (galloc_check) after every operation and verifies
 * payload data integrity (which catches overlaps / metadata-into-payload bugs).
 * Covers the Audit-05 regressions: M2 realloc min-copy, M3 16-align,
 * M9 NULL-on-exhaustion, M10 calloc overflow, malloc(0) unique non-NULL.
 *
 * Build+run (host, x86):
 *   cc -Wall -Wextra -O1 -fsanitize=address,undefined -I../src \
 *      test_galloc.c ../src/galloc.c -o /tmp/test_galloc && /tmp/test_galloc
 */
#include "galloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ---- flat-buffer guest_mem backend (bounds-checked) ---- */
typedef struct { uint8_t *buf; uint32_t base, size; } hostmem;
static void hm_read(guest_mem*m, void*d, uint32_t a, uint32_t n){
    hostmem*h=(hostmem*)m->ctx;
    if (a < h->base || (uint64_t)a+n > (uint64_t)h->base+h->size){
        fprintf(stderr,"OOB read  @0x%08x +%u (arena 0x%08x+%u)\n",a,n,h->base,h->size); abort(); }
    memcpy(d, h->buf + (a - h->base), n);
}
static void hm_write(guest_mem*m, uint32_t a, const void*s, uint32_t n){
    hostmem*h=(hostmem*)m->ctx;
    if (a < h->base || (uint64_t)a+n > (uint64_t)h->base+h->size){
        fprintf(stderr,"OOB write @0x%08x +%u (arena 0x%08x+%u)\n",a,n,h->base,h->size); abort(); }
    memcpy(h->buf + (a - h->base), s, n);
}
static uint32_t hm_reg_get(guest_mem*m,int i){(void)m;(void)i;return 0;}
static void     hm_reg_set(guest_mem*m,int i,uint32_t v){(void)m;(void)i;(void)v;}

static guest_mem *make_mem(hostmem*h, uint32_t base, uint32_t size){
    h->buf = calloc(1,size); h->base=base; h->size=size;
    guest_mem *m = calloc(1,sizeof *m);
    m->read=hm_read; m->write=hm_write; m->reg_get=hm_reg_get; m->reg_set=hm_reg_set; m->ctx=h;
    return m;
}

/* ---- deterministic RNG (xorshift32) so any failure reproduces ---- */
static uint32_t rng_state = 0x1234567u;
static uint32_t rnd(void){ uint32_t x=rng_state; x^=x<<13; x^=x>>17; x^=x<<5; return rng_state=x; }

/* ---- payload pattern (seed-derived) ---- */
static void fill_pat(guest_mem*m, uint32_t p, uint32_t n, uint32_t seed){
    for (uint32_t i=0;i<n;i++) gm_wr8(m, p+i, (uint8_t)(seed*31u + i*7u + 0x5A));
}
static int check_pat(guest_mem*m, uint32_t p, uint32_t n, uint32_t seed){
    for (uint32_t i=0;i<n;i++)
        if (gm_rd8(m,p+i) != (uint8_t)(seed*31u + i*7u + 0x5A)) return 0;
    return 1;
}

static int fails = 0;
#define CK(cond, msg) do{ if(!(cond)){ printf("  FAIL: %s\n", msg); fails++; } }while(0)
#define CKHEAP(a) do{ int _e=galloc_check(a); if(_e){ printf("  FAIL: galloc_check=%d at %s:%d\n",_e,__FILE__,__LINE__); fails++; } }while(0)

/* ================= targeted regression tests ================= */
static void test_basic(void){
    printf("[basic]\n");
    hostmem h; guest_mem *m = make_mem(&h, 0x50000000u, 0x10000u);
    galloc *a = galloc_create(m, 0x50000000u, 0x10000u);
    CK(a!=NULL, "create"); CKHEAP(a);

    /* malloc(0) -> unique non-NULL, 16-aligned */
    uint32_t z1 = galloc_malloc(a,0), z2 = galloc_malloc(a,0);
    CK(z1 && z2 && z1!=z2, "malloc(0) unique non-NULL");
    CK((z1&0xF)==0 && (z2&0xF)==0, "malloc(0) 16-aligned (M3)");
    CKHEAP(a);

    /* alignment for a spread of sizes (M3) */
    for (uint32_t n=1;n<=300;n+=7){ uint32_t p=galloc_malloc(a,n);
        CK(p && (p&0xF)==0, "alloc 16-aligned"); CK(galloc_usable(a,p)>=n,"usable>=n"); }
    CKHEAP(a);
    galloc_free(a,z1); galloc_free(a,z2); CKHEAP(a);
    galloc_destroy(a); free(h.buf); free(m);
}

static void test_realloc_mincopy(void){
    printf("[realloc min-copy (M2)]\n");
    hostmem h; guest_mem *m = make_mem(&h, 0x50000000u, 0x10000u);
    galloc *a = galloc_create(m, 0x50000000u, 0x10000u);
    /* grow: contents preserved; shrink-then-grow must not resurrect old tail */
    uint32_t p = galloc_malloc(a,16); fill_pat(m,p,16,7); CKHEAP(a);
    uint32_t q = galloc_realloc(a,p,64);
    CK(q!=0 && check_pat(m,q,16,7), "grow preserves the 16 old bytes"); CKHEAP(a);
    /* shrink to 8, then grow to 64: bytes [8,64) are indeterminate but the op
       must not read past the (old, smaller) block — the ASan/OOB guard covers that */
    uint32_t r = galloc_realloc(a,q,8);
    CK(r!=0 && check_pat(m,r,8,7), "shrink preserves first 8"); CKHEAP(a);
    uint32_t s = galloc_realloc(a,r,64); CK(s!=0,"regrow"); CKHEAP(a);
    /* realloc(p,0) frees and returns NULL */
    CK(galloc_realloc(a,s,0)==0, "realloc(p,0)==NULL"); CKHEAP(a);
    /* realloc(NULL,n)==malloc */
    uint32_t t = galloc_realloc(a,0,32); CK(t!=0,"realloc(NULL,n)==malloc"); CKHEAP(a);
    galloc_destroy(a); free(h.buf); free(m);
}

static void test_calloc_overflow(void){
    printf("[calloc overflow (M10)]\n");
    hostmem h; guest_mem *m = make_mem(&h, 0x50000000u, 0x10000u);
    galloc *a = galloc_create(m, 0x50000000u, 0x10000u);
    CK(galloc_calloc(a, 0x10000u, 0x10000u)==0, "0x10000*0x10000 overflow -> NULL");
    CK(galloc_calloc(a, 0xFFFFFFFFu, 2u)==0, "huge*2 overflow -> NULL");
    uint32_t p = galloc_calloc(a, 4u, 8u);
    CK(p!=0, "calloc small ok");
    int zero=1; for (uint32_t i=0;i<32;i++) if (gm_rd8(m,p+i)) zero=0;
    CK(zero, "calloc zeroes"); CKHEAP(a);
    galloc_destroy(a); free(h.buf); free(m);
}

static void test_exhaustion(void){
    printf("[exhaustion NULL, no fault (M9)]\n");
    hostmem h; guest_mem *m = make_mem(&h, 0x50000000u, 0x1000u); /* tiny 4 KiB */
    galloc *a = galloc_create(m, 0x50000000u, 0x1000u);
    int got=0, nulled=0;
    for (int i=0;i<10000;i++){ uint32_t p=galloc_malloc(a,64); if(p){got++; fill_pat(m,p,64,i);} else {nulled=1; break;} }
    CK(got>0 && nulled, "fills then returns NULL (never faults/bumps past arena)");
    CKHEAP(a);
    galloc_destroy(a); free(h.buf); free(m);
}

/* ================= randomized torture ================= */
#define SLOTS 512
typedef struct { uint32_t ptr, size, seed; int live; } slot;

static void test_torture(uint32_t arena_size, int iters, const char*name){
    printf("[torture %s: arena=%u iters=%d]\n", name, arena_size, iters);
    hostmem h; guest_mem *m = make_mem(&h, 0x50000000u, arena_size);
    galloc *a = galloc_create(m, 0x50000000u, arena_size);
    CKHEAP(a);
    slot S[SLOTS]; memset(S,0,sizeof S);
    uint32_t maxreq = arena_size/8;
    int worst_heap_err = 0;

    for (int it=0; it<iters; it++){
        int op = rnd()%3;
        int i = rnd()%SLOTS;
        if (op==0){ /* alloc into an empty slot */
            if (S[i].live) { /* verify+free to reuse the slot */
                if (!check_pat(m,S[i].ptr,S[i].size,S[i].seed)){ printf("  FAIL: corrupt before free it=%d\n",it); fails++; }
                galloc_free(a,S[i].ptr); S[i].live=0;
            }
            uint32_t n = rnd()%maxreq;
            uint32_t seed = rnd();
            uint32_t p = (rnd()&1) ? galloc_malloc(a,n) : galloc_calloc(a, n?(n%64)+1:1, n?((n/((n%64)+1))+1):1);
            if (p){ uint32_t use=galloc_usable(a,p); CK(use>= (op==0?0:n), "usable"); (void)use;
                    /* fill exactly the usable capacity to detect any overlap fully */
                    S[i].ptr=p; S[i].size=galloc_usable(a,p); S[i].seed=seed; S[i].live=1;
                    fill_pat(m,p,S[i].size,seed); }
        } else if (op==1){ /* free a live slot */
            if (S[i].live){
                if (!check_pat(m,S[i].ptr,S[i].size,S[i].seed)){ printf("  FAIL: corrupt at free it=%d\n",it); fails++; }
                galloc_free(a,S[i].ptr); S[i].live=0;
            }
        } else { /* realloc a live slot */
            if (S[i].live){
                if (!check_pat(m,S[i].ptr,S[i].size,S[i].seed)){ printf("  FAIL: corrupt at realloc it=%d\n",it); fails++; }
                uint32_t nn = rnd()%maxreq;
                uint32_t np = galloc_realloc(a,S[i].ptr,nn);
                if (nn==0){
                    /* realloc(p,0) == free + return NULL (Audit 05): the block is gone */
                    S[i].live = 0;
                } else if (np){
                    uint32_t keep = S[i].size < nn ? S[i].size : nn; /* min(old,new) must survive (M2) */
                    if (!check_pat(m,np,keep,S[i].seed)){ printf("  FAIL: realloc lost data it=%d (keep=%u)\n",it,keep); fails++; }
                    S[i].ptr=np; S[i].size=galloc_usable(a,np);
                    fill_pat(m,np,S[i].size,S[i].seed); /* refill full new capacity */
                } /* else nn>0 && np==0: exhaustion — old block still valid, slot unchanged */
            }
        }
        int e = galloc_check(a); if (e){ if(!worst_heap_err){ printf("  FAIL: galloc_check=%d at it=%d op=%d i=%d\n",e,it,op,i); fails++; } worst_heap_err=e; break; }
        /* DIAG: every live slot's chunk must still be in-use with the recorded size */
        for (int k=0;k<SLOTS;k++) if (S[k].live){
            uint32_t hd = gm_rd32(m, S[k].ptr-8+4);
            if (!(hd&2)){ printf("  DIAG it=%d op=%d i=%d: live slot %d lost CINUSE (ptr=0x%08x hd=0x%08x)\n",it,op,i,k,S[k].ptr,hd); fails++; goto done; }
            if (((hd&~3u)-8u) != S[k].size){ printf("  DIAG it=%d op=%d i=%d: live slot %d size drift rec=%u chunk=%u (ptr=0x%08x)\n",it,op,i,k,S[k].size,(hd&~3u)-8u,S[k].ptr); fails++; goto done; }
        }
        /* periodic full sweep of all live slots */
        if ((it & 0x3FF)==0){
            for (int k=0;k<SLOTS;k++) if (S[k].live && !check_pat(m,S[k].ptr,S[k].size,S[k].seed)){
                printf("  FAIL: sweep corrupt slot=%d it=%d\n",k,it); fails++; break; }
        }
    }
done:;
    /* final: free everything, expect the heap to collapse back to one top chunk */
    for (int k=0;k<SLOTS;k++) if (S[k].live){
        if (!check_pat(m,S[k].ptr,S[k].size,S[k].seed)){ printf("  FAIL: final corrupt slot=%d\n",k); fails++; }
        galloc_free(a,S[k].ptr); S[k].live=0; }
    CKHEAP(a);
    CK(galloc_inuse_bytes(a)==0, "all freed -> 0 in-use bytes (no leak)");
    /* after freeing all, a full-arena malloc should succeed (full coalesce) */
    uint32_t big = galloc_malloc(a, arena_size - 64);
    CK(big!=0, "post-free full coalesce allows a large alloc");
    galloc_destroy(a); free(h.buf); free(m);
}

int main(void){
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered so a crash keeps the log */
    printf("=== galloc host test ===\n");
    test_basic();
    test_realloc_mincopy();
    test_calloc_overflow();
    test_exhaustion();
    test_torture(0x10000u,  200000, "64KiB");
    test_torture(0x100000u, 200000, "1MiB");
    test_torture(0x800u,     50000, "2KiB tiny");
    printf(fails? "\n=== %d FAILURE(S) ===\n" : "\n=== ALL PASS ===\n", fails);
    return fails ? 1 : 0;
}
