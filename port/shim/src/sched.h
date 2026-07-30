/* sched.h — the guest-thread scheduler (device layer, Audit 02 concretized + Audit 09 S2).
 *
 * MODEL (opinionated, single mode): the GEL (Global Emulator Lock) green-thread model.
 *   - ONE uc_engine, ONE shared guest address space.
 *   - Every guest thread is a `gthread` carrying its own saved uc_context (CPU+VFP).
 *   - The GEL (one host mutex) means ONLY ONE guest thread executes guest code at a
 *     time, so the engine's ~28k inline ldrex/strex atomics are correct BY
 *     CONSTRUCTION (the ARM exclusive monitor is never raced). Multi-engine was
 *     rejected precisely because that monitor is per-engine.
 *   - Guest threads are multiplexed onto whichever HOST thread currently holds the
 *     GEL (a Java thread inside a JNI call, or another during a deadlock-defense
 *     handoff). A pthread_create'd engine thread has NO host thread of its own; it
 *     runs when the scheduler picks it. State lives in uc_context, so a green thread
 *     may run on different host threads across time-slices — correct, because all its
 *     per-thread state (TPIDRURO, errno, TLS keys) is GUEST-side and reloaded on switch.
 *   - Preemption for liveness (spin/yield loops): guest runs in bounded uc_emu_start
 *     instruction-count QUANTA; on quantum expiry the scheduler may switch.
 *   - Blocking (mutex/cond/join/rwlock/once): the PROVEN longjmp mechanism — the
 *     primitive stashes resume_pc = guest LR, forces engine LR = RG_RET so the stub's
 *     own `bx lr` returns to RG_RET (leak-free; NOT uc_emu_stop, which leaks insns),
 *     the run-loop then saves ctx + parks the gthread; on wake it restores ctx, sets
 *     PC = resume_pc, injects r0 = result. Same mechanism as dispatch.c h_longjmp.
 *   - Real host-backed guest primitives keyed by the guest object address.
 *
 * The scheduler runs entirely on the GEL-holding host thread; the GEL itself
 * serializes ALL scheduler-state access (no second lock), except the deadlock-defense
 * wait, which releases the GEL on a condvar so another host thread can make progress.
 */
#ifndef ABSHIM_SCHED_H
#define ABSHIM_SCHED_H
#include <unicorn/unicorn.h>
#include <pthread.h>
#include <stdint.h>
#include "cpu.h"

#define SCHED_MAX_THREADS 64
#define SCHED_MAX_KEYS    128
/* SCHED_SLICE_NS replaces SCHED_QUANTUM as the actual preemption bound (see dispatch_arm_slice()).
 * 16 ms is chosen to match what the old 200000-instruction count worked out to in practice: at the
 * measured on-device rate of ~11 Minsn/s, 200000 insns is ~18 ms. So responsiveness is unchanged
 * while the x1.56 instruction-counting penalty is dropped. SCHED_QUANTUM is kept because
 * sched_once()/nested starts still use a bounded count, where the cost is irrelevant. */
#define SCHED_SLICE_NS    16000000ull      /* 16 ms wall-clock slice, bridge-boundary preemption */
void dispatch_arm_slice(uc_engine *uc, uint64_t ns_from_now);   /* dispatch.c */
#define SCHED_QUANTUM     200000u          /* guest insns per time-slice (preemption). Kept at 200000: a 20M test
                                            * showed the gameplay fatal is DETERMINISTIC (same THEME_23 level-load
                                            * spot, pc=0x10000050, regardless of quantum) — NOT a preemption race, so
                                            * reducing preemption doesn't help it AND a large quantum risks choppy
                                            * rendering on the slower on-device Unicorn (a thread could hold ~seconds
                                            * before a preempt). The gameplay fatal is the registry-BadType UAF path. */
#define SCHED_WORKER_STACK 0x00200000u     /* 2 MiB per engine thread */
#define SCHED_MAIN_STACK   0x00800000u     /* 8 MiB for the main/Java-entry thread (Android default).
                                            * NB: bumping this does NOT fix draws=0 — the scene ctor
                                            * 0x6f370's tree-walk (0x7d2d18/0x7c53fc) recurses INFINITELY
                                            * (fills any stack: MINSP=stack_lo-0x1768 at 8MB AND 32MB) on
                                            * a CYCLIC/corrupted scene tree; the fix is the tree cycle, not
                                            * the stack size. */
