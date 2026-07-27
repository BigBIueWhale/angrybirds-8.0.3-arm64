/* test_sched.c — device layer: validate the GEL green-thread scheduler with three
 * hand-assembled guest programs driven through the REAL dispatch->sched path:
 *   T1  pthread_create + pthread_join + return value + shared memory
 *   T2  pthread_cond_wait / signal handoff (block -> mutex re-acquire -> resume)
 *   T3  preemption: a non-yielding spin-loop must still let another thread run
 * No engine needed. */
#include "dispatch.h"
#include "sched.h"
#include "regions.h"
#include <stdio.h>
#include <string.h>

static int fails=0;
#define CK(c,msg) do{ if(!(c)){ printf("  FAIL: %s\n",msg); fails++; } }while(0)

/* ---- ARM encoders ---- */
static uint32_t e_bl (uint32_t f,uint32_t t){ int32_t o=((int32_t)(t-(f+8)))>>2; return 0xEB000000u|((uint32_t)o&0xFFFFFFu); }
static uint32_t e_b  (uint32_t f,uint32_t t){ int32_t o=((int32_t)(t-(f+8)))>>2; return 0xEA000000u|((uint32_t)o&0xFFFFFFu); }
static uint32_t e_beq(uint32_t f,uint32_t t){ int32_t o=((int32_t)(t-(f+8)))>>2; return 0x0A000000u|((uint32_t)o&0xFFFFFFu); }
static uint32_t e_bne(uint32_t f,uint32_t t){ int32_t o=((int32_t)(t-(f+8)))>>2; return 0x1A000000u|((uint32_t)o&0xFFFFFFu); }
static uint32_t movw_(int rd,uint32_t i){ i&=0xffff; return 0xe3000000u|((uint32_t)rd<<12)|((i&0xf000u)<<4)|(i&0xfffu); }
static uint32_t movt_(int rd,uint32_t i){ i=(i>>16)&0xffff; return 0xe3400000u|((uint32_t)rd<<12)|((i&0xf000u)<<4)|(i&0xfffu); }
static uint32_t movi_(int rd,uint32_t i){ return 0xe3a00000u|((uint32_t)rd<<12)|(i&0xff); }
static uint32_t addi_(int rd,int rn,uint32_t i){ return 0xe2800000u|((uint32_t)rn<<16)|((uint32_t)rd<<12)|(i&0xff); }
static uint32_t ldr_ (int rd,int rn,uint32_t i){ return 0xe5900000u|((uint32_t)rn<<16)|((uint32_t)rd<<12)|(i&0xfff); }
static uint32_t str_ (int rd,int rn,uint32_t i){ return 0xe5800000u|((uint32_t)rn<<16)|((uint32_t)rd<<12)|(i&0xfff); }
static uint32_t movr_(int rd,int rm){ return 0xe1a00000u|((uint32_t)rd<<12)|(uint32_t)rm; }
static uint32_t cmpi_(int rn,uint32_t i){ return 0xe3500000u|((uint32_t)rn<<16)|(i&0xff); }
static uint32_t bxr_ (int rm){ return 0xe12fff10u|(uint32_t)rm; }
#define BXLR 0xe12fff1eu

typedef struct { uint32_t buf[128]; uint32_t base; int n; } A;
static void E(A*a,uint32_t i){ a->buf[a->n++]=i; }
static uint32_t HERE(A*a){ return a->base + (uint32_t)a->n*4; }
static void Em32(A*a,int rd,uint32_t v){ E(a,movw_(rd,v)); E(a,movt_(rd,v)); }
static void Ebl(A*a,uint32_t to){ uint32_t f=HERE(a); E(a,e_bl(f,to)); }
static void flush(cpu_t*c,A*a){ uc_mem_write(c->uc,a->base,a->buf,(size_t)a->n*4); }

/* stub slots */
enum { S_CREATE,S_JOIN,S_MLOCK,S_MUNLOCK,S_CWAIT,S_CSIG,S_YIELD,S_SELF,S_DETACH,S_N };
static uint32_t STUB(int s){ return RG_STUB + (uint32_t)s*4; }

/* guest data cells */
#define DATA    (RG_GUESTDATA+0x800u)
#define TIDP    (DATA+0x00u)
#define RETP    (DATA+0x04u)
#define SHARED  (DATA+0x08u)
#define MUTEX   (DATA+0x40u)
#define COND    (DATA+0x50u)
#define PRED    (DATA+0x60u)
#define DONE    (DATA+0x70u)
#define SHARED2 (DATA+0x80u)

#define CODE   0x10040000u
#define WORKER 0x10040400u

