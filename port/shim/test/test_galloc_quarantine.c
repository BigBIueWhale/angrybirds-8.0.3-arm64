/* test_galloc_quarantine.c — does galloc_check() hold on a QUARANTINED heap?
 *
 * WHY: production enables quarantine (cpu.c:69, galloc_set_quarantine(heap, 131072)), but
 * test_galloc.c never calls galloc_set_quarantine at all — so galloc_check() has never been
 * validated against a quarantined heap, including the 450k-op torture that "proved galloc
 * clean". Meanwhile the live shim reports galloc_check == -5 (CINUSE(cur) != PINUSE(next))
 * from op 16384 onward and never recovers: 383 of 384 checks fail on an API-25 run that
 * nonetheless plays and wins a level with zero fatals.
 *
 * Two mutually exclusive explanations:
 *   (A) the guest engine really does corrupt chunk headers, and the game tolerates it; or
 *   (B) quarantine legitimately produces a state galloc_check misreads, i.e. the DIAGNOSTIC
 *       is wrong, not the heap.
 *
 * This test decides it with no engine involved: drive a healthy allocation/free workload
 * through galloc itself with quarantine ON, and ask galloc_check what it thinks. Any failure
 * here is necessarily (B), because nothing but galloc has touched the arena.
 *
 * RESULT (2026-07-27): (B) is REFUTED. All seven configurations below are clean — quarantine
 * off/small/production-ring, and every production op mix (malloc/free, realloc, calloc, >1MB
 * blocks that bypass the ring), 200k ops each under ASan+UBSan. galloc and galloc_check are
 * mutually consistent. Therefore the live shim's galloc_check == -5 is NOT a quarantine
 * artifact: in the real runtime something writes chunk headers that galloc did not write.
 * That is real guest-side corruption (the classic shape is a heap buffer overflow clobbering
 * the adjacent chunk's PINUSE bit), and it remains OPEN — see the note at the bottom.
 *
 * This test is kept as a REGRESSION GUARD, because the gap it closes was real: production runs
 * with quarantine enabled (cpu.c:69) and the entire pre-existing galloc suite ran with it off.
 *
 * Build+run (host, x86):
 *   cc -Wall -Wextra -O1 -fsanitize=address,undefined -I../src \
 *      test_galloc_quarantine.c ../src/galloc.c -o /tmp/tgq && /tmp/tgq
 */
#include "galloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint8_t *buf; uint32_t base, size; } hostmem;
static void hm_read(guest_mem*m, void*d, uint32_t a, uint32_t n){
    hostmem*h=(hostmem*)m->ctx;
    if (a < h->base || (uint64_t)a+n > (uint64_t)h->base+h->size){
        fprintf(stderr,"OOB read @0x%08x +%u\n",a,n); abort(); }
    memcpy(d, h->buf + (a - h->base), n); }
static void hm_write(guest_mem*m, uint32_t a, const void*s, uint32_t n){
    hostmem*h=(hostmem*)m->ctx;
    if (a < h->base || (uint64_t)a+n > (uint64_t)h->base+h->size){
        fprintf(stderr,"OOB write @0x%08x +%u\n",a,n); abort(); }
    memcpy(h->buf + (a - h->base), s, n); }
static uint32_t hm_rg(guest_mem*m,int i){(void)m;(void)i;return 0;}
static void     hm_sg(guest_mem*m,int i,uint32_t v){(void)m;(void)i;(void)v;}
static guest_mem *make_mem(hostmem*h, uint32_t base, uint32_t size){
    h->buf = calloc(1,size); h->base=base; h->size=size;
    guest_mem *m = calloc(1,sizeof *m);
    m->read=hm_read; m->write=hm_write; m->reg_get=hm_rg; m->reg_set=hm_sg; m->ctx=h;
    return m; }

static uint32_t rs=0x1234567u;
static uint32_t rnd(void){ uint32_t x=rs; x^=x<<13; x^=x>>17; x^=x<<5; return rs=x; }

static int fails=0;

/* Op mix flags — isolate which entry point breaks the invariant under quarantine. */
enum { OP_REALLOC=1, OP_CALLOC=2, OP_HUGE=4 };

