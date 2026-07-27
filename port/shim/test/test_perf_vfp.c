/* test_perf_vfp.c — measure HOT VFP (single-precision float) throughput under TCG, the
 * crux for a float-heavy physics engine (Box2D). The loop body was assembled by the NDK
 * (correct encodings), not hand-written. Compare to test_perf.c (B) = 376 Minsn/s for hot
 * integer+memory: this tells us whether emulated floating-point is a bottleneck. */
#include "cpu.h"
#include "regions.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec + t.tv_nsec/1e9; }
int main(void){
    cpu_t c; if(cpu_create(&c)){ printf("cpu_create failed\n"); return 1; }
    uint32_t CODE=0x10040000u; uc_mem_map(c.uc,CODE,0x1000,UC_PROT_READ|UC_PROT_EXEC);
    /* NDK-assembled: vldr s0,[r2]; vldr s1,[r2,#4]; vadd.f32 s2,s0,s1; vmul.f32 s3,s0,s1;
       vadd.f32 s4,s2,s3; vmul.f32 s5,s4,s0; vstr s5,[r2,#8]; subs r0,#1; bne loop; bx lr */
    uint32_t prog[10]={0xed920a00u,0xedd20a01u,0xee301a20u,0xee601a20u,0xee312a21u,
                       0xee622a00u,0xedc22a02u,0xe2500001u,0x1afffff6u,0xe12fff1eu};
    uc_mem_write(c.uc,CODE,prog,sizeof prog);
    uint32_t D=RG_HEAP+0x1000u; float f1=1.5f,f2=2.5f;
    uc_mem_write(c.uc,D,&f1,4); uc_mem_write(c.uc,D+4,&f2,4);
    uint64_t N=50000000ull; uint32_t r0=(uint32_t)N,r2=D,lr=RG_RET;
    uc_reg_write(c.uc,UC_ARM_REG_R0,&r0); uc_reg_write(c.uc,UC_ARM_REG_R2,&r2); uc_reg_write(c.uc,UC_ARM_REG_LR,&lr);
    double t0=now(); uc_err e=uc_emu_start(c.uc,CODE,RG_RET,0,0); double dt=now()-t0;
    double ins=9.0*N;   /* 9 instructions per loop trip (7 VFP + subs + bne) */
    printf("VFP-heavy hot loop: emu=%s  %.0f Minsn / %.3f s = %.1f Minsn/s (7 VFP ops/9 insns per trip)\n",
           uc_strerror(e), ins/1e6, dt, ins/1e6/dt);
    printf("  (cf. hot integer+memory = ~376 Minsn/s; cold init = ~41 Minsn/s)\n");
    cpu_destroy(&c);
    return 0;
}