static void setup(cpu_t*cpu, sched*S, loader_t*L, dispatch_t*D){
    cpu_create(cpu);
    uc_mem_map(cpu->uc, CODE, 0x1000, UC_PROT_READ|UC_PROT_EXEC);
    sched_init(S,cpu);
    memset(L,0,sizeof *L);
    L->stub_name[S_CREATE]="pthread_create"; L->stub_name[S_JOIN]="pthread_join";
    L->stub_name[S_MLOCK]="pthread_mutex_lock"; L->stub_name[S_MUNLOCK]="pthread_mutex_unlock";
    L->stub_name[S_CWAIT]="pthread_cond_wait"; L->stub_name[S_CSIG]="pthread_cond_signal";
    L->stub_name[S_YIELD]="sched_yield"; L->stub_name[S_SELF]="pthread_self";
    L->stub_name[S_DETACH]="pthread_detach"; L->stub_count=S_N;
    dispatch_install(D,cpu,L);
    D->sch=S;
    /* zero the data cells */
    for(uint32_t o=0;o<0x100;o+=4) gm_wr32(&cpu->mem, DATA+o, 0);
}
static void teardown(cpu_t*cpu, sched*S){ sched_destroy(S); cpu_destroy(cpu); }

/* --------------------------------------------------------------- T1 */
static void t1_create_join(void){
    printf("[T1] create + join + retval + shared write\n");
    cpu_t cpu; sched S; loader_t L; dispatch_t D; setup(&cpu,&S,&L,&D);
    A m={.base=CODE}, w={.base=WORKER};
    /* main */
    E(&m,movr_(10,14));                                  /* mov r10,lr (=RG_RET) */
    Em32(&m,0,TIDP); E(&m,movi_(1,0)); Em32(&m,2,WORKER); E(&m,movw_(3,0x100));
    Ebl(&m,STUB(S_CREATE));
    Em32(&m,4,TIDP); E(&m,ldr_(0,4,0));                  /* r0 = tid */
    Em32(&m,1,RETP);
    Ebl(&m,STUB(S_JOIN));
    E(&m,bxr_(10));                                      /* return to RG_RET */
    /* worker(arg=0x100): *SHARED=arg+1; return arg+7 */
    E(&w,addi_(1,0,1)); Em32(&w,2,SHARED); E(&w,str_(1,2,0));
    E(&w,addi_(0,0,7)); E(&w,BXLR);
    flush(&cpu,&m); flush(&cpu,&w);

    gthread*mg=sched_host_gthread(&S);
    uc_err e=sched_call(&S,mg,CODE,0,0,0,0);
    uint32_t sh=gm_rd32(&cpu.mem,SHARED), rv=gm_rd32(&cpu.mem,RETP), tid=gm_rd32(&cpu.mem,TIDP);
    printf("   emu=%s tid=%u SHARED=0x%x RETP=0x%x\n", uc_strerror(e), tid, sh, rv);
    CK(e==UC_ERR_OK,"emu ok");
    CK(tid!=0,"pthread_create produced a tid");
    CK(sh==0x101,"worker ran and wrote SHARED=arg+1");
    CK(rv==0x107,"pthread_join delivered the worker return value arg+7");
    teardown(&cpu,&S);
}

/* --------------------------------------------------------------- T2 */
static void t2_cond(void){
    printf("[T2] cond_wait / signal handoff\n");
    cpu_t cpu; sched S; loader_t L; dispatch_t D; setup(&cpu,&S,&L,&D);
    A m={.base=CODE}, w={.base=WORKER};
    /* main: create; yield (let worker cond_wait first); lock; PRED=1; signal; unlock; join */
    E(&m,movr_(10,14));
    Em32(&m,0,TIDP); E(&m,movi_(1,0)); Em32(&m,2,WORKER); E(&m,movi_(3,0));
    Ebl(&m,STUB(S_CREATE));
    Ebl(&m,STUB(S_YIELD));
    Em32(&m,0,MUTEX); Ebl(&m,STUB(S_MLOCK));
    E(&m,movi_(1,1)); Em32(&m,2,PRED); E(&m,str_(1,2,0));
    Em32(&m,0,COND); Ebl(&m,STUB(S_CSIG));
    Em32(&m,0,MUTEX); Ebl(&m,STUB(S_MUNLOCK));
    Em32(&m,4,TIDP); E(&m,ldr_(0,4,0)); Em32(&m,1,RETP); Ebl(&m,STUB(S_JOIN));
    E(&m,bxr_(10));
    /* worker: lock; while(PRED==0) cond_wait(COND,MUTEX); SHARED2=0x55; unlock; return 0x77.
     * Save entry LR(=RG_RET) in r10 since the bl calls clobber LR (as compiled code would). */
    E(&w,movr_(10,14));
    Em32(&w,0,MUTEX); Ebl(&w,STUB(S_MLOCK));
    uint32_t loop=HERE(&w);
    Em32(&w,0,PRED); E(&w,ldr_(1,0,0)); E(&w,cmpi_(1,0));
    int bne_i=w.n; E(&w,0);                               /* placeholder: bne skip */
    Em32(&w,0,COND); Em32(&w,1,MUTEX); Ebl(&w,STUB(S_CWAIT));
    { uint32_t f=HERE(&w); E(&w,e_b(f,loop)); }
    uint32_t skip=HERE(&w);
    w.buf[bne_i]=e_bne(w.base+(uint32_t)bne_i*4, skip);
    E(&w,movi_(1,0x55)); Em32(&w,2,SHARED2); E(&w,str_(1,2,0));
    Em32(&w,0,MUTEX); Ebl(&w,STUB(S_MUNLOCK));
    E(&w,movi_(0,0x77)); E(&w,bxr_(10));                 /* return via saved LR (RG_RET) */
    flush(&cpu,&m); flush(&cpu,&w);

    gthread*mg=sched_host_gthread(&S);
    uc_err e=sched_call(&S,mg,CODE,0,0,0,0);
    uint32_t s2=gm_rd32(&cpu.mem,SHARED2), rv=gm_rd32(&cpu.mem,RETP);
    printf("   emu=%s SHARED2=0x%x RETP=0x%x\n", uc_strerror(e), s2, rv);
    CK(e==UC_ERR_OK,"emu ok");
    CK(s2==0x55,"worker passed the cond_wait and ran its critical section");
    CK(rv==0x77,"join delivered the post-cond worker return value");
    teardown(&cpu,&S);
}

