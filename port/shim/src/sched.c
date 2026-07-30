/* sched.c — the GEL green-thread guest scheduler. See sched.h for the model. */
#include "sched.h"
#include "regions.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#if defined(__ANDROID__)
#include <android/log.h>
#define SLOG(...) __android_log_print(4,"abshim",__VA_ARGS__)   /* DIAG: unconditional logcat (SDBG->stderr is invisible on Android) */
#else
#define SLOG(...) fprintf(stderr,__VA_ARGS__)                    /* host test build: no liblog */
#endif
/* DIAG: monotonically bumped once per run_loop quantum. A separate watchdog thread (jni_entry.c)
 * samples it: FROZEN => the guest is stuck INSIDE one uc_emu_start (a blocking bridge/JNI call);
 * ADVANCING => a slow run_loop spin (the lowered [sched-sample] heartbeat then pins the PC). */
volatile unsigned long g_sched_tick = 0;

/* ---- errno-ish return codes the guest expects (bionic values) ---- */
#define E_PERM 1
#define E_SRCH 3
#define E_DEADLK 35
#define E_BUSY 16
#define E_INVAL 22
#define E_TIMEDOUT 110

enum { SOBJ_MUTEX=1, SOBJ_COND, SOBJ_RWLOCK, SOBJ_ONCE };

struct sobj {
    uint32_t addr; int kind;
    /* mutex — owner is a thread ID, not a gthread* (M4: ids are monotonic and never
     * reused, so a recycled thread slot can't alias a stale owner and spuriously pass
     * the recursive-lock check; a pointer could). 0 = free. */
    uint32_t owner_id; int count;
    /* rwlock */
    int readers; uint32_t writer_id;
    /* once: 0 not-run, 1 running, 2 done */
    int once_state; uint32_t once_runner_id;
    /* waiter lists (intrusive via gthread.qnext); rwaiters = rwlock readers */
    gthread *waiters, *rwaiters;
    struct sobj *hnext;
};

static uint64_t now_ns(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec*1000000000ull + (uint64_t)ts.tv_nsec;
}
static uint32_t rd(sched*S,int id){ uint32_t v=0; uc_reg_read(S->cpu->uc,id,&v); return v; }
static void     wr(sched*S,int id,uint32_t v){ uc_reg_write(S->cpu->uc,id,&v); }

/* ------------------------------------------------------------------ hashing */
static struct sobj *getobj(sched*S, uint32_t addr, int kind){
    unsigned b=(addr>>4)&255u;
    for(struct sobj*o=S->obj[b]; o; o=o->hnext) if(o->addr==addr) return o;
    struct sobj*o=(struct sobj*)calloc(1,sizeof *o);
    /* Unchecked before: the very next line dereferenced this. Every guest mutex/cond/rwlock
     * operation allocates through here on first use, so an OOM meant a NULL deref rather than a
     * failed lock. Callers now propagate the failure - see the !mx / !cv / !o guards below. */
    if(!o) return NULL;
    o->addr=addr; o->kind=kind; o->hnext=S->obj[b]; S->obj[b]=o; return o;
}
static void __attribute__((unused)) destroyobj(sched*S, uint32_t addr){  /* wired when mutex_destroy is */
    unsigned b=(addr>>4)&255u; struct sobj**pp=&S->obj[b];
    while(*pp){ if((*pp)->addr==addr){ struct sobj*d=*pp; *pp=d->hnext; free(d); return; } pp=&(*pp)->hnext; }
}

/* ------------------------------------------------------------- queue helpers */
static void runq_push(sched*S, gthread*g){
    g->qnext=NULL; g->state=GT_RUNNABLE;
    if(S->runq_tail) S->runq_tail->qnext=g; else S->runq_head=g;
    S->runq_tail=g;
}
static gthread *runq_pop(sched*S){
    gthread*g=S->runq_head; if(!g) return NULL;
    S->runq_head=g->qnext; if(!S->runq_head) S->runq_tail=NULL;
    g->qnext=NULL; return g;
}
static void wq_push(gthread**head, gthread*g){
    g->qnext=NULL; if(!*head){ *head=g; return; }
    gthread*t=*head; while(t->qnext) t=t->qnext; t->qnext=g;
}
static gthread *wq_pop(gthread**head){
    gthread*g=*head; if(g){ *head=g->qnext; g->qnext=NULL; } return g;
}
static void wq_remove(gthread**head, gthread*g){
    while(*head){ if(*head==g){ *head=g->qnext; g->qnext=NULL; return; } head=&(*head)->qnext; }
}

/* ------------------------------------------------------------- thread lookup */
static gthread *by_id(sched*S, uint32_t id){
    if(!id) return NULL;
    for(int i=0;i<S->nthreads;i++) if(S->all[i].state!=GT_UNUSED && S->all[i].id==id) return &S->all[i];
    return NULL;
}
static gthread *alloc_gt(sched*S){
    for(int i=0;i<S->nthreads;i++) if(S->all[i].state==GT_UNUSED){ gthread*g=&S->all[i]; memset(g,0,sizeof *g); return g; }
    if(S->nthreads>=SCHED_MAX_THREADS) return NULL;
    gthread*g=&S->all[S->nthreads++]; memset(g,0,sizeof *g); return g;
}

/* H2: assign a thread's stack slice + TCB page DETERMINISTICALLY by slot index, so a
 * recycled slot reuses the same regions and create/join churn never leaks the carve.
 * Slot 0 (the main host thread) gets the 8 MiB top stack; every other slot a 2 MiB slice
 * below it. (M6 guard pages were tried here but reverted: punching per-thread PROT_NONE
 * pages into the single RG_STACK mapping fragments it and badly slows TCG emulation —
 * the wrong tradeoff for defence against a guest overflow that shouldn't occur. Doing it
 * right needs per-thread SEPARATE mappings with gaps; not worth it for the shipping shim.) */
