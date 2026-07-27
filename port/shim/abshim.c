// abshim.c — arm64 JNI shim that runs the 32-bit libAngryBirdsClassic.so under Unicorn.
// Direct C port of the proven port/poc_load.py. Built for arm64-v8a/android-24, linked
// against libunicorn-static.a + libunicorn-common.a + libarm-softmmu.a.
//
// Architecture (all validated in the Python PoC):
//   - map the 32-bit engine into a Unicorn ARM CPU, apply R_ARM relocations
//   - import calls trap to bx-lr stubs -> dispatch(): libc/GLES forwarded to the REAL
//     arm64 libs via dlsym; JNIEnv/JavaVM calls forwarded to the REAL Android env
//   - the 72 Java_* exports are re-exported here as arm64 thunks that enter emulation
//
// STATUS: foundation (loader + unicorn init + stub framework). Bridge table + JNIEnv
// vtable marshalling + the 72 entry thunks are the remaining Stage-4/5 build-out.
#include <unicorn/unicorn.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <elf.h>
#define TAG "abshim"
#ifndef SHIM_TEST
#include <jni.h>
#include <android/log.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <GLES2/gl2.h>
#include <EGL/egl.h>
#define LOGI(...) __android_log_print(4, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(6, TAG, __VA_ARGS__)
#else
#define LOGI(...) do{ printf("[I] "); printf(__VA_ARGS__); printf("\n"); }while(0)
#define LOGE(...) do{ printf("[E] "); printf(__VA_ARGS__); printf("\n"); }while(0)
#endif

// ---- emulated address map (mirrors poc_load.py) ----
#define BASE   0x40000000u
#define STACK  0x70000000u
#define STACK_SZ 0x00200000u
#define STUB   0x10000000u
#define STUB_SZ  0x00020000u
#define HEAP   0x50000000u
#define HEAP_SZ  0x08000000u
#define KUSER  0xffff0000u
#define RET    0xdead0000u

#define R_ARM_ABS32 2
#define R_ARM_GLOB_DAT 21
#define R_ARM_JUMP_SLOT 22
#define R_ARM_RELATIVE 23

static uc_engine *uc;
static uint8_t   *g_elf;        // raw 32-bit .so bytes
static size_t     g_elf_len;
static uint32_t   g_heap_ptr = HEAP;

// import stub index -> imported symbol name (built during reloc)
#define MAX_IMPORTS 512
static const char *g_import_name[MAX_IMPORTS];
static int         g_import_count = 0;

static uint32_t balloc(uint32_t n){ n=(n+15)&~15u; uint32_t p=g_heap_ptr; g_heap_ptr+=n; return p; }
static uint32_t rd32(uint32_t a){ uint32_t v; uc_mem_read(uc,a,&v,4); return v; }
static void     wr32(uint32_t a,uint32_t v){ uc_mem_write(uc,a,&v,4); }

