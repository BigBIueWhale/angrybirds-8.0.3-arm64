/* test_libc.c — validate bridge_libc.c's soft-float / int ABI directly (no dispatch).
 * Places args in the guest regs, calls libc_try, checks r0/r1 against the host libc. */
#include "bridge.h"
#include "regions.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static int fails=0;
#define CK(c,msg) do{ if(!(c)){ printf("  FAIL: %s\n",msg); fails++; } }while(0)

static cpu_t CPU;
static void setr(int id,uint32_t v){ uc_reg_write(CPU.uc,id,&v); }
static uint32_t getr(int id){ uint32_t v=0; uc_reg_read(CPU.uc,id,&v); return v; }

/* call libc_try(name) with the double `x` in r0:r1; return the double result r0:r1 */
static double call_d1(const char*nm,double x){
    uint64_t b; memcpy(&b,&x,8); setr(UC_ARM_REG_R0,(uint32_t)b); setr(UC_ARM_REG_R1,(uint32_t)(b>>32));
    mcur cur; marshal_cur_init(&CPU.mem,&cur);
    CK(libc_try(&CPU,nm,&cur)==1,nm);
    uint64_t r=getr(UC_ARM_REG_R0)|((uint64_t)getr(UC_ARM_REG_R1)<<32); double d; memcpy(&d,&r,8); return d;
}
static double call_d2(const char*nm,double a,double b){
    uint64_t ba,bb; memcpy(&ba,&a,8); memcpy(&bb,&b,8);
    setr(UC_ARM_REG_R0,(uint32_t)ba); setr(UC_ARM_REG_R1,(uint32_t)(ba>>32));
    setr(UC_ARM_REG_R2,(uint32_t)bb); setr(UC_ARM_REG_R3,(uint32_t)(bb>>32));  /* 2nd double 8-aligned r2:r3 */
    mcur cur; marshal_cur_init(&CPU.mem,&cur);
    CK(libc_try(&CPU,nm,&cur)==1,nm);
    uint64_t r=getr(UC_ARM_REG_R0)|((uint64_t)getr(UC_ARM_REG_R1)<<32); double d; memcpy(&d,&r,8); return d;
}
static float call_f1(const char*nm,float x){
    uint32_t b; memcpy(&b,&x,4); setr(UC_ARM_REG_R0,b);
    mcur cur; marshal_cur_init(&CPU.mem,&cur);
    CK(libc_try(&CPU,nm,&cur)==1,nm);
    uint32_t r=getr(UC_ARM_REG_R0); float f; memcpy(&f,&r,4); return f;
}
static uint32_t call_w(const char*nm){ mcur cur; marshal_cur_init(&CPU.mem,&cur); int ok=libc_try(&CPU,nm,&cur); CK(ok==1,nm); return getr(UC_ARM_REG_R0); }

int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    printf("=== bridge_libc ABI (soft-float / int) ===\n");
    if(cpu_create(&CPU)){ printf("cpu_create failed\n"); return 1; }
    setr(UC_ARM_REG_SP, RG_STACK+RG_STACK_SZ-0x1000u);

    /* --- libm double --- */
    CK(fabs(call_d1("sin",0.5)   - sin(0.5))   <1e-12,"sin(0.5)");
    CK(fabs(call_d1("cos",1.25)  - cos(1.25))  <1e-12,"cos(1.25)");
    CK(fabs(call_d1("sqrt",2.0)  - sqrt(2.0))  <1e-12,"sqrt(2)");
    CK(fabs(call_d1("log",10.0)  - log(10.0))  <1e-12,"log(10)");
    CK(fabs(call_d2("atan2",1.0,1.0) - atan2(1.0,1.0)) <1e-12,"atan2(1,1)");
    CK(fabs(call_d2("pow",2.0,10.0)  - 1024.0) <1e-9,"pow(2,10)=1024");
    CK(fabs(call_d2("fmod",7.5,2.0)  - fmod(7.5,2.0)) <1e-12,"fmod(7.5,2)");

    /* --- libm float --- */
    CK(fabsf(call_f1("sinf",0.5f) - sinf(0.5f)) <1e-6f,"sinf(0.5)");
    CK(fabsf(call_f1("sqrtf",4.0f)- 2.0f)       <1e-6f,"sqrtf(4)=2");

    /* --- ldexp(double,int): double r0:r1, int r2 --- */
    { double x=1.5; uint64_t b; memcpy(&b,&x,8); setr(UC_ARM_REG_R0,(uint32_t)b); setr(UC_ARM_REG_R1,(uint32_t)(b>>32));
      setr(UC_ARM_REG_R2,3); mcur cur; marshal_cur_init(&CPU.mem,&cur); CK(libc_try(&CPU,"ldexp",&cur)==1,"ldexp");
      uint64_t r=getr(UC_ARM_REG_R0)|((uint64_t)getr(UC_ARM_REG_R1)<<32); double d; memcpy(&d,&r,8);
      CK(fabs(d-12.0)<1e-12,"ldexp(1.5,3)=12"); }

    /* --- ctype --- */
    setr(UC_ARM_REG_R0,'5'); { mcur cur; marshal_cur_init(&CPU.mem,&cur); libc_try(&CPU,"isdigit",&cur); }
    CK(getr(UC_ARM_REG_R0)!=0,"isdigit('5') nonzero");
    setr(UC_ARM_REG_R0,'A'); { mcur cur; marshal_cur_init(&CPU.mem,&cur); libc_try(&CPU,"isdigit",&cur); }
    CK(getr(UC_ARM_REG_R0)==0,"isdigit('A')==0");

    /* --- network HARD-FAIL: socket must be -1, NOT 0 --- */
    setr(UC_ARM_REG_R0,2); setr(UC_ARM_REG_R1,1); setr(UC_ARM_REG_R2,0);
    CK(call_w("socket")==0xffffffffu,"socket() == -1 (hard-fail, not fd 0)");
    CK(call_w("connect")==0xffffffffu,"connect() == -1");
    CK(call_w("dlopen")==0,"dlopen() == NULL");

    /* --- strtol over a guest string --- */
    { const char*s="12345xy"; uint32_t p=RG_GUESTDATA+0x200;
      for(int i=0;s[i];i++){ uint8_t ch=(uint8_t)s[i]; uc_mem_write(CPU.uc,p+i,&ch,1);} uint8_t z=0; uc_mem_write(CPU.uc,p+7,&z,1);
      uint32_t ep=RG_GUESTDATA+0x210;
      setr(UC_ARM_REG_R0,p); setr(UC_ARM_REG_R1,ep); setr(UC_ARM_REG_R2,10);
      mcur cur; marshal_cur_init(&CPU.mem,&cur); libc_try(&CPU,"strtol",&cur);
      CK(getr(UC_ARM_REG_R0)==12345,"strtol(\"12345xy\",,10)==12345");
      CK(gm_rd32(&CPU.mem,ep)==p+5,"strtol endptr points past the digits"); }

    /* --- rand48 determinism --- */
    setr(UC_ARM_REG_R0,1); { mcur cur; marshal_cur_init(&CPU.mem,&cur); libc_try(&CPU,"srand48",&cur); }
    uint32_t r1=call_w("lrand48"), r2=call_w("lrand48");
    CK(r1!=r2 && (r1&0x80000000u)==0,"lrand48 produces distinct 31-bit values");

    cpu_destroy(&CPU);
    printf(fails? "\n=== %d FAILURE(S) ===\n":"\n=== ALL PASS ===\n", fails);
    return fails?1:0;
}