static void assign_stack_tcb(sched*S, gthread*g){
    uint32_t idx=(uint32_t)(g - S->all);
    if(idx==0){ g->stack_hi=RG_STACK+RG_STACK_SZ-0x1000u; g->stack_lo=g->stack_hi-SCHED_MAIN_STACK; }
    else { g->stack_hi=(RG_STACK+RG_STACK_SZ-0x1000u-SCHED_MAIN_STACK)-(idx-1)*SCHED_WORKER_STACK;
           g->stack_lo=g->stack_hi-SCHED_WORKER_STACK; }
    g->tcb=RG_TCB+idx*SCHED_TCB_SZ; g->errno_addr=g->tcb;
    gm_wr32(&S->cpu->mem, g->errno_addr, 0);
}

/* --------------------------------------------------------- context handling */
/* Clear the ARM exclusive monitor on a green-thread RESUME (real OSes CLREX on context switch).
 * If a thread is preempted by the time-slice BETWEEN an LDREX and its STREX, the monitor must not
 * survive into the switch and let the later STREX spuriously succeed against a stale/foreign
 * reservation -> refcount/atomic corruption -> use-after-free (the cont.74 _Rep corruption). Unicorn
 * exposes no monitor register, so we execute a real CLREX (planted at RG_KUSER) via a 1-instruction
 * nested uc_emu_start. CLREX touches no GP regs; PC is re-set by run_loop's uc_emu_start(resume_pc);
 * we save/restore CPSR so the tiny ARM-mode run can't perturb the resumed thread's flags/mode. */
static void clrex_on_switch(sched*S){
    uint32_t cpsr=0; uc_reg_read(S->cpu->uc, UC_ARM_REG_CPSR, &cpsr);
    uc_emu_start(S->cpu->uc, RG_KUSER, RG_KUSER+4u, 0, 1);
    uc_reg_write(S->cpu->uc, UC_ARM_REG_CPSR, &cpsr);
}
static void ctx_switch_in(sched*S, gthread*g){
    S->cur=g; g->state=GT_RUNNING;
    if(g->started){
        uc_context_restore(S->cpu->uc, g->ctx);
        clrex_on_switch(S);
    } else if(g->is_engine){
        /* first schedule of a pthread_create'd thread: build its entry frame */
        wr(S, UC_ARM_REG_SP, g->stack_hi & ~15u);
        wr(S, UC_ARM_REG_LR, RG_RET);              /* return-from-start -> RG_RET -> exit */
        wr(S, UC_ARM_REG_R0, g->start_arg);
        g->resume_pc = g->start_fn;
        g->started = 1;
    }
    wr(S, UC_ARM_REG_C13_C0_3, g->tcb);            /* TPIDRURO (defensive per-thread TLS) */
    if(g->inject_r0){ wr(S, UC_ARM_REG_R0, g->inject_val); g->inject_r0=0; }
}

/* the running thread requested a block: stash the guest resume point and force
 * uc_emu_start to unwind to RG_RET via the stub's own `bx lr` (proven, leak-free). */
static void arm_block(sched*S){
    gthread*g=S->cur;
    g->resume_pc = rd(S, UC_ARM_REG_LR);           /* guest return addr = where to resume */
    wr(S, UC_ARM_REG_LR, RG_RET);
    g->pending_block = 1;
}

/* ------------------------------------------------------------ mutex internals */
static void grant_mutex(sched*S, struct sobj*mx, gthread*w){
    mx->owner_id=w->id; mx->count=1; w->wait_kind=WK_NONE; w->wait_obj=NULL;
    if(!w->inject_r0){ w->inject_val=0; w->inject_r0=1; }  /* preserve a cond result if set */
    runq_push(S,w);
}
static void mutex_release(sched*S, uint32_t m){
    struct sobj*mx=getobj(S,m,SOBJ_MUTEX);
    if(!mx) return;
    gthread*w=wq_pop(&mx->waiters);
    if(w) grant_mutex(S,mx,w);
    else { mx->owner_id=0; mx->count=0; }
}

/* ------------------------------------------------------------- cond internals */
static void cond_release_one(sched*S, struct sobj*cv, gthread*w, uint32_t result){
    (void)cv;   /* caller already removed w from cv's queue */
    /* w is leaving the cond wait (signalled or timed out); re-acquire its mutex */
    w->inject_val=result; w->inject_r0=1; w->wait_obj=NULL;
    struct sobj*mx=getobj(S,w->wait_mutex,SOBJ_MUTEX);
    /* OOM re-acquiring the mutex on wake: leave the thread runnable rather than deref NULL. It
     * proceeds without the lock, which is wrong but survivable; crashing here is neither. */
    if(!mx){ w->wait_kind=WK_NONE; return; }
    if(mx->owner_id==0) grant_mutex(S,mx,w);
    else { w->wait_kind=WK_MUTEX; w->wait_obj=mx; wq_push(&mx->waiters,w); }
}

/* a BLOCKED cond-waiter's (or pure sleeper's) timed deadline elapsed */
static void timeout_fire(sched*S, gthread*g){
    g->timed_out=1; g->deadline_ns=0;
    if(g->wait_kind==WK_SLEEP || g->wait_obj==NULL){   /* pure sched_sleep: no cond/mutex, just wake with r0=0 */
        g->wait_kind=WK_NONE; g->inject_val=0; g->inject_r0=1; runq_push(S,g); return;
    }
    struct sobj*cv=(struct sobj*)g->wait_obj;
    wq_remove(&cv->waiters, g);
    cond_release_one(S, cv, g, E_TIMEDOUT);
}