// ---- ELF32 loader ----
static Elf32_Sym *g_symtab=NULL;
static const char *g_strtab=NULL;
static uint32_t g_sym_stub[8192];
static void apply_rel(Elf32_Rel *r, uint32_t sz){
    for (uint32_t i=0;i<sz/sizeof(Elf32_Rel);i++){
        uint32_t off=BASE+r[i].r_offset, t=ELF32_R_TYPE(r[i].r_info), si=ELF32_R_SYM(r[i].r_info);
        uint32_t cur=rd32(off);
        if (t==R_ARM_RELATIVE){ wr32(off, cur+BASE); }
        else if (t==R_ARM_JUMP_SLOT || t==R_ARM_GLOB_DAT || t==R_ARM_ABS32){
            Elf32_Sym *s=&g_symtab[si]; uint32_t a;
            if (s->st_shndx==SHN_UNDEF){
                if (!g_sym_stub[si] && g_import_count<MAX_IMPORTS){
                    g_sym_stub[si]=STUB + g_import_count*4;
                    g_import_name[g_import_count]=g_strtab+s->st_name;
                    g_import_count++;
                }
                a=g_sym_stub[si];
            } else a=BASE+s->st_value;
            wr32(off, (t==R_ARM_ABS32)? a+cur : a);
        }
    }
}
static int load_engine(void){
    Elf32_Ehdr *eh = (Elf32_Ehdr*)g_elf;
    Elf32_Phdr *ph = (Elf32_Phdr*)(g_elf + eh->e_phoff);

    // image size = max(vaddr+memsz) of PT_LOAD, page-rounded
    uint32_t maxva = 0;
    for (int i=0;i<eh->e_phnum;i++)
        if (ph[i].p_type==PT_LOAD){ uint32_t e=ph[i].p_vaddr+ph[i].p_memsz; if(e>maxva)maxva=e; }
    uint32_t image_sz = (maxva + 0xfff) & ~0xfffu;

    if (uc_mem_map(uc, BASE, image_sz, UC_PROT_ALL)) return -1;
    for (int i=0;i<eh->e_phnum;i++)
        if (ph[i].p_type==PT_LOAD)
            uc_mem_write(uc, BASE+ph[i].p_vaddr, g_elf+ph[i].p_offset, ph[i].p_filesz);

    uc_mem_map(uc, STACK, STACK_SZ, UC_PROT_ALL);
    uc_mem_map(uc, STUB,  STUB_SZ,  UC_PROT_ALL);
    uc_mem_map(uc, HEAP,  HEAP_SZ,  UC_PROT_ALL);
    uc_mem_map(uc, RET & ~0xfffu, 0x1000, UC_PROT_ALL);
    // fill import stubs with ARM `bx lr` (e12fff1e) for native interworking returns
    uint32_t bxlr = 0xe12fff1e;
    for (uint32_t o=0;o<STUB_SZ;o+=4) uc_mem_write(uc, STUB+o, &bxlr, 4);

    // enable VFP/NEON
    uint32_t cpacr; uc_reg_read(uc, UC_ARM_REG_C1_C0_2, &cpacr);
    cpacr |= (0xf<<20); uc_reg_write(uc, UC_ARM_REG_C1_C0_2, &cpacr);
    uint32_t fpexc = 0x40000000; uc_reg_write(uc, UC_ARM_REG_FPEXC, &fpexc);

    // kuser helper page
    uc_mem_map(uc, KUSER, 0x1000, UC_PROT_ALL);
    for (uint32_t o=0;o<0x1000;o+=4) uc_mem_write(uc, KUSER+o, &bxlr, 4);

    // --- dynamic section: find SYMTAB, STRTAB, REL, JMPREL ---
    Elf32_Dyn *dyn=NULL;
    for (int i=0;i<eh->e_phnum;i++) if (ph[i].p_type==PT_DYNAMIC) dyn=(Elf32_Dyn*)(g_elf+ph[i].p_offset);
    if (!dyn){ LOGE("no PT_DYNAMIC"); return -1; }
    Elf32_Rel *rel=NULL; uint32_t relsz=0; Elf32_Rel *jmprel=NULL; uint32_t pltrelsz=0;
    for (Elf32_Dyn *d=dyn; d->d_tag!=DT_NULL; d++){
        uint32_t v=d->d_un.d_val;
        switch(d->d_tag){
            case DT_SYMTAB: g_symtab=(Elf32_Sym*)(g_elf+v); break;  // p_offset 0 -> file offset == vaddr
            case DT_STRTAB: g_strtab=(const char*)(g_elf+v); break;
            case DT_REL:    rel=(Elf32_Rel*)(g_elf+v); break;
            case DT_RELSZ:  relsz=v; break;
            case DT_JMPREL: jmprel=(Elf32_Rel*)(g_elf+v); break;
            case DT_PLTRELSZ: pltrelsz=v; break;
        }
    }
    if (rel)    apply_rel(rel, relsz);
    if (jmprel) apply_rel(jmprel, pltrelsz);
    LOGI("engine mapped @%08x size %08x, %d imports to bridge", BASE, image_sz, g_import_count);
    return 0;
}

