/* test_ctors.c — device layer: load the real engine, install the dispatch, and
 * EXECUTE the init_array constructors under emulation (the old PoC's milestone,
 * now on the hardened path: real allocator, X1-fixed EH, correct classification).
 */
#include "dispatch.h"
#include "regions.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    printf("=== ctor execution test ===\n");
    const char*path=getenv("ABSHIM_ENGINE_SO");
    if(!path) path="work803/libv7/libAngryBirdsClassic.so";
    FILE*fp=fopen(path,"rb"); if(!fp){ printf("  SKIP: no engine\n"); return 0; }
    fseek(fp,0,SEEK_END); long len=ftell(fp); fseek(fp,0,SEEK_SET);
    uint8_t*elf=malloc(len); if(fread(elf,1,len,fp)!=(size_t)len){return 1;} fclose(fp);

    cpu_t cpu; if(cpu_create(&cpu)){ printf("cpu_create failed\n"); return 1; }
    loader_t L; if(loader_load(&L,&cpu,elf,len)){ printf("loader_load failed\n"); return 1; }
    dispatch_t D; if(dispatch_install(&D,&cpu,&L)){ printf("dispatch_install failed\n"); return 1; }

    int total=0;
    int ran = dispatch_run_init_array(&D, &total);
    printf("  init_array: %d/%d constructors ran CLEAN  (unimpl bridge calls: %d, last='%s')\n",
           ran, total, D.unimpl_count, D.last_unimpl);

    int fails = (ran != total);
    if (fails) printf("  (stopped early — see the FAILED line above)\n");
    /* heap still consistent after all that C++ static init */
    if (galloc_check(cpu.heap)!=0){ printf("  FAIL: heap corrupt after ctors\n"); fails=1; }
    else printf("  heap valid after ctors; in-use bytes=%u\n", galloc_inuse_bytes(cpu.heap));

    /* functional EH: drive the C++ unwinder's exidx finder stub through the emulator, as the
     * guest's _Unwind_RaiseException would, and confirm it returns the engine's PT_ARM_EXIDX
     * table + entry count for an in-engine PC (a 0 here = std::terminate on every throw). */
    uint32_t estub=0;
    for(uint32_t i=0;i<L.stub_count;i++) if(L.stub_name[i] && !strcmp(L.stub_name[i],"__gnu_Unwind_Find_exidx")){ estub=RG_STUB+i*4; break; }
    uint32_t nrec = galloc_malloc(cpu.heap, 4);
    if(estub && nrec){
        gm_wr32(&cpu.mem, nrec, 0xEEEEEEEEu);
        uc_err e = cpu_call(&cpu, estub, RG_ENGINE+0x1000u, nrec, 0, 0, 1000000);
        uint32_t r0=0; uc_reg_read(cpu.uc, UC_ARM_REG_R0, &r0);
        uint32_t n = gm_rd32(&cpu.mem, nrec);
        uint32_t want_base = RG_ENGINE + L.img.exidx_va, want_n = L.img.exidx_sz/8u;
        if(e!=UC_ERR_OK || r0!=want_base || n!=want_n){
            printf("  FAIL: exidx finder e=%s r0=0x%x(want 0x%x) nrec=%u(want %u)\n", uc_strerror(e), r0, want_base, n, want_n); fails=1;
        } else printf("  exidx finder OK: base=0x%x count=%u (C++ throw can unwind)\n", r0, n);
    } else { printf("  FAIL: no exidx stub / no scratch\n"); fails=1; }

    loader_free(&L); cpu_destroy(&cpu); free(elf);
    printf(fails? "\n=== FAILURE ===\n":"\n=== ALL PASS ===\n");
    return fails?1:0;
}