/* -------------------------------------------------------- thread termination */
static void thread_exit(sched*S, gthread*g, uint32_t retval){
    g->retval=retval;
    if(g->joiner){
        gthread*j=g->joiner;
        if(j->join_retptr) gm_wr32(&S->cpu->mem, j->join_retptr, retval);
        j->wait_kind=WK_NONE; j->wait_obj=NULL; j->inject_val=0; j->inject_r0=1;
        runq_push(S,j);
        if(g->ctx){ uc_context_free(g->ctx); g->ctx=NULL; }
        g->state=GT_UNUSED;                       /* joiner consumed us */
    } else if(g->detached){
        if(g->ctx){ uc_context_free(g->ctx); g->ctx=NULL; }
        g->state=GT_UNUSED;
    } else {
        g->state=GT_DEAD; g->reapable=1;          /* await a future join */
    }
}

/* --------------------------------------------------------------- idle waiting */
/* nothing runnable and cur is blocked/exited: service timed waits, else release
 * the GEL and wait for another host thread to make something runnable. */
static gthread *idle_wait_pick(sched*S){
    for(;;){
        uint64_t now=now_ns(); uint64_t earliest=0; int have=0;
        { static uint64_t s_last=0; if(now - s_last > 8000000000ull){ s_last=now; sched_dump_state(S); } }  /* DIAG: pin stalls (rate-limited 8s) */
        for(int i=0;i<S->nthreads;i++){
            gthread*g=&S->all[i];
            if(g->state==GT_BLOCKED && g->deadline_ns){
                if(g->deadline_ns<=now) timeout_fire(S,g);
                else if(!have || g->deadline_ns<earliest){ earliest=g->deadline_ns; have=1; }
            }
        }
        gthread*nx=runq_pop(S); if(nx) return nx;
        if(have){
            /* a timed waiter is pending: sleep (releasing the GEL) until its deadline,
             * then loop to fire it. gel_cv is a MONOTONIC-clock cond (M1) so this absolute
             * time is in the right clock domain. */
            struct timespec ts; ts.tv_sec=(time_t)(earliest/1000000000ull); ts.tv_nsec=(long)(earliest%1000000000ull);
            pthread_cond_timedwait(&S->gel_cv, &S->gel, &ts);
        } else {
            /* C1/M7: no runnable thread AND no timed waiter. Under the coarse BEL exactly
             * one host thread is ever inside the scheduler, so nothing can ever make a
             * thread runnable again — a genuine guest deadlock. Fail loudly instead of
             * releasing the GEL to wait for a signal that can never come (the old
             * pthread_cond_wait here hung the process forever). */
            S->fatal=1; snprintf(S->fatal_msg,sizeof S->fatal_msg,"deadlock: all guest threads blocked, none runnable");
            return NULL;
        }
    }
}

/* ------------------------------------------------------------------ run-loop */
/* the scheduler core: assumes the GEL is held and S->cur is set up. `gt` is the
 * ENTRY gthread whose return-to-RG_RET completes this call. Runs until then. */