/* Does the quarantine actually HOLD a freed block? Everything else in this file asks whether
 * galloc_check stays consistent WITH a quarantine configured; nothing asked whether the quarantine
 * does its job. That gap was found on 2026-07-28 by mutation_modules.sh: setting QUARANTINE_N to 0
 * — disabling the use-after-free protection outright — left the ENTIRE module suite passing.
 *
 * That protection is not incidental. It is the fix for the session-long std::string UAF that
 * crashed the game at level end: a block written while quarantined has a live stale pointer, so its
 * address is never reclaimed and the stale write lands harmlessly. Silently losing it would bring
 * back a crash that took a long time to find, with every test still green.
 *
 * The invariant asserted here is the weakest one that is still meaningful: while a freed block is
 * inside the quarantine window, a subsequent same-size allocation must NOT be handed that address
 * back. With the window at 0 the allocator reuses it immediately and this fails. */
static void t_quarantine_holds(void){
    const uint32_t BASE=0x50000000u, ARENA=8u*1024*1024;
    hostmem hm; guest_mem *m = make_mem(&hm, BASE, ARENA);
    galloc *a = galloc_create(m, BASE, ARENA);
    galloc_set_quarantine(a, 4096);
    printf("[quarantine actually holds freed blocks]\n");

    uint32_t p = galloc_malloc(a, 64);
    if (!p){ printf("  FAIL: initial malloc\n"); fails++; return; }
    galloc_free(a, p);
    int reused = 0;
    for (int i = 0; i < 64; i++){                 /* well inside a 4096-op window */
        uint32_t q = galloc_malloc(a, 64);
        if (q == p){ reused = 1; break; }
    }
    if (reused){
        printf("  FAIL: a freed block was handed back while quarantined — UAF protection is OFF\n");
        fails++;
    } else {
        printf("  ok: freed block withheld across 64 same-size allocations\n");
    }

    /* And the complement: with NO quarantine the address is expected to come straight back, so the
     * check above is testing the quarantine rather than some unrelated allocator habit. */
    hostmem hm2; guest_mem *m2 = make_mem(&hm2, BASE, ARENA);
    galloc *b = galloc_create(m2, BASE, ARENA);
    uint32_t r = galloc_malloc(b, 64);
    galloc_free(b, r);
    uint32_t r2 = galloc_malloc(b, 64);
    if (r2 != r) printf("  note: unquarantined heap did not reuse immediately (allocator policy)\n");
    else         printf("  ok: unquarantined heap reuses at once, so the test above is quarantine-specific\n");

    /* Both arenas must be released. This file is compiled with ASan and -fno-sanitize-recover, so a
     * leak is a hard failure — the first version of this test leaked 22 MB across the two 8 MB
     * arenas and broke the suite. Mirrors run_mix's teardown below. */
    galloc_destroy(a); free(hm.buf);  free(m);
    galloc_destroy(b); free(hm2.buf); free(m2);
}

static void run_mix(const char *name, uint32_t qn, unsigned long nops, int mix){
    const uint32_t BASE=0x50000000u, ARENA=192u*1024*1024;
    hostmem hm; guest_mem *m = make_mem(&hm, BASE, ARENA);
    galloc *a = galloc_create(m, BASE, ARENA);
    if (qn) galloc_set_quarantine(a, qn);
    enum { LIVE=2048 };
    uint32_t live[LIVE]; memset(live,0,sizeof live);
    unsigned long first_bad=0; int first_rc=0, bad=0, checked=0;
    for (unsigned long op=1; op<=nops; op++){
        uint32_t i = rnd() % LIVE;
        uint32_t roll = rnd() % 100;
        if (live[i] && (mix & OP_REALLOC) && roll < 25){
            uint32_t ns = 8 + rnd()%2048;
            uint32_t np = galloc_realloc(a, live[i], ns);
            if (np) live[i]=np;
        } else if (live[i]){
            galloc_free(a, live[i]); live[i]=0;
        } else {
            uint32_t sz;
            if ((mix & OP_HUGE) && roll < 2) sz = 0x100000u + (rnd()%65536); /* bypasses ring */
            else sz = (roll < 80) ? (8 + rnd()%56) : (64 + rnd()%960);
            live[i] = (mix & OP_CALLOC) && (roll & 1) ? galloc_calloc(a, 1, sz) : galloc_malloc(a, sz);
        }
        if ((op & 0x3ffUL)==0){
            checked++;
            int rc = galloc_check(a);
            if (rc){ bad++; if(!first_bad){ first_bad=op; first_rc=rc; } }
        }
    }
    int final_rc = galloc_check(a);
    printf("  %-34s qn=%-7u bad=%-5d/%-5d first_bad_op=%-8lu first_rc=%-3d final_rc=%d%s\n",
           name, qn, bad, checked, first_bad, first_rc, final_rc,
           (bad||final_rc) ? "   <== FAIL" : "");
    if (bad || final_rc) fails++;
    galloc_destroy(a); free(hm.buf); free(m);
}