#define SCHED_TCB_SZ       0x00001000u     /* one guest page per thread (errno+guard) */

enum { GT_UNUSED=0, GT_NEW, GT_RUNNABLE, GT_RUNNING, GT_BLOCKED, GT_IDLE, GT_DEAD };
enum { WK_NONE=0, WK_MUTEX, WK_COND, WK_JOIN, WK_RWLOCK_RD, WK_RWLOCK_WR, WK_ONCE, WK_SLEEP };

typedef struct gthread {
    uint32_t   id;                 /* pthread_t token the guest sees (1-based, != 0) */
    int        state;
    uc_context*ctx;                /* saved CPU+VFP; valid whenever state != RUNNING  */
    uint32_t   resume_pc;          /* guest addr to resume at after block/yield/start */
    int        inject_r0;          /* inject inject_val into r0 on resume             */
    uint32_t   inject_val;
    int        pending_block;      /* a primitive requested a block this quantum      */
    /* blocking JNI Call*Method (S2 stop/restart): env_dispatch_real stashes the call
     * (params in jni_entry.c's per-slot side table, keyed by this gthread's index) and
     * redirects the guest to RG_RET; run_loop then does the real (blocking) ART call with
     * the GEL RELEASED so other green threads run — fixes the audio/render Call* deadlock. */
    int        pending_jni;        /* a blocking ART Call*Method is queued this quantum */
    uint32_t   jni_ret_pc;         /* guest addr to resume at after the ART call        */

    uint32_t   stack_lo, stack_hi; /* this thread's stack slice within RG_STACK       */
    uint32_t   tcb;                /* guest TCB page base (TPIDRURO) inside RG_TCB     */
    uint32_t   errno_addr;         /* per-thread errno word (guest addr)              */
    uint32_t   tls[SCHED_MAX_KEYS];/* pthread_getspecific store                        */

    uint32_t   start_fn, start_arg;/* engine-thread entry                              */
    int        is_engine;          /* pthread_create'd (green) vs Java-entry           */

    int        started;            /* engine thread has had its entry regs initialised */
    uint32_t   retval;             /* void* the thread returned (pthread_exit/return)  */
    int        detached, reapable; /* detach state; reapable=exited & unjoined         */
    struct gthread *joiner;        /* a thread BLOCKED in join on me (one, per POSIX)  */
    uint32_t   join_retptr;        /* where THIS thread (as joiner) wants the retval    */

    struct gthread *qnext;         /* intrusive link: runqueue OR a wait-queue         */
    void      *wait_obj;           /* sync object I'm blocked on                        */
    int        wait_kind;          /* WK_*                                             */
    uint32_t   wait_mutex;         /* cond_wait: guest mutex to re-acquire on wake     */
    uint64_t   deadline_ns;        /* timed wait absolute monotonic deadline; 0=none   */
    int        timed_out;          /* set when a timed wait fired                       */

    /* pthread_once call-injection (avoids a re-entrant nested uc_emu_start): sched_once
     * redirects THIS gthread to run the init routine as its own flow, then resumes the
     * caller. once_inject_pc!=0 => run_loop must (re)start the routine with LR=RG_RET.
     * once_obj[]/once_ret[] are a LIFO stack (a once-routine may itself call pthread_once):
     * push on inject, and when the routine returns to RG_RET, pop to complete that once
     * (mark done + wake waiters) and resume its caller (once_ret) with r0=0. */
    uint32_t   once_inject_pc;
    void      *once_obj[8];
    uint32_t   once_ret[8];
    int        once_depth;

    pthread_t  host; int host_valid; /* Java host thread bound to this gthread          */
    char       name[24];
} gthread;

/* sync-object hash node (keyed by guest address) — defined in sched.c */
typedef struct sobj sobj;