static int SDBG=-1;
static uc_err run_loop(sched *S, gthread *gt){
    if(SDBG<0){ SDBG = getenv("SCHED_DBG")?1:0; }
    uc_err e=UC_ERR_OK;
    long guard=0;
    for(;;){
        gthread*r=S->cur;
        uint32_t begin=r->resume_pc;
        if(SDBG && guard<400) fprintf(stderr,"[loop %ld] cur=%u begin=0x%x inj=%d wk=%d runq=%s\n",
            guard, r->id, begin, r->inject_r0, r->wait_kind, S->runq_head?"Y":"n");
        if(++guard>50000000L){ fprintf(stderr,"[SCHED] runaway loop guard tripped\n"); S->fatal=1; break; }
        g_sched_tick++;          /* DIAG: watchdog progress heartbeat (frozen=>blocked in a bridge/JNI call; advancing=>spin) */
        if((guard%500L)==0L)     /* DIAG: periodic PC sample -> locate a slow spin (lowered 20000->500: fires ~15s into a stall) */
            SLOG("[sched-sample] q=%ld cur=%u begin=+0x%x wk=%d runq=%s nthreads=%d",
                 guard, r->id, begin>=RG_ENGINE?begin-RG_ENGINE:begin, r->wait_kind, S->runq_head?"Y":"n", S->nthreads);
        if(r->inject_r0){ wr(S,UC_ARM_REG_R0,r->inject_val); r->inject_r0=0; }
        /* The GEL stays HELD across uc_emu_start -> exactly one host thread ever drives
         * the shared engine. It is released only inside idle_wait_pick (cond_wait), when
         * this thread is blocked with nothing runnable and yields to another host thread. */
        /* count=0, NOT SCHED_QUANTUM: a non-zero count makes Unicorn instrument every instruction
         * and lose TCG block chaining, measured at x1.56 on identical work. The slice is bounded by
         * wall clock at a bridge boundary instead -- see dispatch_arm_slice(). Armed immediately
         * before and disarmed immediately after, on this same host thread, so a stale deadline can
         * never truncate the following slice. */
        /* ADAPTIVE SLICE. count=0 is ~1.56x faster (measured) but can only be ended by the
         * bridge-boundary check, so it is used ONLY for a thread that demonstrably reached a bridge in
         * its previous slice. A thread that reached none keeps the bounded count, so it can always be
         * preempted. Safe by default: slice_fast is zero-initialised.
         *
         * There is deliberately NO asynchronous stopper. An earlier version had a timer thread calling
         * uc_emu_stop, and it produced a different fault in a different subsystem on every attempt
         * (St9bad_alloc, then gr::GraphicsException, then St12length_error) because uc_emu_stop makes a
         * hook callback re-fire and the shim's callbacks are the bridge/JNI implementations. Disabling
         * it made the same playthrough clean and winning. Guarding callback families was a whitelist
         * against a hazard that is the default; this removes the hazard instead. */
        unsigned long _b0 = abshim_bridge_calls;
        uint64_t _cnt = r->slice_fast ? 0ull : (uint64_t)SCHED_QUANTUM;
        /* Use uc_emu_start's OWN `timeout` (microseconds) rather than a hand-rolled timer thread.
         * The header documents it as "duration to emulate the code (in microseconds)", so Unicorn
         * bounds the slice itself. Whether its internal stopper is safer than mine is an empirical
         * question -- my timer produced a fault in a different subsystem on every attempt -- and this
         * isolates it: same count=0 fast path, same 16 ms bound, Unicorn's mechanism instead of mine.
         * The synchronous bridge-boundary check stays armed as a second, provably-safe bound. */
        dispatch_arm_slice(S->cpu->uc, _cnt ? 0ull : SCHED_SLICE_NS);
        /* TIMEOUT MUST BE 0. Unicorn's own `timeout` parameter bounds the slice asynchronously, and
         * MEASURED it hits full speed (59.99 fps play-phase, card at ~45 s) while reproducing exactly
         * the fault my own timer thread produced: THROW St9bad_alloc, h_fatal=1.
         *
         * That is the decisive result: the hazard is INHERENT to asynchronous stopping in this shim,
         * not a defect in my stopper. Unicorn's supported mechanism triggers it too, because this shim
         * puts NON-IDEMPOTENT work inside UC_HOOK_CODE callbacks -- malloc/free, real JNI calls into
         * ART, guest syscalls -- and any async stop can make a callback re-fire (proven separately).
         *
         * So async preemption is off the table until every bridge callback is made re-entrant-safe.
         * That is a real, well-scoped piece of future work with a proven 6.3x payoff; it is not a
         * tweak. Until then the slice is bounded synchronously only: count for a thread that has never
         * reached a bridge, bridge-boundary check for one that has. */
        e=uc_emu_start(S->cpu->uc, begin, RG_RET, 0, _cnt);
        dispatch_arm_slice(S->cpu->uc, 0);
        /* STICKY. Do NOT reset to 0: measured, resetting made the adaptive build SLOWER than plain
         * count=0 (13.84 fps vs 21.57 fps play-phase) because a slice that happens to end without
         * touching a bridge -- a thread that blocks immediately, a short callback-driven slice --
         * demoted the thread back to the 1.56x-slower counted path, so threads flip-flopped.
         *
         * The invariant that actually matters is weaker than "used a bridge last slice": a thread that
         * has EVER reached a bridge will reach one again, so the bridge-boundary check can always end
         * its slice. A thread that has never reached one keeps the bounded count forever -- which is
         * precisely the [T3] non-yielding spin-loop, and is why slice_fast is zero-initialised. */
        if(abshim_bridge_calls != _b0) r->slice_fast = 1;
        if(e!=UC_ERR_OK){ S->fatal=1; snprintf(S->fatal_msg,sizeof S->fatal_msg,"uc_err %d",e);
            { uint32_t pc=rd(S,UC_ARM_REG_PC),sp=rd(S,UC_ARM_REG_SP),lr=rd(S,UC_ARM_REG_LR);
                SLOG("[FAULT] e=%d(%s) pc=engine+0x%x sp=0x%x lr=+0x%x gt=%u begin=0x%x",
                    e, uc_strerror(e), pc>=RG_ENGINE?pc-RG_ENGINE:pc, sp, lr>=RG_ENGINE?lr-RG_ENGINE:lr,
                    S->cur->id, begin>=RG_ENGINE?begin-RG_ENGINE:begin); }
            break; }
        if(S->fatal) break;                          /* set by a nested fault (sched_once) */
        if(S->fatal_ext && *S->fatal_ext){           /* M3: dispatch abort/__stack_chk_fail */
            S->fatal=1; snprintf(S->fatal_msg,sizeof S->fatal_msg,"guest fatal (abort)"); break; }
        uint32_t pcnow=rd(S,UC_ARM_REG_PC);
        r=S->cur;
        if(SDBG && guard<400) fprintf(stderr,"      -> e=%d pcnow=0x%x pend_blk=%d wk=%d is_eng=%d\n",
            e, pcnow, r->pending_block, r->wait_kind, r->is_engine);

        /* pthread_once call-injection: sched_once set once_inject_pc and uc_emu_stop'd this
         * quantum; (re)start the gthread at the init routine with LR=RG_RET so it runs as this
         * thread's own flow (no nested uc_emu_start). Its return-to-RG_RET is handled below. */
        if(r->once_inject_pc){
            uint32_t rt=r->once_inject_pc; r->once_inject_pc=0;
            wr(S, UC_ARM_REG_LR, RG_RET);
            r->resume_pc = rt;                       /* LSB selects ARM/Thumb */
            continue;
        }

        /* S2 stop/restart for a blocking JNI Call*Method (audio/render deadlock fix). uc_emu_start
         * has ALREADY returned (env_dispatch_real stashed the call + redirected the guest to RG_RET
         * via LR, so pcnow==RG_RET — this MUST be checked before the RG_RET-exit handling below).
         * Do the real, possibly-BLOCKING ART call with the GEL RELEASED so ANOTHER green thread's
         * carrier can run meanwhile -> the cross-thread ABBA deadlock (A holds GEL + waits in ART
         * for a monitor held by B; B waits for the GEL) cannot form. Then re-acquire, restore this
         * gthread's saved regs, inject the result, and resume after the call. cur stays RUNNING —
         * this is NOT a park-for-signal block; the SAME carrier does the ART call. */
        if(r->pending_jni){
            r->pending_jni=0;
            uc_context_save(S->cpu->uc, r->ctx); r->started=1;
            pthread_mutex_unlock(&S->gel);
            pthread_cond_broadcast(&S->gel_cv);          /* wake a carrier waiting for the GEL */
            if(S->jni_do_call) S->jni_do_call(r);        /* the real ART Call*MethodA (GEL released) */
            pthread_mutex_lock(&S->gel);
            S->cur=r; uc_context_restore(S->cpu->uc, r->ctx); wr(S, UC_ARM_REG_C13_C0_3, r->tcb);
            if(S->jni_finalize) S->jni_finalize(r);      /* tokenise + write r0/r1 into the guest (GEL held) */
            r->resume_pc = r->jni_ret_pc;                /* resume the guest right after the JNI call */
            continue;
        }

        if(r->pending_block){
            r->pending_block=0;
            uc_context_save(S->cpu->uc, r->ctx); r->started=1;
            if(r->wait_kind==WK_NONE) runq_push(S,r);      /* sched_yield: RUNNABLE behind others */
            else                      r->state=GT_BLOCKED; /* real block: already on a wait queue */
            gthread*nx=runq_pop(S);
            if(!nx) nx=idle_wait_pick(S);
            if(S->fatal) break;
            ctx_switch_in(S,nx);
            continue;
        }
        if(pcnow==RG_RET){
            /* an injected pthread_once init routine returned: pop it, mark the once done,
             * wake any threads that were waiting on it, and resume the pthread_once CALLER
             * (r0=0) — NOT a thread exit. Must be checked before is_engine/gt below. */
            if(r->once_depth>0){
                r->once_depth--;
                struct sobj*o=(struct sobj*)r->once_obj[r->once_depth];
                o->once_state=2; o->once_runner_id=0;
                gthread*w; while((w=wq_pop(&o->waiters))){ w->wait_kind=WK_NONE; w->wait_obj=NULL; w->inject_val=0; w->inject_r0=1; runq_push(S,w); }
                wr(S, UC_ARM_REG_R0, 0);                 /* pthread_once returns 0 */
                r->resume_pc = r->once_ret[r->once_depth];   /* resume the caller after pthread_once */
                SLOG("[once-complete] gt=%u depth->%d resume-caller=+0x%x", r->id, r->once_depth,
                    r->resume_pc>=RG_ENGINE?r->resume_pc-RG_ENGINE:r->resume_pc);
                continue;
            }
            if(r->is_engine){
                uint32_t rv=rd(S,UC_ARM_REG_R0);
                thread_exit(S,r,rv);
                gthread*nx=runq_pop(S);
                if(!nx) nx=idle_wait_pick(S);
                if(S->fatal) break;
                ctx_switch_in(S,nx);
                continue;
            }
            /* a host-entry gthread returned to the top => its JNI call is complete */
            if(r==gt){
                { uint32_t sp=rd(S,UC_ARM_REG_SP); uint32_t r0=rd(S,UC_ARM_REG_R0);
                    int premature = sp < gt->stack_hi-0x40u;
                    if(premature || SDBG)
                        SLOG("[RG_RET-exit] gt=%u sp=0x%x r0=0x%x stack_hi=0x%x once_depth=%d => %s",
                            gt->id,sp,r0,gt->stack_hi,gt->once_depth, premature?"PREMATURE-deep(!!)":"NORMAL-unwind"); }
                uc_context_save(S->cpu->uc, gt->ctx); gt->started=1;
                gt->state=GT_IDLE;
                break;                               /* return to the Java caller */
            }
            /* defensive: some other host gthread hit RG_RET (should not happen) */
            r->state=GT_IDLE;
            gthread*nx=runq_pop(S);
            if(!nx) nx=idle_wait_pick(S);
            if(S->fatal) break;
            ctx_switch_in(S,nx);
            continue;
        }
        /* quantum expired mid-execution => preemption point */
        {
            /* resume_pc must carry the Thumb bit: rd(UC_ARM_REG_PC) strips it, and
             * uc_emu_start selects ARM/Thumb from the begin-address LSB (not the live
             * CPSR.T). Without this, a Thumb function (e.g. the C++ runtime / std::string
             * in the Lua parser) that straddles a quantum boundary resumes in ARM mode
             * and decodes garbage -> UC_ERR_INSN_INVALID. CPSR.T is bit 5 (0x20). */
            uint32_t resume = pcnow | ((rd(S,UC_ARM_REG_CPSR) & 0x20u) ? 1u : 0u);
            gthread*nx=runq_pop(S);
            if(nx){
                uc_context_save(S->cpu->uc, r->ctx); r->started=1;
                r->resume_pc=resume;
                runq_push(S,r);                      /* round-robin: behind nx */
                ctx_switch_in(S,nx);
            } else {
                r->resume_pc=resume;                 /* solo: keep running r */
            }
            continue;
        }
    }
    S->cur=NULL;
    pthread_cond_broadcast(&S->gel_cv);          /* let a waiting host thread take over */
    return e;
}