// ---- bridge dispatch (direct C port of poc_load.py bridge()) ----
#include <math.h>
static uint32_t Rg(int i){ uint32_t v; uc_reg_read(uc,UC_ARM_REG_R0+i,&v); return v; }
static void S0(uint32_t v){ uc_reg_write(uc,UC_ARM_REG_R0,&v); }
static void Si(int i,uint32_t v){ uc_reg_write(uc,UC_ARM_REG_R0+i,&v); }
static int em_str(uint32_t p,char*b,int max){ int i=0; for(;i<max-1;i++){ uint8_t c; uc_mem_read(uc,p+i,&c,1); if(!c)break; b[i]=(char)c; } b[i]=0; return i; }
static void em_copy(uint32_t d,uint32_t s,uint32_t n){ uint8_t t[8192]; while(n){ uint32_t k=n<sizeof t?n:sizeof t; uc_mem_read(uc,s,t,k); uc_mem_write(uc,d,t,k); s+=k;d+=k;n-=k; } }
static void em_set(uint32_t d,int c,uint32_t n){ uint8_t t[4096]; memset(t,c,n<sizeof t?(size_t)n:sizeof t); while(n){ uint32_t k=n<sizeof t?n:sizeof t; uc_mem_write(uc,d,t,k); d+=k;n-=k; } }
static double d_in(void){ uint64_t b=((uint64_t)Rg(1)<<32)|Rg(0); double d; memcpy(&d,&b,8); return d; }
static void d_out(double x){ uint64_t b; memcpy(&b,&x,8); Si(0,(uint32_t)b); Si(1,(uint32_t)(b>>32)); }
#define EQ(s)  (!strcmp(nm,(s)))
#define PRE(s) (!strncmp(nm,(s),strlen(s)))
static FILE *g_am[256]; static int g_amn=0;   /* SHIM_TEST asset FILE handles */
static void dispatch(const char *nm){
    uint32_t a0=Rg(0),a1=Rg(1),a2=Rg(2);
    if(EQ("malloc")||EQ("valloc")){ S0(balloc(a0?a0:1)); return; }
    if(EQ("calloc")){ uint32_t n=a0*a1,p=balloc(n?n:1); em_set(p,0,n); S0(p); return; }
    if(EQ("realloc")){ uint32_t p=balloc(a1?a1:1); if(a0&&a1)em_copy(p,a0,a1); S0(p); return; }
    if(EQ("free")){ S0(0); return; }
    if(PRE("memcpy")||EQ("memmove")||PRE("__aeabi_memcpy")||PRE("__aeabi_memmove")){ em_copy(a0,a1,a2); S0(a0); return; }
    if(PRE("memset")||PRE("__aeabi_memset")){ em_set(a0,(int)(a1&0xff),a2); S0(a0); return; }
    if(PRE("__aeabi_memclr")){ em_set(a0,0,a1); S0(a0); return; }
    if(EQ("memcmp")){ for(uint32_t i=0;i<a2;i++){ uint8_t x,y; uc_mem_read(uc,a0+i,&x,1); uc_mem_read(uc,a1+i,&y,1); if(x!=y){ S0((uint32_t)((int)x-(int)y)); return; } } S0(0); return; }
    if(EQ("strlen")){ char b[8192]; S0((uint32_t)em_str(a0,b,sizeof b)); return; }
    if(EQ("strcmp")){ char x[2048],y[2048]; em_str(a0,x,sizeof x); em_str(a1,y,sizeof y); S0((uint32_t)strcmp(x,y)); return; }
    if(EQ("strncmp")){ char x[2048],y[2048]; em_str(a0,x,sizeof x); em_str(a1,y,sizeof y); S0((uint32_t)(a2?strncmp(x,y,a2):0)); return; }
    if(EQ("strrchr")){ char b[8192]; em_str(a0,b,sizeof b); char*p=strrchr(b,(int)a1); S0(p?a0+(uint32_t)(p-b):0); return; }
    if(EQ("strchr")){ char b[8192]; em_str(a0,b,sizeof b); char*p=strchr(b,(int)a1); S0(p?a0+(uint32_t)(p-b):0); return; }
    if(EQ("__android_log_print")||EQ("__android_log_write")){ char b[512]; em_str(EQ("__android_log_print")?a2:a1,b,sizeof b); LOGI("[engine] %s",b); S0(0); return; }
    if(EQ("__cxa_atexit")||EQ("__cxa_finalize")||EQ("atexit")||EQ("__cxa_guard_release")||EQ("__register_atfork")){ S0(0); return; }
    if(EQ("__cxa_guard_acquire")){ S0(1); return; }
    if(EQ("__errno")){ S0(HEAP+HEAP_SZ-0x100); return; }
    if(EQ("ceil")){ d_out(ceil(d_in())); return; }
    if(EQ("floor")){ d_out(floor(d_in())); return; }
    if(EQ("sysconf")||EQ("getpagesize")){ S0(4096); return; }
    if(EQ("getauxval")){ S0(a0==16?0x1000:0); return; }
    if(PRE("pthread")||EQ("__google_potentially_blocking_region_begin")||EQ("__google_potentially_blocking_region_end")){ S0(0); return; }
    if(EQ("open")||EQ("openat")||EQ("open64")){ S0(3); return; }
    if(EQ("read")||EQ("close")||EQ("lseek")||EQ("lseek64")){ S0(0); return; }
    if(EQ("btowc")){ S0(a0); return; }
    if(EQ("wctob")){ S0(a0<0x80?a0:0xffffffffu); return; }
    if(EQ("wctype")){ S0(1); return; }
    if(EQ("strcpy")||EQ("stpcpy")){ char b[8192]; int L=em_str(a1,b,sizeof b); em_copy(a0,a1,L+1); S0(a0); return; }
    if(EQ("strncpy")){ char b[8192]; int L=em_str(a1,b,sizeof b); uint32_t k=((uint32_t)L<a2)?(uint32_t)L:a2; em_copy(a0,a1,k); if(k<a2)em_set(a0+k,0,a2-k); S0(a0); return; }
    if(EQ("strcat")||EQ("strncat")){ char d[8192]; int dl=em_str(a0,d,sizeof d); char s[8192]; int sl=em_str(a1,s,sizeof s); if(EQ("strncat")&&(uint32_t)sl>a2)sl=a2; em_copy(a0+dl,a1,sl); uint8_t z=0; uc_mem_write(uc,a0+dl+sl,&z,1); S0(a0); return; }
    if(EQ("strstr")){ char h[8192],q[1024]; em_str(a0,h,sizeof h); em_str(a1,q,sizeof q); char*p=strstr(h,q); S0(p?a0+(uint32_t)(p-h):0); return; }
    if(EQ("strcasecmp")||EQ("strncasecmp")){ char x[2048],y[2048]; em_str(a0,x,sizeof x); em_str(a1,y,sizeof y); S0((uint32_t)(EQ("strncasecmp")?strncasecmp(x,y,a2):strcasecmp(x,y))); return; }
    if(EQ("strdup")){ char b[8192]; int L=em_str(a0,b,sizeof b); uint32_t p=balloc(L+1); em_copy(p,a0,L+1); S0(p); return; }
    if(EQ("memchr")){ for(uint32_t i=0;i<a2;i++){ uint8_t c; uc_mem_read(uc,a0+i,&c,1); if(c==(a1&0xff)){ S0(a0+i); return; } } S0(0); return; }
    if(EQ("toupper")){ int c=(int)a0; S0((c>=97&&c<=122)?c-32:c); return; }
    if(EQ("tolower")){ int c=(int)a0; S0((c>=65&&c<=90)?c+32:c); return; }
    if(EQ("__gnu_Unwind_Find_exidx")||EQ("dl_unwind_find_exidx")||EQ("__cxa_begin_catch")||EQ("__cxa_end_catch")||EQ("setlocale")){ S0(0); return; }
    if(EQ("abort")||EQ("__stack_chk_fail")){ LOGE("[engine] %s() from lr", nm); uc_emu_stop(uc); S0(0); return; }
    if(nm[0]=='g'&&nm[1]=='l'){ static uint32_t gid=0x1000;      /* GLES catch-all (no real GL in host test) */
        if(EQ("glCreateProgram")||EQ("glCreateShader")||EQ("glGetUniformLocation")||EQ("glGetAttribLocation")){ S0(++gid); return; }
        if(EQ("glGenTextures")||EQ("glGenBuffers")||EQ("glGenFramebuffers")||EQ("glGenRenderbuffers")){ for(uint32_t i=0;i<a0;i++){ uint32_t v=++gid; uc_mem_write(uc,a1+i*4,&v,4); } S0(0); return; }
        if(EQ("glGetShaderiv")||EQ("glGetProgramiv")){ uint32_t one=1; uc_mem_write(uc,a2,&one,4); S0(0); return; }
        S0(0); return;
    }
    if(EQ("sprintf")||EQ("snprintf")||EQ("vsprintf")||EQ("vsnprintf")){   /* degenerate: copy format (TODO varargs) */
        uint32_t fp=(EQ("snprintf")||EQ("vsnprintf"))?a2:a1; char f[2048]; int L=em_str(fp,f,sizeof f); em_copy(a0,fp,L+1); S0(L); return; }
    if(EQ("printf")){ char f[1024]; em_str(a0,f,sizeof f); LOGI("[engine] %s",f); S0(0); return; }
#ifdef SHIM_TEST
    if(EQ("AAssetManager_fromJava")){ S0(0x41000000u); return; }
    if(EQ("AAssetManager_open")){ char p[1024]; em_str(a1,p,sizeof p); char full[1300]; snprintf(full,sizeof full,"/work/work803/assets/%s",p); FILE*f=fopen(full,"rb"); if(!f||g_amn>=256){ if(f)fclose(f); S0(0); return; } g_am[g_amn]=f; S0(0x42000000u+(uint32_t)g_amn); g_amn++; return; }
    if(EQ("AAsset_read")){ int i=(int)(a0-0x42000000u); if(i<0||i>=g_amn||!g_am[i]){ S0(0xffffffffu); return; } uint8_t b[8192]; uint32_t tot=0; while(tot<a2){ uint32_t k=(a2-tot)<sizeof b?(a2-tot):sizeof b; size_t r=fread(b,1,k,g_am[i]); if(!r)break; uc_mem_write(uc,a1+tot,b,r); tot+=r; } S0(tot); return; }
    if(EQ("AAsset_getLength")||EQ("AAsset_getLength64")){ int i=(int)(a0-0x42000000u); if(i<0||i>=g_amn||!g_am[i]){ S0(0); return; } long c=ftell(g_am[i]); fseek(g_am[i],0,SEEK_END); long L=ftell(g_am[i]); fseek(g_am[i],c,SEEK_SET); S0((uint32_t)L); return; }
    if(EQ("AAsset_seek")||EQ("AAsset_seek64")){ int i=(int)(a0-0x42000000u); if(i<0||i>=g_amn||!g_am[i]){ S0(0xffffffffu); return; } fseek(g_am[i],(long)a1,(int)a2); S0((uint32_t)ftell(g_am[i])); return; }
    if(EQ("AAsset_getBuffer")){ int i=(int)(a0-0x42000000u); if(i<0||i>=g_amn||!g_am[i]){ S0(0); return; } fseek(g_am[i],0,SEEK_END); long L=ftell(g_am[i]); fseek(g_am[i],0,SEEK_SET); uint32_t pp=balloc((uint32_t)L+16); uint8_t b[8192]; uint32_t o=0; size_t r; while((r=fread(b,1,sizeof b,g_am[i]))>0){ uc_mem_write(uc,pp+o,b,r); o+=r; } S0(pp); return; }
    if(EQ("AAsset_close")){ int i=(int)(a0-0x42000000u); if(i>=0&&i<g_amn&&g_am[i]){ fclose(g_am[i]); g_am[i]=NULL; } S0(0); return; }
#endif
    { static int nl=0; if(nl<80){ LOGE("UNIMPL %s", nm); nl++; } S0(0); }
}
static const char *import_at(uint32_t addr){ uint32_t i=(addr-STUB)/4; return i<(uint32_t)g_import_count?g_import_name[i]:"?"; }
static void stub_hook(uc_engine *u, uint64_t addr, uint32_t size, void *ud){ (void)u;(void)size;(void)ud; dispatch(import_at((uint32_t)addr)); }
static void kuser_hook(uc_engine *u, uint64_t addr, uint32_t size, void *ud){ (void)u;(void)size;(void)ud;
    if(addr==0xffff0fe0u){ Si(0, HEAP+HEAP_SZ-0x2000); }                 /* __kuser_get_tls */
    else if(addr==0xffff0fc0u){ uint32_t old=Rg(0),nw=Rg(1),p=Rg(2),cur; uc_mem_read(uc,p,&cur,4); if(cur==old){ uc_mem_write(uc,p,&nw,4); S0(0);} else S0(1);} /* cmpxchg */
    /* 0xffff0fa0 __kuser_memory_barrier: no-op; page holds bx lr */
}
static void intr_hook(uc_engine *u, uint32_t intno, void *ud){ (void)u;(void)intno;(void)ud;
    uint32_t nr; uc_reg_read(uc,UC_ARM_REG_R7,&nr); uint32_t r=(nr==20||nr==224)?1234:0; uc_reg_write(uc,UC_ARM_REG_R0,&r);
}
/* --- JNIEnv/JavaVM fake tables (host test only; shipping forwards to the REAL env) --- */
static uint32_t g_ENV=0, g_VM=0;
#define JNISTUB 0x11000000u
static void jni_hook(uc_engine *u, uint64_t addr, uint32_t size, void *ud){ (void)u;(void)size;(void)ud;
    static uint32_t hc=0x30000000u;
    uint32_t off=(uint32_t)addr-JNISTUB, r1=Rg(1), v=0;
    if(off<0x4000u){ uint32_t idx=off/4;
        if(idx==4) v=0x10006;                                   /* GetVersion */
        else if(idx==219){ uc_mem_write(uc,r1,&g_VM,4); v=0; }   /* GetJavaVM */
        else if(idx==6||idx==31||idx==33||idx==113||idx==94||idx==144||idx==21||idx==25||idx==167){ hc+=8; v=hc; } /* Find/Method/Field/refs/NewStringUTF */
        else v=0;
    } else { uint32_t idx=(off-0x4000u)/4; if(idx==4||idx==6||idx==7) uc_mem_write(uc,r1,&g_ENV,4); v=0; } /* GetEnv/AttachCurrentThread */
    S0(v);
}
static void jni_setup(void){
    uc_mem_map(uc, JNISTUB, 0x10000, UC_PROT_ALL);
    uint32_t bx=0xe12fff1e; for(uint32_t o=0;o<0x10000;o+=4) uc_mem_write(uc,JNISTUB+o,&bx,4);
    uint32_t envtab=balloc(300*4); for(uint32_t i=0;i<300;i++){ uint32_t s=JNISTUB+i*4; uc_mem_write(uc,envtab+i*4,&s,4);} g_ENV=balloc(4); uc_mem_write(uc,g_ENV,&envtab,4);
    uint32_t vmtab=balloc(16*4);  for(uint32_t i=0;i<16;i++){ uint32_t s=JNISTUB+0x4000+i*4; uc_mem_write(uc,vmtab+i*4,&s,4);} g_VM=balloc(4); uc_mem_write(uc,g_VM,&vmtab,4);
    uc_hook h; uc_hook_add(uc,&h,UC_HOOK_CODE,(void*)jni_hook,NULL, JNISTUB, JNISTUB+0x10000);
}

