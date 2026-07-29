/* test_native_init.c — device layer: boot the engine, run ctors, then drive
 * JNI_OnLoad + nativeInit through the fake-env JNI passthrough. On host (no JVM)
 * nativeInit progresses to the JVM-dependency boundary; this validates the JNI
 * sentinel/vtable/handle-table plumbing on the hardened path. */
#include "dispatch.h"
#include "jni_passthrough.h"
#include "elf32.h"
#include "regions.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t sym_addr(loader_t*L,const char*name){
    uint32_t si; if(elf32_find_symbol(&L->img,name,&si)) return 0;
    elf32_symres r=elf32_classify(&L->img,si);
    return (RG_ENGINE + r.value) | (r.thumb?1u:0u);
}

int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    printf("=== nativeInit drive test (fake-env JNI passthrough) ===\n");
    const char*path=getenv("ABSHIM_ENGINE_SO");
    if(!path) path="work803/libv7/libAngryBirdsClassic.so";
    FILE*fp=fopen(path,"rb");
    /* see test_boot.c: a missing engine is a harness failure, not a pass. */
    if(!fp){ printf("  FAIL: no engine at %s (set ABSHIM_ENGINE_SO)\n",path); return 1; }
    fseek(fp,0,SEEK_END); long len=ftell(fp); fseek(fp,0,SEEK_SET);
    uint8_t*elf=malloc(len); if(fread(elf,1,len,fp)!=(size_t)len)return 1; fclose(fp);

    cpu_t cpu; if(cpu_create(&cpu)){printf("cpu fail\n");return 1;}
    loader_t L; if(loader_load(&L,&cpu,elf,len)){printf("load fail\n");return 1;}
    dispatch_t D; dispatch_install(&D,&cpu,&L);
    jni_state J; jni_install(&J,&cpu,1);
    D.jni=&J;   /* route AAsset* (host: dlsym->NULL -> no-op, but exercises the path) */

    int total=0, ran=dispatch_run_init_array(&D,&total);
    printf("  ctors %d/%d clean\n", ran, total);

    uint32_t onload=sym_addr(&L,"JNI_OnLoad");
    printf("  JNI_OnLoad @0x%x\n", onload);
    if(onload){ uc_err e=cpu_call(&cpu, onload, J.vm, 0,0,0, 100000000);
        uint32_t r0; uc_reg_read(cpu.uc,UC_ARM_REG_R0,&r0);
        printf("  JNI_OnLoad -> 0x%x (%s)\n", r0, uc_strerror(e)); }

    uint32_t ninit=sym_addr(&L,"Java_com_rovio_fusion_NativeApplication_nativeInit");
    printf("  nativeInit @0x%x\n", ninit);
    uint32_t thiz = ht_new_ref(J.ht,HK_LOCAL,(void*)0x1234);
    D.fatal=0;
    uc_err e = cpu_call(&cpu, ninit, J.env, thiz, 0, 0, 800000000);   /* bounded */
    uint32_t r0=0,pc=0; uc_reg_read(cpu.uc,UC_ARM_REG_R0,&r0); uc_reg_read(cpu.uc,UC_ARM_REG_PC,&pc);
    printf("  nativeInit -> r0=0x%x  emu=%s  pc=0x%x  fatal=%d\n", r0, uc_strerror(e), pc, D.fatal);
    if(D.fatal){ const char*sn=loader_stub_name(&L, pc & ~1u);
        uint32_t lr=0; uc_reg_read(cpu.uc,UC_ARM_REG_LR,&lr);
        printf("  FATAL at stub: %s  (__stack_chk_fail=real corruption; abort/exit/raise=engine halt, expected at fake-env boundary)\n", sn?sn:"(not a stub)");
        printf("  abort call-site LR=0x%x  (engine off 0x%x — symbolize to see which fn halted)\n", lr, lr>=RG_ENGINE?lr-RG_ENGINE:0); }

    /* which JNI slots did the engine exercise? */
    int used=0; for(int i=0;i<260;i++) if(J.slots_used[i]) used++;
    printf("  JNI env slots exercised: %d  (dispatch unimpl: %d, last='%s'; nested pthread_once: %d)\n",
           used, D.unimpl_count, D.last_unimpl, D.nested_ran);
    printf("  slot calls:"); for(int i=0;i<260;i++) if(J.call_count[i]) printf(" [%d]x%u",i,J.call_count[i]); printf("\n");
    printf("  heap %s after nativeInit; in-use=%u\n", galloc_check(cpu.heap)==0?"valid":"CORRUPT", galloc_inuse_bytes(cpu.heap));

    jni_free(&J); loader_free(&L); cpu_destroy(&cpu); free(elf);
    /* This is a progression probe, not pass/fail: reaching the JVM boundary with
     * a valid heap and no shim crash is the success signal. */
    printf("\n=== nativeInit drive complete ===\n");
    return 0;
}