uc_err sched_enter(sched *S, gthread *gt, uint32_t pc, uint64_t budget){
    (void)budget;                                 /* caller has already framed the regs */
    pthread_mutex_lock(&S->gel);
    int _sav=S->drive_active; S->drive_active=1;  /* run_loop is now the active driver (S2 gate) */
    gt->resume_pc=pc; gt->state=GT_RUNNING; gt->started=1; S->cur=gt;
    wr(S, UC_ARM_REG_C13_C0_3, gt->tcb);
    uc_err e=run_loop(S,gt);
    S->drive_active=_sav;
    pthread_mutex_unlock(&S->gel);
    return e;
}

/* framed entry (analogue of cpu_call): sets SP/LR/args UNDER the GEL so concurrent
 * host-thread entries can't clobber each other's register frame. */
uc_err sched_call(sched *S, gthread *gt, uint32_t addr, uint32_t a0,uint32_t a1,uint32_t a2,uint32_t a3){
    pthread_mutex_lock(&S->gel);
    int _sav=S->drive_active; S->drive_active=1;  /* run_loop is now the active driver (S2 gate) */
    wr(S, UC_ARM_REG_SP, gt->stack_hi & ~15u);
    wr(S, UC_ARM_REG_LR, RG_RET);
    wr(S, UC_ARM_REG_R0,a0); wr(S, UC_ARM_REG_R1,a1); wr(S, UC_ARM_REG_R2,a2); wr(S, UC_ARM_REG_R3,a3);
    gt->resume_pc=addr; gt->state=GT_RUNNING; gt->started=1; S->cur=gt;
    wr(S, UC_ARM_REG_C13_C0_3, gt->tcb);
    uc_err e=run_loop(S,gt);
    S->drive_active=_sav;
    pthread_mutex_unlock(&S->gel);
    return e;
}

