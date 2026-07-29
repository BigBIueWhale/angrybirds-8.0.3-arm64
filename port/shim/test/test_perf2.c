/* test_perf2.c — calibrate SR1 against REAL engine code: run the 125 C++ constructors
 * (the only substantial engine code path drivable without the JVM) and measure the actual
 * guest-instruction count + unhooked TCG throughput. This is much closer to per-frame
 * Box2D/render code than the synthetic loops in test_perf.c. */
#include "dispatch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec + t.tv_nsec/1e9; }
static void count_cb(uc_engine*uc,uint64_t a,uint32_t s,void*u){ (void)uc;(void)a;(void)s; (*(uint64_t*)u)++; }

static uint8_t* load_file(const char*path,long*len){
    FILE*fp=fopen(path,"rb"); if(!fp) return NULL;
    fseek(fp,0,SEEK_END); *len=ftell(fp); fseek(fp,0,SEEK_SET);
    uint8_t*e=malloc(*len); if(fread(e,1,*len,fp)!=(size_t)*len){ fclose(fp); free(e); return NULL; } fclose(fp); return e;
}
/* run the ctors; if count!=NULL install a per-insn counting hook. returns wall time. */
static double run_ctors(const uint8_t*elf,long len,uint64_t*count,int*ran,int*tot){
    cpu_t cpu; cpu_create(&cpu); loader_t L; loader_load(&L,&cpu,elf,len); dispatch_t D; dispatch_install(&D,&cpu,&L);
    uc_hook h; if(count){ *count=0; uc_hook_add(cpu.uc,&h,UC_HOOK_CODE,(void*)count_cb,count,1,0); }
    double t0=now(); *ran=dispatch_run_init_array(&D,tot); double dt=now()-t0;
    if(count) uc_hook_del(cpu.uc,h);
    loader_free(&L); cpu_destroy(&cpu); return dt;
}

int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    const char*path=getenv("ABSHIM_ENGINE_SO");
    if(!path) path="work803/libv7/libAngryBirdsClassic.so";
    long len; uint8_t*elf=load_file(path,&len); if(!elf){ printf("SKIP: no engine\n"); return 0; }
    printf("=== SR1 calibration on REAL engine code (125 C++ constructors) ===\n");
    uint64_t insns=0; int ran=0,tot=0;
    double th=run_ctors(elf,len,&insns,&ran,&tot);
    double tu=run_ctors(elf,len,NULL,&ran,&tot);
    free(elf);
    if(insns==0 || tu<=0){ printf("  (no instructions counted)\n"); return 0; }
    double rate=insns/1e6/tu;                         /* Minsn/s on this x86 host */
    printf("  ctors ran: %d/%d\n", ran, tot);
    printf("  guest instructions: %.3f Minsn\n", insns/1e6);
    printf("  unhooked time: %.4f s  ->  %.0f Minsn/s on REAL engine code (host)\n", tu, rate);
    printf("  (hooked %.3fs, hook overhead x%.1f)\n", th, th/tu);
    double a56=rate*0.40;                             /* A56 ~0.4x this desktop's single-thread rate (conservative) */
    printf("\n  Estimated A56 rate (~0.4x host): %.0f Minsn/s.\n", a56);
    printf("  Per-frame budget @60fps = 16.7 ms => a frame may be up to %.2f Minsn of guest code.\n", a56*0.0167);
    printf("  Typical 2009 physics frame ~1-2 Minsn => ~%.1f-%.1f ms (%.0f-%.0f fps).\n",
           1.0/a56*1000, 2.0/a56*1000, 1000.0/(1.0/a56*1000), 1000.0/(2.0/a56*1000));
    return 0;
}