/* Run a healthy alloc/free workload and report the FIRST op at which galloc_check breaks. */
static void run_case(const char *name, uint32_t qn, unsigned long nops){
    const uint32_t BASE=0x50000000u, ARENA=64u*1024*1024;
    hostmem hm; guest_mem *m = make_mem(&hm, BASE, ARENA);
    galloc *a = galloc_create(m, BASE, ARENA);
    if (qn) galloc_set_quarantine(a, qn);

    enum { LIVE=4096 };
    uint32_t live[LIVE]; memset(live,0,sizeof live);
    unsigned long first_bad=0; int first_rc=0, bad=0, checked=0;

    for (unsigned long op=1; op<=nops; op++){
        uint32_t i = rnd() % LIVE;
        if (live[i]){ galloc_free(a, live[i]); live[i]=0; }
        else {
            uint32_t sz = (rnd()%100 < 80) ? (8 + rnd()%56) : (64 + rnd()%960);
            live[i] = galloc_malloc(a, sz);
        }
        if ((op & 0x3ffUL)==0){                       /* sample like the shim does */
            checked++;
            int rc = galloc_check(a);
            if (rc){ bad++; if(!first_bad){ first_bad=op; first_rc=rc; } }
        }
    }
    int final_rc = galloc_check(a);
    printf("  %-34s qn=%-7u ops=%-8lu checks=%-5d bad=%-5d first_bad_op=%-8lu first_rc=%-3d final_rc=%d\n",
           name, qn, nops, checked, bad, first_bad, first_rc, final_rc);
    if (bad || final_rc){ printf("    FAIL: galloc_check must hold for this configuration\n"); fails++; }
    galloc_destroy(a); free(hm.buf); free(m);
}

int main(void){
    t_quarantine_holds();

    printf("galloc_check() on a QUARANTINED heap — nothing but galloc touches the arena,\n"
           "so ANY failure below is a diagnostic defect, not guest corruption.\n\n");

    run_case("baseline: quarantine OFF",            0,      200000);
    run_case("quarantine ON, small ring",           64,     200000);
    run_case("quarantine ON, production ring",      131072, 200000);

    printf("\n  --- isolating the operation mix (production ring) ---\n");
    run_mix("malloc/free only",              131072, 200000, 0);
    run_mix("+ realloc",                     131072, 200000, OP_REALLOC);
    run_mix("+ calloc",                      131072, 200000, OP_CALLOC);
    run_mix("+ huge (>1MB, bypasses ring)",  131072, 200000, OP_HUGE);
    run_mix("+ ALL (production-like)",       131072, 200000, OP_REALLOC|OP_CALLOC|OP_HUGE);
    printf("\n  --- same mixes with quarantine OFF (control) ---\n");
    run_mix("realloc, quarantine OFF",       0,      200000, OP_REALLOC);
    run_mix("ALL, quarantine OFF",           0,      200000, OP_REALLOC|OP_CALLOC|OP_HUGE);

    printf("\n");
    if (fails){
        printf("FAILED: %d configuration(s) broke galloc_check with nothing but galloc touching\n"
               "the arena. That is an allocator/diagnostic defect and must be fixed here.\n", fails);
        return 1;
    }
    printf("ALL PASS — galloc and galloc_check are mutually consistent under quarantine for\n"
           "every production op mix. NOTE: the LIVE shim still reports galloc_check == -5 from\n"
           "op 16384 onward (383/384 checks on an API-25 run, never recovering). Since this test\n"
           "rules out the allocator and the quarantine, that failure means something in the real\n"
           "runtime writes chunk headers galloc did not write. OPEN — do not close it by\n"
           "weakening this test.\n");
    return 0;
}
