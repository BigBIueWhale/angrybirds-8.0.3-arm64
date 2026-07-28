/* test_elf32.c — runs the loader against the REAL engine .so and asserts its
 * symbol classification + reloc/init enumeration match the audit findings
 * (readelf-verified). This is the correctness gate for the live X1/B1-1 fixes.
 *
 * Engine path from $ABSHIM_ENGINE_SO (exported by run_tests.sh), else a default.
 */
#include "elf32.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails=0;
#define CK(c,msg) do{ if(!(c)){ printf("  FAIL: %s\n",msg); fails++; } }while(0)

static const char* cls_name(int c){
    switch(c){case SC_GUEST_FUNC:return"GUEST_FUNC";case SC_GUEST_OBJECT:return"GUEST_OBJECT";
              case SC_UND_FUNC:return"UND_FUNC";case SC_UND_OBJECT:return"UND_OBJECT";
              case SC_WEAK_ZERO:return"WEAK_ZERO";default:return"?";}
}

static elf32_image IMG;
static void expect(const char*name, int want){
    uint32_t si;
    if (elf32_find_symbol(&IMG,name,&si)!=0){ printf("  FAIL: symbol %s not found\n",name); fails++; return; }
    elf32_symres r=elf32_classify(&IMG,si);
    if (r.cls!=want){ printf("  FAIL: %s classified %s, want %s\n",name,cls_name(r.cls),cls_name(want)); fails++; }
    else printf("    %-42s -> %s%s\n",name,cls_name(r.cls), r.thumb?" (thumb)":"");
}

struct rc { uint32_t rel,glob,jump,abs,other; };
static void reloc_cb(void*ud,uint32_t off,uint32_t type,uint32_t sym){
    (void)off;(void)sym; struct rc*c=ud;
    switch(type){case 23:c->rel++;break;case 21:c->glob++;break;case 22:c->jump++;break;case 2:c->abs++;break;default:c->other++;}
}