// entry to run one emulated function (LSB of addr selects ARM/Thumb — unicorn 2.1.x)
static uint32_t emu_call(uint32_t addr, uint32_t a0,uint32_t a1,uint32_t a2,uint32_t a3){
    uint32_t sp=STACK+STACK_SZ-0x1000, lr=RET;
    uc_reg_write(uc, UC_ARM_REG_SP,&sp);
    uc_reg_write(uc, UC_ARM_REG_R0,&a0); uc_reg_write(uc, UC_ARM_REG_R1,&a1);
    uc_reg_write(uc, UC_ARM_REG_R2,&a2); uc_reg_write(uc, UC_ARM_REG_R3,&a3);
    uc_reg_write(uc, UC_ARM_REG_LR,&lr);
    uc_err e=uc_emu_start(uc, addr, RET, 0, 0);
    if (e){ uint32_t pc; uc_reg_read(uc,UC_ARM_REG_PC,&pc); LOGE("emu err %d pc=%08x", e, pc); }
    uint32_t r0; uc_reg_read(uc, UC_ARM_REG_R0,&r0); return r0;
}

// one-time init of the emulator (called from JNI_OnLoad)
static int shim_init(void){
    if (uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc)) { LOGE("uc_open failed"); return -1; }
    if (load_engine()) return -1;
    uc_hook h1,h2,h3;
    uc_hook_add(uc,&h1,UC_HOOK_CODE,(void*)stub_hook,NULL, STUB, STUB+STUB_SZ);
    uc_hook_add(uc,&h2,UC_HOOK_CODE,(void*)kuser_hook,NULL, KUSER, KUSER+0x1000);
    uc_hook_add(uc,&h3,UC_HOOK_INTR,(void*)intr_hook,NULL, 1, 0);
    return 0;
}