gthread *sched_current(sched *S){ return S->cur; }

/* diagnostic: dump every live green thread's state — reveals a worker (loader) thread stuck
 * BLOCKED (state=2) on a cond/mutex it's never signalled out of, with resume_pc = where it
 * waits. wait_kind: 0 none,1 mutex,2 cond,3 join. */
/* Dump every live green-thread's block state to logcat (fprintf/stderr is invisible on
 * Android). resume_pc is where a BLOCKED thread will resume = the return addr of the
 * primitive it blocked in, so (resume-4/-2) is the call site; printed RG_ENGINE-relative
 * (matches eng.dis offsets). wk names the primitive, obj is its guest addr. A stall shows
 * as a thread stuck BLOCKED with a STABLE resume across successive dumps — e.g. parked on
 * a cond/join that no one will signal (a network worker we cut).
 *
 * wk VALUES (keep in sync with the enum in sched.h — the old list here said only
 * "0 none,1 mutex,2 cond,3 join" and was four values out of date, which is exactly how a
 * real device dump showing wk=7 got misread):
 *     0 WK_NONE   1 WK_MUTEX   2 WK_COND   3 WK_JOIN
 *     4 WK_RWLOCK_RD   5 WK_RWLOCK_WR   6 WK_ONCE   7 WK_SLEEP  */
void sched_dump_state(sched *S){
    SLOG("[sched-dump] nthreads=%d cur=%u runq=%s", S->nthreads, S->cur?S->cur->id:0u, S->runq_head?"Y":"n");
    for(int i=0;i<S->nthreads;i++){ gthread*g=&S->all[i];
        if(g->state==GT_UNUSED) continue;
        struct sobj*wo=(struct sobj*)g->wait_obj;
        uint32_t rp=g->resume_pc, rrel=(rp>=RG_ENGINE)?rp-RG_ENGINE:rp;
        uint32_t sf=(g->start_fn>=RG_ENGINE)?g->start_fn-RG_ENGINE:g->start_fn;
        SLOG("  gt[%d] id=%u st=%d wk=%d obj=0x%x mtx=0x%x resume=+0x%x dl=%llu joiner=%u odep=%d eng=%d startfn=+0x%x",
             i, g->id, g->state, g->wait_kind, wo?wo->addr:0u, g->wait_mutex, rrel,
             (unsigned long long)g->deadline_ns, g->joiner?g->joiner->id:0u, g->once_depth, g->is_engine, sf);
    }
}

/* ---------------------------------------------------------------- primitives */
int sched_create(sched *S, uint32_t start_fn, uint32_t arg, uint32_t *out_tid){
    gthread*g=alloc_gt(S);
    if(!g) return E_INVAL;                            /* EAGAIN-ish: out of thread slots */
    g->id = S->next_id++; if(!g->id) g->id=S->next_id++;
    g->is_engine=1; g->started=0; g->start_fn=start_fn; g->start_arg=arg;
    assign_stack_tcb(S,g);
    if(uc_context_alloc(S->cpu->uc, &g->ctx)){ g->state=GT_UNUSED; return E_INVAL; }
    runq_push(S,g);                                  /* RUNNABLE; scheduler will pick it */
    if(out_tid) *out_tid=g->id;
    return 0;
}

int sched_join(sched *S, uint32_t tid, uint32_t retval_ptr){
    gthread*t=by_id(S,tid);
    if(!t || t==S->cur) return E_SRCH;
    if(t->detached) return E_INVAL;
    if(t->reapable){                                 /* already exited: reap now */
        if(retval_ptr) gm_wr32(&S->cpu->mem, retval_ptr, t->retval);
        if(t->ctx){ uc_context_free(t->ctx); t->ctx=NULL; }
        t->state=GT_UNUSED;
        return 0;
    }
    if(t->joiner) return E_INVAL;                     /* already being joined */
    t->joiner=S->cur; S->cur->join_retptr=retval_ptr;
    S->cur->wait_kind=WK_JOIN; S->cur->wait_obj=t;
    arm_block(S);
    return 0;
}

int sched_detach(sched *S, uint32_t tid){
    gthread*t=by_id(S,tid); if(!t) return E_SRCH;
    if(t->reapable){ if(t->ctx){ uc_context_free(t->ctx); t->ctx=NULL; } t->state=GT_UNUSED; return 0; }
    t->detached=1; return 0;
}

void sched_exit(sched *S, uint32_t retval){
    /* pthread_exit: make it look exactly like the start routine returning to RG_RET,
     * then let the run-loop's RG_RET+is_engine branch run thread_exit(). The
     * pthread_exit stub's own `bx lr` now branches to RG_RET. */
    wr(S, UC_ARM_REG_R0, retval);
    wr(S, UC_ARM_REG_LR, RG_RET);
}

uint32_t sched_self(sched *S){ return S->cur ? S->cur->id : 0; }