int main(int argc,char**argv){
    setvbuf(stdout,NULL,_IONBF,0);
    printf("=== elf32 host test (real engine binary) ===\n");
    const char*path = argc>1 ? argv[1] : getenv("ABSHIM_ENGINE_SO");
    if (!path) path = "/home/user/original_angry_birds/apk-binary-analysis/work803/libv7/libAngryBirdsClassic.so";

    FILE*f=fopen(path,"rb");
    if(!f){ printf("  SKIP: engine .so not found at %s\n",path); return 0; }
    fseek(f,0,SEEK_END); long len=ftell(f); fseek(f,0,SEEK_SET);
    uint8_t*buf=malloc(len); if(fread(buf,1,len,f)!=(size_t)len){printf("  FAIL: read\n");return 1;} fclose(f);
    printf("  loaded %ld bytes from %s\n",len,path);

    int pr=elf32_parse(buf,len,&IMG);
    CK(pr==0,"elf32_parse ok");
    if(pr!=0){ printf("  parse rc=%d\n",pr); return 1; }

    /* REJECTION PATHS. This test only ever fed elf32_parse a VALID engine .so, so every guard in the
     * parser was untested: mutation_modules.sh showed that deleting the ELF-magic check outright
     * left the whole test passing. A loader that accepts arbitrary bytes is a loader that will map
     * garbage as code, so the negative cases are now asserted alongside the positive one.
     * Added 2026-07-28. */
    { elf32_image bad; uint8_t junk[64];
      memset(junk,0xA5,sizeof junk);
      CK(elf32_parse(junk,sizeof junk,&bad)!=0,"rejects non-ELF bytes");
      uint8_t trunc[8] = {0x7f,'E','L','F',1,1,1,0};
      CK(elf32_parse(trunc,sizeof trunc,&bad)!=0,"rejects truncated ELF header");
      /* right magic, wrong class/endianness/machine — a 64-bit or big-endian object must not load */
      uint8_t *wrongclass = malloc(len); memcpy(wrongclass,buf,len);
      wrongclass[4] = 2;                                   /* ELFCLASS64 */
      CK(elf32_parse(wrongclass,len,&bad)!=0,"rejects ELFCLASS64");
      free(wrongclass);
      CK(elf32_parse(buf,0,&bad)!=0,"rejects zero length");
      /* The case that isolates the MAGIC check specifically. The three above are all rejected by
       * OTHER guards too (bounds, class), so deleting the magic check left them passing and the
       * mutation invisible — mutation_modules.sh reported elf32 NOT DETECTED even with the rejection
       * tests present. This one is a byte-for-byte valid ELF32 whose e_ident magic alone is wrong,
       * so nothing downstream objects to it and only the magic check can catch it. */
      uint8_t *badmagic = malloc(len); memcpy(badmagic,buf,len);
      badmagic[0] = 0x7e;                                  /* 0x7f -> 0x7e: everything else valid */
      CK(elf32_parse(badmagic,len,&bad)!=0,"rejects valid ELF32 with wrong magic");
      free(badmagic);
    }

    printf("  image_size=0x%x symcount=%u  symtab@0x%x strtab@0x%x hash@0x%x\n",
           IMG.image_size,IMG.symcount,IMG.symtab_va,IMG.strtab_va,IMG.hash_va);
    CK(IMG.symcount>1000,"symcount plausible");
    CK(IMG.image_size>=0xA00000 && IMG.image_size<=0xC00000,"image ~11MiB (Audit 05: 0xAF0000)");

    printf("  -- classification (must match audits) --\n");
    /* The weak-UND exidx finder is now a real BRIDGE (UND_FUNC -> stub -> dispatch
     * h_find_exidx), NOT weak-zero: the engine statically links the C++ unwinder but leaves
     * the finder weak-UND and defines NO __exidx_start/end fallback, so it must be handed the
     * real PT_ARM_EXIDX table or every throw -> std::terminate. The table must be captured. */
    expect("__gnu_Unwind_Find_exidx", SC_UND_FUNC);
    CK(IMG.exidx_va==0x9bf3e4u && IMG.exidx_sz==0x23ae8u, "PT_ARM_EXIDX captured (va=0x9bf3e4 sz=0x23ae8)");
    /* the two Google profiling hooks are the only other ->0 symbols */
    expect("__google_potentially_blocking_region_begin", SC_WEAK_ZERO);
    expect("__google_potentially_blocking_region_end",   SC_WEAK_ZERO);
    /* B1-1: the data-object imports need real GUESTDATA mirrors */
    expect("_ctype_",            SC_UND_OBJECT);
    expect("__sF",               SC_UND_OBJECT);
    expect("__stack_chk_guard",  SC_UND_OBJECT);
    expect("_tolower_tab_",      SC_UND_OBJECT);
    expect("_toupper_tab_",      SC_UND_OBJECT);
    /* UND functions -> host bridge stubs */
    expect("malloc",             SC_UND_FUNC);
    expect("free",               SC_UND_FUNC);
    expect("__aeabi_memset",     SC_UND_FUNC);
    expect("socket",             SC_UND_FUNC);
    expect("__errno",            SC_UND_FUNC);
    /* X8: weak pthread_* is NOT whitelisted -> a real bridge (stub), like strong-UND */
    expect("pthread_create",     SC_UND_FUNC);
    /* the entire C++ EH runtime is DEFINED in-guest (the key Audit-04 fact) */
    expect("__cxa_throw",             SC_GUEST_FUNC);
    expect("_Unwind_RaiseException",  SC_GUEST_FUNC);
    expect("__cxa_begin_catch",       SC_GUEST_FUNC);
    expect("__cxa_guard_acquire",     SC_GUEST_FUNC);
    expect("__aeabi_dadd",            SC_GUEST_FUNC);   /* static soft-float helper */
    /* an exported JNI entry is guest-defined */
    expect("JNI_OnLoad",              SC_GUEST_FUNC);
    expect("Java_com_rovio_fusion_NativeApplication_nativeUpdate", SC_GUEST_FUNC);

    printf("  -- relocations --\n");
    struct rc c={0,0,0,0,0}; elf32_foreach_reloc(&IMG,reloc_cb,&c);
    printf("    RELATIVE=%u GLOB_DAT=%u JUMP_SLOT=%u ABS32=%u other=%u\n",c.rel,c.glob,c.jump,c.abs,c.other);
    CK(c.rel>10000,"RELATIVE relocs plausible (~17k)");
    CK(c.glob>0 && c.jump>0,"GLOB_DAT + JUMP_SLOT present");
    CK(c.other==0,"no unexpected reloc types (only the 4 handled)");

    uint32_t iav; uint32_t ic=elf32_init_array(&IMG,&iav);
    printf("  -- init_array: %u ctors @0x%x --\n",ic,iav);
    CK(ic>100 && ic<200,"init_array ~126 (Audit: 125/126 ctors)");

    free(buf);
    printf(fails? "\n=== %d FAILURE(S) ===\n":"\n=== ALL PASS ===\n", fails);
    return fails?1:0;
}