// TODO Stage 5/6: load g_elf from the bundled asset, run init_array, and re-export the
// 72 Java_com_rovio_* thunks that marshal args and call emu_call() into the engine, with
// the REAL JNIEnv passed through (not the PoC fakes).
#ifndef SHIM_TEST
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
JavaVM *g_realvm = 0;
static void run_ctors(void){
    Elf32_Ehdr*eh=(Elf32_Ehdr*)g_elf; Elf32_Phdr*ph=(Elf32_Phdr*)(g_elf+eh->e_phoff);
    Elf32_Dyn*dyn=0; for(int i=0;i<eh->e_phnum;i++) if(ph[i].p_type==PT_DYNAMIC) dyn=(Elf32_Dyn*)(g_elf+ph[i].p_offset);
    uint32_t ia=0,iasz=0; for(Elf32_Dyn*d=dyn;d->d_tag!=DT_NULL;d++){ if(d->d_tag==DT_INIT_ARRAY)ia=d->d_un.d_val; else if(d->d_tag==DT_INIT_ARRAYSZ)iasz=d->d_un.d_val; }
    int n=iasz/4, ok=0;
    for(int i=0;i<n;i++){ uint32_t fn=rd32(BASE+ia+i*4); if(fn==0||fn==0xffffffffu)continue; emu_call(fn,0,0,0,0); ok++; }
    LOGI("init_array: %d constructors run under emulation", ok);
}
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved){
    (void)reserved; g_realvm = vm;
    LOGI("abshim JNI_OnLoad");
    Dl_info di; char path[1024];
    if(!dladdr((void*)&JNI_OnLoad,&di)){ LOGE("dladdr failed"); return JNI_VERSION_1_6; }
    strncpy(path, di.dli_fname, sizeof path-1); path[sizeof path-1]=0;
    char *sl=strrchr(path,'/'); if(!sl){ LOGE("bad shim path %s",path); return JNI_VERSION_1_6; }
    strcpy(sl+1,"libengine32.so");
    int fd=open(path,O_RDONLY); if(fd<0){ LOGE("open %s failed",path); return JNI_VERSION_1_6; }
    struct stat st; fstat(fd,&st); g_elf_len=st.st_size;
    g_elf=mmap(0,g_elf_len,PROT_READ,MAP_PRIVATE,fd,0); close(fd);
    if(g_elf==MAP_FAILED){ LOGE("mmap engine failed"); return JNI_VERSION_1_6; }
    LOGI("engine loaded: %zu bytes from %s", g_elf_len, path);
    if(shim_init()){ LOGE("shim_init failed"); return JNI_VERSION_1_6; }
    run_ctors();
    LOGI("engine initialized under emulation; awaiting JNI thunks");
    /* TODO Stage 4/5: real-JNIEnv passthrough + the 72 Java_* entry thunks */
    return JNI_VERSION_1_6;
}
#endif

