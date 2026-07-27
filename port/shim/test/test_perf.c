/* test_perf.c — measure Unicorn TCG guest-instruction throughput, to put a first number
 * on SR1 (can the emulated ARM32 engine hit ~60 fps?). The shim runs native arm64 on the
 * phone; only the guest is emulated, so this host measurement is a usable proxy for the
 * TCG interpreter's rate (an arm64 host is typically in the same order of magnitude). */
#include "cpu.h"
#include "regions.h"
#include <stdio.h>
#include <time.h>

static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec + t.tv_nsec/1e9; }

int main(void){
    cpu_t c; if(cpu_create(&c)){ printf("cpu_create failed\n"); return 1; }
    uint32_t CODE=0x10040000u; uc_mem_map(c.uc, CODE, 0x1000, UC_PROT_READ|UC_PROT_EXEC);
    uint64_t N=200000000ull;                       /* loop trips */

    /* --- (A) tight arithmetic loop: subs r0,#1 ; bne .  (2 insns/trip) = TCG ceiling --- */
    { uint32_t prog[2]={0xe2500001u/*subs r0,r0,#1*/,0x1afffffdu/*bne CODE*/};
      uc_mem_write(c.uc,CODE,prog,8);
      uint32_t r0=(uint32_t)N; uc_reg_write(c.uc,UC_ARM_REG_R0,&r0);
      double t0=now(); uc_emu_start(c.uc,CODE,CODE+8,0,0); double dt=now()-t0;
      double ins=2.0*N; printf("(A) tight   : %6.0f Minsn / %5.2fs = %6.1f Minsn/s\n", ins/1e6, dt, ins/1e6/dt); }

    /* --- (B) memory+arith loop: ldr;str;subs;bne (4 insns/trip, 2 softmmu ops) ~ Box2D-ish --- */
    { uint32_t base=RG_HEAP+0x1000u;
      uint32_t prog[4]={0xe5921000u/*ldr r1,[r2]*/,0xe5831000u/*str r1,[r3]*/,0xe2500001u/*subs r0,#1*/,0x1afffffbu/*bne CODE*/};
      uc_mem_write(c.uc,CODE,prog,16);
      uint32_t r0=(uint32_t)N; uc_reg_write(c.uc,UC_ARM_REG_R0,&r0);
      uc_reg_write(c.uc,UC_ARM_REG_R2,&base); uint32_t b3=base+64; uc_reg_write(c.uc,UC_ARM_REG_R3,&b3);
      double t0=now(); uc_emu_start(c.uc,CODE,CODE+16,0,0); double dt=now()-t0;
      double ins=4.0*N; printf("(B) mem+arith: %6.0f Minsn / %5.2fs = %6.1f Minsn/s\n", ins/1e6, dt, ins/1e6/dt); }

    printf("\nFrame budget @60fps = 16.7 ms. If a frame of the engine is ~F Minsn of guest\n");
    printf("code, frame time ~= F / (throughput). E.g. at (B)'s rate:\n");
    /* print a small table */
    double rateB; { /* recompute B rate cheaply is overkill; just annotate at runtime via A/B above */ rateB=0; (void)rateB; }
    printf("   F=0.5 Minsn -> see (B) row;  F=2 Minsn -> 4x that;  F=5 Minsn -> 10x.\n");
    cpu_destroy(&c);
    return 0;
}