/* --------------------------------------------------------------- T3 */
static void t3_preempt(void){
    printf("[T3] preemption of a non-yielding spin-loop\n");
    cpu_t cpu; sched S; loader_t L; dispatch_t D; setup(&cpu,&S,&L,&D);
    A m={.base=CODE}, w={.base=WORKER};
    /* main: create; spin on DONE (NO yield); then join */
    E(&m,movr_(10,14));
    Em32(&m,0,TIDP); E(&m,movi_(1,0)); Em32(&m,2,WORKER); E(&m,movi_(3,0));
    Ebl(&m,STUB(S_CREATE));
    Em32(&m,0,DONE);                                     /* r0 = &DONE */
    uint32_t spin=HERE(&m);
    E(&m,ldr_(1,0,0)); E(&m,cmpi_(1,0));
    { uint32_t f=HERE(&m); E(&m,e_beq(f,spin)); }        /* while(*DONE==0) spin */
    Em32(&m,4,TIDP); E(&m,ldr_(0,4,0)); Em32(&m,1,RETP); Ebl(&m,STUB(S_JOIN));
    E(&m,bxr_(10));
    /* worker: SHARED2=0x99; DONE=1; return 0 */
    E(&w,movi_(1,0x99)); Em32(&w,2,SHARED2); E(&w,str_(1,2,0));
    E(&w,movi_(1,1));    Em32(&w,2,DONE);    E(&w,str_(1,2,0));
    E(&w,movi_(0,0)); E(&w,BXLR);
    flush(&cpu,&m); flush(&cpu,&w);

    gthread*mg=sched_host_gthread(&S);
    uc_err e=sched_call(&S,mg,CODE,0,0,0,0);
    uint32_t s2=gm_rd32(&cpu.mem,SHARED2), dn=gm_rd32(&cpu.mem,DONE);
    printf("   emu=%s SHARED2=0x%x DONE=%u\n", uc_strerror(e), s2, dn);
    CK(e==UC_ERR_OK,"emu ok");
    CK(dn==1,"spinner was preempted so the worker could set DONE");
    CK(s2==0x99,"worker ran its body under preemption");
    teardown(&cpu,&S);
}

/* --------------------------------------------------------------- T4: Thumb entry */
static void t4_thumb(void){
    printf("[T4] Thumb-mode worker entry (mode selected from start_fn LSB)\n");
    cpu_t cpu; sched S; loader_t L; dispatch_t D; setup(&cpu,&S,&L,&D);
    A m={.base=CODE};
    E(&m,movr_(10,14));
    Em32(&m,0,TIDP); E(&m,movi_(1,0)); Em32(&m,2,WORKER|1u); E(&m,movi_(3,0));  /* fn=WORKER|1 => Thumb */
    Ebl(&m,STUB(S_CREATE));
    Em32(&m,4,TIDP); E(&m,ldr_(0,4,0)); Em32(&m,1,RETP); Ebl(&m,STUB(S_JOIN));
    E(&m,bxr_(10));
    flush(&cpu,&m);
    uint32_t thumb=0x2042u|(0x4770u<<16);            /* movs r0,#0x42 ; bx lr (two Thumb halfwords) */
    uc_mem_write(cpu.uc,WORKER,&thumb,4);
    gthread*mg=sched_host_gthread(&S);
    uc_err e=sched_call(&S,mg,CODE,0,0,0,0);
    uint32_t rv=gm_rd32(&cpu.mem,RETP);
    printf("   emu=%s RETP=0x%x\n", uc_strerror(e), rv);
    CK(e==UC_ERR_OK && !S.fatal,"emu ok");
    CK(rv==0x42,"Thumb worker ran and returned 0x42");
    teardown(&cpu,&S);
}

