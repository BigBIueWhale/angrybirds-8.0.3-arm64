/* test_boot_assets.c — drive the engine boot with a HOST AAsset backend that serves the
 * REAL extracted APK assets (dir in ABSHIM_ASSET_DIR). bridge_asset.c resolves the AAsset
 * API via dlsym(RTLD_DEFAULT,...), so the global functions below are what it calls. This
 * lets the engine's OWN asset/config/script loading + decryption run under emulation — far
 * more core coverage (unicorn/marshalling/libc/memory) than the fake-env-only probe, which
 * stalls the moment the engine needs its assets. Fake JVM for the rest. Progression probe:
 * we report how far nativeInit gets + which assets were opened.
 *
 * Build (needs -rdynamic so dlsym(RTLD_DEFAULT) finds these symbols):
 *   see run_tests.sh
 */
#include "dispatch.h"
#include "jni_passthrough.h"
#include "sched.h"
#include "elf32.h"
#include "regions.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------- host AAsset backend (global symbols; resolved by bridge_asset's dlsym) ---------- */
typedef struct { unsigned char *buf; long len; } HAsset;
static int g_open_ok=0, g_open_miss=0; static char g_last_open[256];

/* index of every asset path (relative to ASSET_DIR), built once, for suffix resolution:
 * the engine (under the fake env) requests paths without the real base prefix
 * (e.g. "gles2/x.fx" for "data/shaders/gles2/x.fx"), so match by path suffix. */
static char **g_idx; static int g_nidx;
static void idx_build(const char *dir){
    /* one popen(find) — simplest robust walk for a test harness */
    char cmd[2200]; snprintf(cmd,sizeof cmd,"cd '%s' && find . -type f 2>/dev/null | sed 's#^\\./##'",dir);
    FILE *p=popen(cmd,"r"); if(!p) return; char line[2048];
    int cap=8192; g_idx=malloc(cap*sizeof(char*));
    while(fgets(line,sizeof line,p)){ size_t L=strlen(line); if(L&&line[L-1]=='\n')line[--L]=0; if(!L)continue;
        if(g_nidx>=cap){ cap*=2; g_idx=realloc(g_idx,cap*sizeof(char*)); } g_idx[g_nidx++]=strdup(line); }
    pclose(p);
}
static const char *idx_resolve(const char *req){
    /* exact, else the shortest full path that ends with "/req" or == req */
    for(int i=0;i<g_nidx;i++) if(!strcmp(g_idx[i],req)) return g_idx[i];
    const char *best=NULL; size_t bl=~0u; size_t rl=strlen(req);
    for(int i=0;i<g_nidx;i++){ size_t L=strlen(g_idx[i]);
        if(L>=rl && !strcmp(g_idx[i]+L-rl,req) && (L==rl || g_idx[i][L-rl-1]=='/'))
            if(L<bl){ bl=L; best=g_idx[i]; } }
    return best;
}
void *AAssetManager_fromJava(void *env, void *obj){ (void)env;(void)obj; return (void*)0x1; } /* non-NULL mgr */
void *AAssetManager_open(void *mgr, const char *path, int mode){
    (void)mgr;(void)mode;
    const char *dir=getenv("ABSHIM_ASSET_DIR"); if(!dir||!path) return NULL;
    snprintf(g_last_open,sizeof g_last_open,"%s",path);
    if(!g_idx) idx_build(dir);
    const char *rel=idx_resolve(path); if(!rel){ g_open_miss++; if(getenv("ABSHIM_ASSET_TRACE")) fprintf(stderr,"[asset] MISS %s\n",path); return NULL; }
    char full[2048]; snprintf(full,sizeof full,"%s/%s",dir,rel);
    FILE *f=fopen(full,"rb"); if(!f){ g_open_miss++; return NULL; }
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    HAsset *a=(HAsset*)malloc(sizeof *a); a->buf=(unsigned char*)malloc(n>0?n:1); a->len=n;
    if(n>0 && fread(a->buf,1,(size_t)n,f)!=(size_t)n){ /* short read: keep what we got */ }
    fclose(f); g_open_ok++;
    if(getenv("ABSHIM_ASSET_TRACE")) fprintf(stderr,"[asset] open %s (%ld B)\n",path,n);
    return a;
}
const void *AAsset_getBuffer(void *asset){ return asset?((HAsset*)asset)->buf:NULL; }
long AAsset_getLength64(void *asset){ return asset?((HAsset*)asset)->len:0; }
long AAsset_getLength  (void *asset){ return asset?((HAsset*)asset)->len:0; }
void AAsset_close(void *asset){ if(asset){ free(((HAsset*)asset)->buf); free(asset); } }