#ifdef SHIM_TEST
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
int main(int argc, char **argv){
    const char *path = argc>1 ? argv[1] : "engine.so";
    int fd = open(path, O_RDONLY);
    if (fd<0){ printf("cannot open %s\n", path); return 1; }
    struct stat st; fstat(fd,&st); g_elf_len = st.st_size;
    g_elf = mmap(0, g_elf_len, PROT_READ, MAP_PRIVATE, fd, 0);
    printf("loaded %s (%zu bytes)\n", path, g_elf_len);
    if (shim_init()){ printf("shim_init FAILED\n"); return 1; }
    printf("shim_init OK: %d imports mapped\n", g_import_count);
    Elf32_Ehdr *eh=(Elf32_Ehdr*)g_elf; Elf32_Phdr *ph=(Elf32_Phdr*)(g_elf+eh->e_phoff);
    Elf32_Dyn *dyn=NULL; for(int i=0;i<eh->e_phnum;i++) if(ph[i].p_type==PT_DYNAMIC) dyn=(Elf32_Dyn*)(g_elf+ph[i].p_offset);
    uint32_t ia=0,iasz=0;
    for(Elf32_Dyn*d=dyn; d->d_tag!=DT_NULL; d++){ if(d->d_tag==DT_INIT_ARRAY)ia=d->d_un.d_val; else if(d->d_tag==DT_INIT_ARRAYSZ)iasz=d->d_un.d_val; }
    int n=iasz/4, ok=0;
    for(int i=0;i<n;i++){ uint32_t fn=rd32(BASE+ia+i*4); if(fn==0||fn==0xffffffffu)continue;
        uint32_t sp=STACK+STACK_SZ-0x1000, lr=RET, z=0;
        uc_reg_write(uc,UC_ARM_REG_SP,&sp); uc_reg_write(uc,UC_ARM_REG_LR,&lr);
        uc_reg_write(uc,UC_ARM_REG_R0,&z); uc_reg_write(uc,UC_ARM_REG_R1,&z);
        uc_err e=uc_emu_start(uc, fn, RET, 0, 8000000);
        if(e){ uint32_t pc; uc_reg_read(uc,UC_ARM_REG_PC,&pc); printf("ctor %d @ %08x FAILED: %s pc=%08x\n", i, fn, uc_strerror(e), pc); break; }
        ok++;
    }
    printf("init_array: %d/%d constructors ran CLEAN\n", ok, n);
    jni_setup();
    printf("JNI_OnLoad -> 0x%x\n", emu_call(BASE+0x1da914u, g_VM, 0, 0, 0));
    printf("calling nativeInit @ %08x ...\n", BASE+0x1de5ecu);
    uint32_t rr = emu_call(BASE+0x1de5ecu, g_ENV, 0x30001000u, 0, 0);
    printf("nativeInit -> %u\n", rr);
    return 0;
}
#endif