void sched_yield_now(sched *S){
    if(!S->runq_head) return;                       /* nobody else runnable: a no-op */
    S->cur->resume_pc = rd(S, UC_ARM_REG_LR);
    wr(S, UC_ARM_REG_LR, RG_RET);
    S->cur->inject_val=0; S->cur->inject_r0=1;      /* sched_yield returns 0 */
    S->cur->wait_kind=WK_NONE;                      /* run-loop: yield (RUNNABLE), not a block */
    S->cur->pending_block=1;                         /* unwind to RG_RET; run-loop re-queues us */
}

int sched_mutex_lock(sched *S, uint32_t m, int trylock){
    struct sobj*mx=getobj(S,m,SOBJ_MUTEX);
    if(!mx) return E_BUSY;   /* out of memory: report contention rather than crash */
    gthread*c=S->cur;
    if(mx->owner_id==0){ mx->owner_id=c->id; mx->count=1; return 0; }
    if(mx->owner_id==c->id){ mx->count++; return 0; } /* recursive-safe over-approximation */
    if(trylock) return E_BUSY;
    c->wait_kind=WK_MUTEX; c->wait_obj=mx; wq_push(&mx->waiters,c);
    arm_block(S);
    return 0;
}
int sched_mutex_unlock(sched *S, uint32_t m){
    struct sobj*mx=getobj(S,m,SOBJ_MUTEX);
    if(!mx) return 0;
    if(mx->owner_id!=S->cur->id) return E_PERM;
    if(mx->count>1){ mx->count--; return 0; }
    mutex_release(S,m);
    return 0;
}

int sched_cond_wait(sched *S, uint32_t c, uint32_t m, uint64_t deadline_ns){
    struct sobj*cv=getobj(S,c,SOBJ_COND);
    if(!cv) return 0;
    /* release the mutex (hands off to any waiter) then block on the cond */
    { struct sobj*mo=getobj(S,m,SOBJ_MUTEX); if(mo && mo->owner_id==S->cur->id) mutex_release(S,m); }
    S->cur->wait_mutex=m; S->cur->wait_kind=WK_COND; S->cur->wait_obj=cv;
    S->cur->deadline_ns=deadline_ns; S->cur->timed_out=0;
    wq_push(&cv->waiters, S->cur);
    arm_block(S);
    return 0;
}
/* Pure timed block: park the current green thread for `duration_ns`, releasing the GEL so other
 * threads (nativeInit, render) run. No wait_obj/mutex — idle_wait_pick's timeout_fire wakes it
 * with r0=0. Used by poll() with no ready fds: a de-fanged phone-home/heartbeat thread polls a
 * dead socket forever and MUST yield here or it busy-spins and starves the cooperative GEL. */
int sched_sleep(sched *S, uint64_t duration_ns){
    if(!S->cur) return 0;
    S->cur->wait_kind=WK_SLEEP; S->cur->wait_obj=NULL; S->cur->wait_mutex=0;
    S->cur->deadline_ns = now_ns() + duration_ns; S->cur->timed_out=0;
    arm_block(S);
    return 0;
}
int sched_cond_wake(sched *S, uint32_t c, int broadcast){
    struct sobj*cv=getobj(S,c,SOBJ_COND);
    if(!cv) return 0;
    for(;;){
        gthread*w=wq_pop(&cv->waiters);
        if(!w) break;
        w->deadline_ns=0;
        cond_release_one(S, cv, w, 0);
        if(!broadcast) break;
    }
    return 0;
}

int sched_rwlock_lock(sched *S, uint32_t rw, int write, int trylock){
    struct sobj*o=getobj(S,rw,SOBJ_RWLOCK);
    if(!o) return E_BUSY;
    if(write){
        if(o->writer_id==0 && o->readers==0){ o->writer_id=S->cur->id; return 0; }
        if(trylock) return E_BUSY;
        S->cur->wait_kind=WK_RWLOCK_WR; S->cur->wait_obj=o; wq_push(&o->waiters,S->cur);
        arm_block(S); return 0;
    } else {
        if(o->writer_id==0 && o->waiters==NULL){ o->readers++; return 0; } /* writer-preference */
        if(trylock) return E_BUSY;
        S->cur->wait_kind=WK_RWLOCK_RD; S->cur->wait_obj=o; wq_push(&o->rwaiters,S->cur);
        arm_block(S); return 0;
    }
}
int sched_rwlock_unlock(sched *S, uint32_t rw){
    struct sobj*o=getobj(S,rw,SOBJ_RWLOCK);
    if(!o) return 0;
    if(o->writer_id==S->cur->id){ o->writer_id=0; }
    else if(o->readers>0){ o->readers--; if(o->readers>0) return 0; }
    else return E_PERM;
    /* grant: prefer a waiting writer, else release all pending readers */
    if(o->writer_id==0 && o->readers==0 && o->waiters){
        gthread*w=wq_pop(&o->waiters); o->writer_id=w->id;
        w->wait_kind=WK_NONE; w->wait_obj=NULL; w->inject_val=0; w->inject_r0=1; runq_push(S,w);
    } else if(o->writer_id==0){
        gthread*w; while((w=wq_pop(&o->rwaiters))){ o->readers++; w->wait_kind=WK_NONE; w->wait_obj=NULL; w->inject_val=0; w->inject_r0=1; runq_push(S,w); }
    }
    return 0;
}