typedef struct sched {
    cpu_t          *cpu;
    pthread_mutex_t gel;                    /* the Global Emulator Lock */
    pthread_cond_t  gel_cv;                 /* runnable-set changed / GEL free */
    int             gel_init;

    gthread        *cur;                    /* the RUNNING gthread (NULL when idle) */
    gthread        *runq_head, *runq_tail;  /* RUNNABLE FIFO */
    gthread         all[SCHED_MAX_THREADS];
    int             nthreads;
    uint32_t        next_id;

    /* per-thread stacks + TCB pages are assigned deterministically by slot index
     * (assign_stack_tcb): recycled slots reuse the same regions, so create/join churn
     * never leaks the carve — the old bump-pointer scheme died after ~123 creates (H2). */

    sobj           *obj[256];               /* sync-object hash buckets */
    uint32_t        key_next;               /* pthread_key_create counter (1-based) */
    int             key_used[SCHED_MAX_KEYS];
    uint32_t        key_destr[SCHED_MAX_KEYS];

    int             fatal; char fatal_msg[64];
    const volatile int *fatal_ext;           /* M3: -> dispatch d->fatal (abort/__stack_chk_fail) */

    /* blocking-JNI stop/restart (S2). drive_active=1 only while run_loop is the active
     * driver (via sched_enter/sched_call) — env_dispatch_real gates the stop/restart on
     * it so a non-sched cpu_call (guest JNI_OnLoad) still does the inline ART call.
     * The two callbacks are set by jni_entry (device); sched.c has no JNIEnv, so it calls
     * them across the GEL boundary: jni_do_call runs the ART call with the GEL RELEASED
     * and stashes the raw result; jni_finalize tokenises + writes r0/r1 with the GEL HELD. */
    int             drive_active;
    void          (*jni_do_call)(gthread *gt);
    void          (*jni_finalize)(gthread *gt);
} sched;

/* lifecycle */
int      sched_init(sched *S, cpu_t *cpu);
void     sched_destroy(sched *S);
/* the gthread bound to the CURRENT host thread (created on first use). Java entry. */
gthread *sched_host_gthread(sched *S);
/* run `gt`'s guest call from pc until it returns to RG_RET, time-slicing every
 * runnable gthread. Acquires+releases the GEL. Returns uc_err (fatal on != OK). */
uc_err   sched_enter(sched *S, gthread *gt, uint32_t pc, uint64_t budget);
/* framed entry: sets SP=gt stack top, LR=RG_RET, r0..r3=a0..a3 under the GEL. */
uc_err   sched_call (sched *S, gthread *gt, uint32_t addr, uint32_t a0,uint32_t a1,uint32_t a2,uint32_t a3);
gthread *sched_current(sched *S);
void     sched_dump_state(sched *S);   /* diagnostic: log all live green-thread states */

/* primitives — called from dispatch handlers; operate on S->cur. Ones marked (may
 * block) set up the resume/park and return the NON-blocking result; the run-loop
 * injects the real r0 on wake. All return the guest-visible return value. */
int      sched_create   (sched *S, uint32_t start_fn, uint32_t arg, uint32_t *out_tid);
int      sched_join     (sched *S, uint32_t tid, uint32_t retval_ptr);   /* may block */
int      sched_detach   (sched *S, uint32_t tid);
void     sched_exit     (sched *S, uint32_t retval);                     /* current returns */
uint32_t sched_self     (sched *S);
void     sched_yield_now(sched *S);                                      /* may switch */

int      sched_mutex_lock  (sched *S, uint32_t m, int trylock);          /* may block */
int      sched_mutex_unlock(sched *S, uint32_t m);
int      sched_cond_wait   (sched *S, uint32_t c, uint32_t m, uint64_t deadline_ns); /* blocks */
int      sched_sleep       (sched *S, uint64_t duration_ns);             /* pure timed block (poll/nanosleep) */
int      sched_cond_wake   (sched *S, uint32_t c, int broadcast);
int      sched_rwlock_lock (sched *S, uint32_t rw, int write, int trylock); /* may block */
int      sched_rwlock_unlock(sched *S, uint32_t rw);
int      sched_once        (sched *S, uint32_t once, uint32_t routine);   /* may block */

uint32_t sched_key_create  (sched *S, uint32_t destructor);              /* -> key id (0=fail) */
int      sched_key_delete  (sched *S, uint32_t key);
uint32_t sched_getspecific (sched *S, uint32_t key);
int      sched_setspecific (sched *S, uint32_t key, uint32_t val);
uint32_t sched_errno_addr  (sched *S);                                   /* per-thread errno */
extern sched *g_sched;                                                   /* live scheduler (bridges set current-thread errno) */

#endif /* ABSHIM_SCHED_H */