/* ---------------------------------------------- T5: create/join churn (H2 no-leak) */
static void t5_churn(void){
    printf("[T5] create/join churn x300 (H2: per-slot stacks, no carve leak)\n");
    cpu_t cpu; sched S; loader_t L; dispatch_t D; setup(&cpu,&S,&L,&D);
    A m={.base=CODE}, w={.base=WORKER};
    E(&m,movr_(10,14));
    E(&m,movi_(6,0));                                 /* r6 = success count */
    E(&m,movw_(7,300));                               /* r7 = loop counter */
    uint32_t loop=HERE(&m);
    Em32(&m,0,TIDP); E(&m,movi_(1,0)); Em32(&m,2,WORKER); E(&m,movi_(3,0));
    Ebl(&m,STUB(S_CREATE));
    E(&m,cmpi_(0,0));                                 /* create returned 0? */
    int bne_i=m.n; E(&m,0);                           /* bne skip (create failed) */
    Em32(&m,4,TIDP); E(&m,ldr_(0,4,0)); E(&m,movi_(1,0)); Ebl(&m,STUB(S_JOIN));
    E(&m,addi_(6,6,1));                               /* success++ */
    uint32_t skip=HERE(&m);
    m.buf[bne_i]=e_bne(m.base+(uint32_t)bne_i*4, skip);
    E(&m,0xe2577001u);                                /* subs r7,r7,#1 */
    { uint32_t f=HERE(&m); E(&m,e_bne(f,loop)); }     /* while r7 != 0 */
    Em32(&m,0,SHARED); E(&m,str_(6,0,0));             /* SHARED = success count */
    E(&m,bxr_(10));
    E(&w,movi_(0,0)); E(&w,BXLR);                     /* worker: return 0 */
    flush(&cpu,&m); flush(&cpu,&w);
    gthread*mg=sched_host_gthread(&S);
    uc_err e=sched_call(&S,mg,CODE,0,0,0,0);
    uint32_t n=gm_rd32(&cpu.mem,SHARED);
    printf("   emu=%s successes=%u/300 fatal=%d\n", uc_strerror(e), n, S.fatal);
    CK(e==UC_ERR_OK && !S.fatal,"emu ok");
    CK(n==300,"all 300 create+join cycles succeeded (recycled slots, no leak)");
    teardown(&cpu,&S);
}

/* ------------------------------------------------- T6: deadlock detection (C1) */
static void t6_deadlock(void){
    printf("[T6] deadlock detection (C1: fail loudly, do not hang)\n");
    cpu_t cpu; sched S; loader_t L; dispatch_t D; setup(&cpu,&S,&L,&D);
    A m={.base=CODE}, w={.base=WORKER};
    E(&m,movr_(10,14));
    Em32(&m,0,TIDP); E(&m,movi_(1,0)); Em32(&m,2,WORKER); E(&m,movi_(3,0)); Ebl(&m,STUB(S_CREATE));
    Ebl(&m,STUB(S_YIELD));                            /* let the worker cond_wait first */
    Em32(&m,4,TIDP); E(&m,ldr_(0,4,0)); E(&m,movi_(1,0)); Ebl(&m,STUB(S_JOIN)); /* blocks forever */
    E(&m,bxr_(10));
    E(&w,movr_(10,14));                               /* worker: cond_wait forever, no signaller */
    Em32(&w,0,COND); Em32(&w,1,MUTEX); Ebl(&w,STUB(S_CWAIT));
    E(&w,bxr_(10));                                   /* unreached */
    flush(&cpu,&m); flush(&cpu,&w);
    gthread*mg=sched_host_gthread(&S);
    uc_err e=sched_call(&S,mg,CODE,0,0,0,0);
    printf("   emu=%s fatal=%d msg=\"%s\"\n", uc_strerror(e), S.fatal, S.fatal_msg);
    CK(S.fatal!=0,"deadlock detected (returned, did not hang)");
    teardown(&cpu,&S);
}

int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    printf("=== GEL green-thread scheduler ===\n");
    t1_create_join();
    t2_cond();
    t3_preempt();
    t4_thumb();
    t5_churn();
    t6_deadlock();
    printf(fails? "\n=== %d FAILURE(S) ===\n" : "\n=== ALL PASS ===\n", fails);
    return fails?1:0;
}
