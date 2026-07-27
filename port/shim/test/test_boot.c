/* test_boot.c — device-layer integration: load the REAL engine into Unicorn via
 * cpu + loader (using elf32/galloc/ctype_tables), and verify the relocations
 * landed correctly in emulated guest memory. Runs on x86 (Unicorn emulates ARM).
 * Engine path from $ABSHIM_ENGINE_SO.
 */
#include "loader.h"
#include "regions.h"
#include "ctype_tables.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails=0;
#define CK(c,msg) do{ if(!(c)){ printf("  FAIL: %s\n",msg); fails++; } }while(0)

/* find the GOT offset (r_offset) of the first GLOB_DAT/JUMP_SLOT for a sym idx */
struct find { uint32_t si; uint32_t off; int got; };
static void find_cb(void*ud,uint32_t off,uint32_t type,uint32_t si){
    struct find*f=ud; if(f->got) return;
    if(si==f->si && (type==21||type==22)){ f->off=off; f->got=1; }
}

int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    printf("=== boot test (load real engine into Unicorn) ===\n");
    const char*path=getenv("ABSHIM_ENGINE_SO");
    if(!path) path="/home/user/original_angry_birds/apk-binary-analysis/work803/libv7/libAngryBirdsClassic.so";
    FILE*fp=fopen(path,"rb"); if(!fp){ printf("  SKIP: no engine at %s\n",path); return 0; }
    fseek(fp,0,SEEK_END); long len=ftell(fp); fseek(fp,0,SEEK_SET);
    uint8_t*elf=malloc(len); if(fread(elf,1,len,fp)!=(size_t)len){printf("read fail\n");return 1;} fclose(fp);

    cpu_t cpu;
    CK(cpu_create(&cpu)==0,"cpu_create");
    /* the allocator works over REAL unicorn guest memory */
    uint32_t p=galloc_malloc(cpu.heap,64), q=galloc_malloc(cpu.heap,64);
    CK(p && q && p!=q && (p&0xF)==0,"galloc over unicorn heap");
    CK(galloc_check(cpu.heap)==0,"heap valid over unicorn");
    galloc_free(cpu.heap,p); galloc_free(cpu.heap,q);

    loader_t L;
    int rc=loader_load(&L,&cpu,elf,len);
    CK(rc==0,"loader_load ok");
    if(rc){ printf("  rc=%d err=%d\n",rc,L.err); return 1; }
    printf("  image_size=0x%x  stubs=%u  gd_used=%u  init_array=%u ctors @0x%x\n",
           L.img.image_size, L.stub_count, L.gd_next, L.init_count, L.init_va);
    CK(L.init_count==126,"126 init_array ctors");
    CK(L.stub_count>200 && L.stub_count<400,"UND-func stubs plausible (~344 jumpslots + globdat funcs)");
    CK(L.err==0,"no unhandled reloc type");

    guest_mem*m=&cpu.mem;
    /* -- __gnu_Unwind_Find_exidx GOT slot must now resolve to a BRIDGE STUB (not 0), so the
     *    engine's static C++ unwinder reaches dispatch h_find_exidx and a throw can unwind
     *    (corrects the old X1 rule: 0 there = std::terminate on every throw). -- */
    uint32_t si;
    if(elf32_find_symbol(&L.img,"__gnu_Unwind_Find_exidx",&si)==0){
        struct find f={si,0,0}; elf32_foreach_reloc(&L.img,find_cb,&f);
        CK(f.got,"exidx has a GOT reloc");
        if(f.got){ uint32_t v=gm_rd32(m, RG_ENGINE+f.off);
                   printf("  __gnu_Unwind_Find_exidx GOT@0x%x = 0x%x\n",f.off,v);
                   CK(v>=RG_STUB && v<RG_STUB+RG_STUB_SZ,"exidx GOT -> bridge stub (C++ throw can unwind)"); }
    }
    /* -- B1-1: _ctype_ GOT points into GUESTDATA, mirror content correct -- */
    if(elf32_find_symbol(&L.img,"_ctype_",&si)==0){
        struct find f={si,0,0}; elf32_foreach_reloc(&L.img,find_cb,&f);
        if(f.got){ uint32_t mir=gm_rd32(m, RG_ENGINE+f.off);
                   printf("  _ctype_ GOT -> 0x%x (GUESTDATA)\n",mir);
                   CK(mir>=RG_GUESTDATA && mir<RG_GUESTDATA+RG_GUESTDATA_SZ,"_ctype_ mirror in GUESTDATA");
                   /* _ctype_ is a POINTER variable: the symbol resolves to a pointer WORD holding &table[0];
                    * the engine does `ldr ptr,[_ctype_]; ldrb [ptr + c + 1]` (T[0]=EOF slot, T[c+1]=classify(c)).
                    * So deref the pointer word first, then index c+1. */
                   uint32_t tbl=gm_rd32(m, mir);
                   uint8_t bA=gm_rd8(m, tbl + ('A'+1));      /* _ctype_[c+1] */
                   CK((bA&_CT_U)&&(bA&_CT_X),"mirror _ctype_['A'+1] = upper|hex");
                   uint8_t b0=gm_rd8(m, tbl + ('0'+1));
                   CK((b0&_CT_N)&&(b0&_CT_X),"mirror _ctype_['0'+1] = digit|hex"); }
    }
    /* -- guest-defined func GOT points into ENGINE with the Thumb bit -- */
    if(elf32_find_symbol(&L.img,"__cxa_begin_catch",&si)==0){
        struct find f={si,0,0}; elf32_foreach_reloc(&L.img,find_cb,&f);
        if(f.got){ uint32_t v=gm_rd32(m, RG_ENGINE+f.off);
                   printf("  __cxa_begin_catch GOT -> 0x%x\n",v);
                   CK((v&~1u)>=RG_ENGINE && (v&~1u)<RG_ENGINE+L.img.image_size,"guest-func GOT in ENGINE");
                   CK(v&1,"guest Thumb func GOT has bit0 set"); }
    }
    /* -- malloc GOT points into the STUB arena -- */
    if(elf32_find_symbol(&L.img,"malloc",&si)==0){
        struct find f={si,0,0}; elf32_foreach_reloc(&L.img,find_cb,&f);
        if(f.got){ uint32_t v=gm_rd32(m, RG_ENGINE+f.off);
                   CK(v>=RG_STUB && v<RG_STUB+RG_STUB_SZ,"malloc GOT in STUB arena");
                   CK(loader_stub_name(&L,v) && !strcmp(loader_stub_name(&L,v),"malloc"),"stub name maps back to malloc"); }
    }
    /* -- init_array entries are RELATIVE-relocated into ENGINE (or a 0/-1 sentinel
          that the executor skips, as the PoC did) -- */
    int good=0, sentinel=0; for(uint32_t i=0;i<L.init_count;i++){ uint32_t e=gm_rd32(m, RG_ENGINE+L.init_va+i*4);
        if(e==0 || e==0xffffffffu){ sentinel++; printf("  init_array[%u] = 0x%x (sentinel, skipped)\n",i,e); }
        else if((e&~1u)>=RG_ENGINE && (e&~1u)<RG_ENGINE+L.img.image_size) good++; }
    printf("  init_array: %d relocated into ENGINE + %d sentinel = %u\n",good,sentinel,L.init_count);
    CK((uint32_t)(good+sentinel)==L.init_count,"every init_array entry is an ENGINE ctor or a skip-sentinel");
    CK(good>=125,"at least 125 real ctors");

    loader_free(&L); cpu_destroy(&cpu); free(elf);
    printf(fails? "\n=== %d FAILURE(S) ===\n":"\n=== ALL PASS ===\n", fails);
    return fails?1:0;
}