int sched_once(sched *S, uint32_t once, uint32_t routine){
    struct sobj*o=getobj(S,once,SOBJ_ONCE);
    if(!o) return 0;                                  /* OOM: treat as already-done rather than crash */
    if(o->once_state==2) return 0;
    if(o->once_state==1){                             /* another thread runs it: wait */
        S->cur->wait_kind=WK_ONCE; S->cur->wait_obj=o; wq_push(&o->waiters,S->cur);
        arm_block(S); return 0;
    }
    o->once_state=1; o->once_runner_id=S->cur->id;
    /* CALL-INJECTION (no nested uc_emu_start): run the init routine as THIS gthread's own
     * flow. A nested cpu_run here is re-entrant (it runs inside the stub UC_HOOK_CODE, inside
     * the scheduler's uc_emu_start) and corrupts the outer emulation's state -> the caller's
     * stack/PC drift and it later faults (observed: worker pop{pc} -> engine BSS). Instead:
     * remember where the pthread_once caller resumes, stop this quantum, and let run_loop
     * (re)start the gthread at `routine` with LR=RG_RET; when the routine returns to RG_RET,
     * run_loop pops this once (mark done + wake waiters) and resumes the caller with r0=0.
     * Because the routine runs as normal scheduled flow, it may even block safely. */
    gthread *g=S->cur;
    if(g->once_depth>=8){ S->fatal=1; snprintf(S->fatal_msg,sizeof S->fatal_msg,"pthread_once nesting too deep"); return 0; }
    g->once_obj[g->once_depth]=o;
    g->once_ret[g->once_depth]=rd(S,UC_ARM_REG_LR);   /* pthread_once caller's resume addr (Thumb LSB kept) */
    g->once_depth++;
    g->once_inject_pc = routine ? routine : 2u;       /* nonzero => run_loop redirects; carries Thumb LSB */
    SLOG("[sched_once] inject routine=engine+0x%x once=0x%x runner=%u depth=%d caller-LR=+0x%x",
        routine>=RG_ENGINE?routine-RG_ENGINE:routine, once, o->once_runner_id, g->once_depth,
        g->once_ret[g->once_depth-1]>=RG_ENGINE?g->once_ret[g->once_depth-1]-RG_ENGINE:g->once_ret[g->once_depth-1]);
    uc_emu_stop(S->cpu->uc);                           /* end this quantum; run_loop takes over */
    return 0;
}

uint32_t sched_key_create(sched *S, uint32_t destructor){
    for(uint32_t k=1;k<SCHED_MAX_KEYS;k++) if(!S->key_used[k]){ S->key_used[k]=1; S->key_destr[k]=destructor; return k; }
    return 0;
}
int sched_key_delete(sched *S, uint32_t key){
    if(key<SCHED_MAX_KEYS){ S->key_used[key]=0; S->key_destr[key]=0; }
    return 0;
}
uint32_t sched_getspecific(sched *S, uint32_t key){
    if(!S->cur || key>=SCHED_MAX_KEYS) return 0;
    return S->cur->tls[key];
}
int sched_setspecific(sched *S, uint32_t key, uint32_t val){
    if(S->cur && key<SCHED_MAX_KEYS) S->cur->tls[key]=val;
    return 0;
}
uint32_t sched_errno_addr(sched *S){ return S->cur ? S->cur->errno_addr : 0; }

/* ------------------------------------------------------------------ lifecycle */
gthread *sched_host_gthread(sched *S){
    pthread_t self=pthread_self();
    for(int i=0;i<S->nthreads;i++){
        gthread*g=&S->all[i];
        if(g->state!=GT_UNUSED && g->host_valid && pthread_equal(g->host,self)){
#ifdef ABSHIM_AUDIO
            /* Same-pthread JNI re-entrancy (exposed by the ABSHIM_AUDIO BEL-release): if this
             * carrier is RUNNING it is parked mid blocking-ART-call (the S2 path keeps state
             * RUNNING) and the guest ran Java on THIS host thread that called back into native
             * (e.g. nativeResume -> AudioOutput.nativeMixData on the main thread). Reusing its gt
             * would make sched_call reset SP to stack_hi and clobber the parked frame's stack
             * canary -> __stack_chk_fail. Skip it; fall through to a fresh nested gt (own stack +
             * TCB), which real hardware provides implicitly (one real stack per thread). A finished
             * carrier is GT_IDLE, so normal reuse is unaffected. Inert in the default build (the
             * BEL is never released mid-call there, so a pthread's gt is never RUNNING here). */
            if(g->state==GT_RUNNING) continue;
#endif
            return g;
        }
    }
    gthread*g=alloc_gt(S); if(!g) return NULL;
    g->id=S->next_id++; if(!g->id) g->id=S->next_id++;
    g->is_engine=0; g->started=0; g->host=self; g->host_valid=1;
    assign_stack_tcb(S,g);                            /* slot 0 -> 8 MiB main stack; else 2 MiB */
    uc_context_alloc(S->cpu->uc,&g->ctx);
    g->state=GT_IDLE;
    return g;
}

sched *g_sched=NULL;   /* the live scheduler (for bridges that must set the CURRENT guest thread's errno) */
int sched_init(sched *S, cpu_t *cpu){
    memset(S,0,sizeof *S);
    S->cpu=cpu; S->next_id=1; S->key_next=1;   /* stacks/TCBs are per-slot (assign_stack_tcb) */
    g_sched=S;
    if(pthread_mutex_init(&S->gel,NULL)) return -1;
    /* M1: gel_cv must be a MONOTONIC-clock cond — idle_wait_pick feeds it absolute
     * monotonic deadlines; the default (REALTIME) would sleep for the wrong duration. */
    pthread_condattr_t ca; pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    int r=pthread_cond_init(&S->gel_cv,&ca); pthread_condattr_destroy(&ca);
    if(r) return -1;
    S->gel_init=1;
    return 0;
}
void sched_destroy(sched *S){
    for(int i=0;i<S->nthreads;i++) if(S->all[i].ctx){ uc_context_free(S->all[i].ctx); S->all[i].ctx=NULL; }
    for(int b=0;b<256;b++){ struct sobj*o=S->obj[b]; while(o){ struct sobj*n=o->hnext; free(o); o=n; } }
    if(S->gel_init){ pthread_mutex_destroy(&S->gel); pthread_cond_destroy(&S->gel_cv); }
}