/* ------------------------------------------------------------------ harness ------------- */
static uint8_t *load_file(const char *p, long *n){ FILE *f=fopen(p,"rb"); if(!f) return NULL;
    fseek(f,0,SEEK_END); *n=ftell(f); fseek(f,0,SEEK_SET); uint8_t *b=malloc(*n);
    if(fread(b,1,*n,f)!=(size_t)*n){ fclose(f); free(b); return NULL; } fclose(f); return b; }
static uint32_t sym_addr(loader_t *L,const char *name){ uint32_t si; if(elf32_find_symbol(&L->img,name,&si)) return 0;
    elf32_symres r=elf32_classify(&L->img,si); return (RG_ENGINE+r.value)|(r.thumb?1u:0u); }

int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    printf("=== boot-with-assets drive (host AAsset backend) ===\n");
    if(!getenv("ABSHIM_ASSET_DIR")){ printf("  SKIP: set ABSHIM_ASSET_DIR to the extracted assets/ dir\n"); return 0; }
    const char *path=getenv("ABSHIM_ENGINE_SO");
    if(!path) path="work803/libv7/libAngryBirdsClassic.so";
    long len; uint8_t *elf=load_file(path,&len); if(!elf){ printf("  SKIP: no engine\n"); return 0; }

    cpu_t cpu; if(cpu_create(&cpu)){ printf("cpu fail\n"); return 1; }
    loader_t L; if(loader_load(&L,&cpu,elf,len)){ printf("load fail\n"); return 1; }
    dispatch_t D; dispatch_install(&D,&cpu,&L);
    /* install the REAL GEL scheduler (jni_entry.c:394-395) so guest pthread_create/cond/mutex
     * behave as on device: green threads actually run and worker-thread task queues (rcs::Eraser)
     * keep their invariants. Without it, pthread_create is a no-op and such subsystems throw. */
    sched sch; sched_init(&sch,&cpu); D.sch=&sch; sch.fatal_ext=&D.fatal;
    gthread *gt=sched_host_gthread(&sch);
    jni_state J; jni_install(&J,&cpu,1); D.jni=&J;

    int total=0, ran=dispatch_run_init_array(&D,&total);
    printf("  ctors %d/%d clean\n", ran, total);
    /* guest JNI_OnLoad: direct cpu_call, exactly like the real shim (jni_entry.c:405) */
    uint32_t onload=sym_addr(&L,"JNI_OnLoad");
    if(onload){ uc_err e=cpu_call(&cpu,onload,J.vm,0,0,0,100000000); uint32_t r0; uc_reg_read(cpu.uc,UC_ARM_REG_R0,&r0);
        printf("  JNI_OnLoad -> 0x%x (%s)\n", r0, uc_strerror(e)); }

    uint32_t ninit=sym_addr(&L,"Java_com_rovio_fusion_NativeApplication_nativeInit");
    uint32_t thiz=ht_new_ref(J.ht,HK_LOCAL,(void*)0x1234);
    D.fatal=0;
    /* nativeInit runs THROUGH the scheduler (shim_call path, jni_entry.c:354) */
    uc_err e=sched_call(&sch,gt,ninit,J.env,thiz,1024,768);
    uint32_t r0=0,pc=0; uc_reg_read(cpu.uc,UC_ARM_REG_R0,&r0); uc_reg_read(cpu.uc,UC_ARM_REG_PC,&pc);
    printf("  nativeInit -> r0=0x%x emu=%s pc=0x%x fatal=%d\n", r0, uc_strerror(e), pc, D.fatal);
    if(D.fatal){ const char*sn=loader_stub_name(&L,pc&~1u); uint32_t lr=0; uc_reg_read(cpu.uc,UC_ARM_REG_LR,&lr);
        printf("  FATAL stub=%s  LR=0x%x (engine +0x%x)\n", sn?sn:"(guest)", lr, lr>=RG_ENGINE?lr-RG_ENGINE:0); }
    printf("  assets opened OK=%d miss=%d  last='%s'\n", g_open_ok, g_open_miss, g_last_open);
    int used=0; for(int i=0;i<260;i++) if(J.slots_used[i]) used++;
    printf("  JNI slots used=%d  dispatch unimpl=%d (last='%s')\n", used, D.unimpl_count, D.last_unimpl);
    printf("  heap %s; in-use=%u\n", galloc_check(cpu.heap)==0?"valid":"CORRUPT", galloc_inuse_bytes(cpu.heap));

    jni_free(&J); loader_free(&L); cpu_destroy(&cpu); free(elf);
    printf("\n=== boot-with-assets drive complete ===\n");
    return 0;
}
