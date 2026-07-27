/* test_boot_render.c — the furthest host validation: give the shim a REAL (software, mesa)
 * GLES2 context + the REAL extracted assets, and drive nativeInit -> nativeResize ->
 * nativeUpdate -> nativeRender. Now glCompileShader/glLinkProgram/glDraw* actually run
 * (mesa llvmpipe), so the engine can get past the shader wall and render into a pbuffer we
 * read back. Proves the GL bridge end-to-end on real GL, not just compile-clean.
 *
 * Run inside ab-port with mesa dev installed; see the docker invocation in the session.
 *   ABSHIM_ASSET_DIR=/work/assets  ABSHIM_ENGINE_SO=...  ./tbr
 */
#include "dispatch.h"
#include "jni_passthrough.h"
#include "sched.h"
#include "elf32.h"
#include "regions.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

/* GL activity counters (defined in bridge_gl.c) — did the engine actually issue draws? */
extern unsigned long g_gl_draws, g_gl_clears, g_gl_useprog;

/* ---------------- host AAsset backend (suffix-resolved real assets) ---------------- */
typedef struct { unsigned char *buf; long len; } HAsset;
static int g_open_ok=0, g_open_miss=0; static char **g_idx; static int g_nidx;
static void idx_build(const char *dir){ char cmd[2200];
    snprintf(cmd,sizeof cmd,"cd '%s' && find . -type f 2>/dev/null | sed 's#^\\./##'",dir);
    FILE *p=popen(cmd,"r"); if(!p) return; char line[2048]; int cap=8192; g_idx=malloc(cap*sizeof(char*));
    while(fgets(line,sizeof line,p)){ size_t L=strlen(line); if(L&&line[L-1]=='\n')line[--L]=0; if(!L)continue;
        if(g_nidx>=cap){ cap*=2; g_idx=realloc(g_idx,cap*sizeof(char*)); } g_idx[g_nidx++]=strdup(line);} pclose(p); }
static const char *idx_resolve(const char *req){ for(int i=0;i<g_nidx;i++) if(!strcmp(g_idx[i],req)) return g_idx[i];
    const char *best=NULL; size_t bl=~0u, rl=strlen(req);
    for(int i=0;i<g_nidx;i++){ size_t L=strlen(g_idx[i]); if(L>=rl && !strcmp(g_idx[i]+L-rl,req) && (L==rl||g_idx[i][L-rl-1]=='/')) if(L<bl){bl=L;best=g_idx[i];} }
    return best; }
void *AAssetManager_fromJava(void *e,void *o){ (void)e;(void)o; return (void*)0x1; }
void *AAssetManager_open(void *m,const char *path,int mode){ (void)m;(void)mode;
    const char *dir=getenv("ABSHIM_ASSET_DIR"); if(!dir||!path) return NULL; if(!g_idx) idx_build(dir);
    const char *rel=idx_resolve(path); if(!rel){ g_open_miss++; fprintf(stderr,"[asset MISS] %s\n",path); return NULL; }
    char full[2048]; snprintf(full,sizeof full,"%s/%s",dir,rel); FILE *f=fopen(full,"rb"); if(!f){ g_open_miss++; return NULL; }
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET); HAsset *a=malloc(sizeof*a); a->buf=malloc(n>0?n:1); a->len=n;
    if(n>0&&fread(a->buf,1,(size_t)n,f)!=(size_t)n){} fclose(f); g_open_ok++;
    if(getenv("ABSHIM_ASSET_TRACE")) fprintf(stderr,"[asset] %s (%ld B)\n",path,n);
    return a; }
const void *AAsset_getBuffer(void *a){ return a?((HAsset*)a)->buf:NULL; }
long AAsset_getLength64(void *a){ return a?((HAsset*)a)->len:0; }
long AAsset_getLength (void *a){ return a?((HAsset*)a)->len:0; }
void AAsset_close(void *a){ if(a){ free(((HAsset*)a)->buf); free(a);} }

/* ---------------- harness ---------------- */
static uint8_t *load_file(const char *p,long *n){ FILE *f=fopen(p,"rb"); if(!f) return NULL;
    fseek(f,0,SEEK_END); *n=ftell(f); fseek(f,0,SEEK_SET); uint8_t *b=malloc(*n); if(fread(b,1,*n,f)!=(size_t)*n){fclose(f);free(b);return NULL;} fclose(f); return b; }
static uint32_t sym_addr(loader_t *L,const char *n){ uint32_t si; if(elf32_find_symbol(&L->img,n,&si)) return 0;
    elf32_symres r=elf32_classify(&L->img,si); return (RG_ENGINE+r.value)|(r.thumb?1u:0u); }
/* Faithful entry: every guest call runs THROUGH the GEL scheduler exactly like the real
 * shim's shim_call (jni_entry.c: sched_enter). Without this, pthread_create is a no-op and
 * any engine subsystem with a worker-thread task queue (e.g. rcs::Eraser) breaks its own
 * invariant and throws — a harness artifact, not a shim bug. g_sch/g_gt are set in main. */
static sched   *g_sch;      /* the GEL scheduler (installed in main) */
static gthread *g_gt;       /* this host thread's entry gthread */
static uint32_t call(cpu_t*c,uint32_t a,uint32_t a0,uint32_t a1,uint32_t a2,uint32_t a3,int*fatal,dispatch_t*D){
    D->fatal=0; uc_err e=sched_call(g_sch,g_gt,a,a0,a1,a2,a3); (void)e;
    uint32_t r0=0; uc_reg_read(c->uc,UC_ARM_REG_R0,&r0); *fatal=D->fatal; return r0; }

static void throw_hook(uc_engine*uc, uint64_t a, uint32_t s, void*u){
    (void)a;(void)s;(void)u; uint32_t r1=0,lr=0,sp=0; uc_reg_read(uc,UC_ARM_REG_R1,&r1); uc_reg_read(uc,UC_ARM_REG_LR,&lr); uc_reg_read(uc,UC_ARM_REG_SP,&sp);
    char tn[80]=""; if(r1){ uint32_t np=0; uc_mem_read(uc,r1+4,&np,4); for(int i=0;i<79;i++){uint8_t ch;uc_mem_read(uc,np+i,&ch,1);if(!ch)break;tn[i]=(char)ch;} }
    fprintf(stderr,"[throw] %s from engine+0x%x\n", tn, lr>=RG_ENGINE?lr-RG_ENGINE:lr);
    if(strstr(tn,"system_error")){ fprintf(stderr,"  [bt]"); int n=0;
        for(uint32_t off=0; off<1024 && n<16; off+=4){ uint32_t w=0; uc_mem_read(uc,sp+off,&w,4); uint32_t e=w&~1u;
            if(e>=RG_ENGINE+0x1000u && e<RG_ENGINE+0xb00000u){ fprintf(stderr," +0x%x",e-RG_ENGINE); n++; } }
        fprintf(stderr,"\n"); }
}
/* dump the JSON parser's live registers + pointed memory at the ParseError throw site (0x68e9a0),
 * to find the config.json parse buffer: 'eacf76..' => still ENCRYPTED (decrypt never ran);
 * '{...' => decrypted OK (issue elsewhere); other garbage => AES ran with wrong key/broken NEON. */
/* hook the VFS scheme-map insert (0x6bd3e0): r1=&scheme-name std::string ([r1]=data,[r1+8]=len).
 * Lists every VFS scheme the engine registers — is 'wgt' among them (link/lookup bug) or missing
 * (registration gap)? */
static void scheme_hook(uc_engine*uc,uint64_t a,uint32_t s,void*u){ (void)a;(void)s;(void)u;
    static int n=0; if(n++>=24) return;
    uint32_t r1=0; uc_reg_read(uc,UC_ARM_REG_R1,&r1);
    uint32_t data=0,len=0; uc_mem_read(uc,r1,&data,4); uc_mem_read(uc,r1+8,&len,4);
    char nm[48]; unsigned L=len<47?len:47; for(unsigned k=0;k<L;k++){ uint8_t c=0; uc_mem_read(uc,data+k,&c,1); nm[k]=(c>=32&&c<127)?(char)c:'.'; } nm[L]=0;
    fprintf(stderr,"[scheme#%d] VFS add scheme='%s' (len=%u data=0x%x)\n",n,nm,len,data); }
/* hook util::JSON::parse ENTRY (0x69814c): r1=&{begin,end} ([r1]=begin, [r1+4]=end), LR=caller.
 * Names the content-loader that hands it an EMPTY range (len=0), and shows what the non-empty
 * parses (config.json) are + who loads them. */
static void jsonparse_hook(uc_engine*uc,uint64_t a,uint32_t s,void*u){ (void)a;(void)s;(void)u;
    static int n=0; if(n++>=24) return;
    uint32_t r1=0,lr=0; uc_reg_read(uc,UC_ARM_REG_R1,&r1); uc_reg_read(uc,UC_ARM_REG_LR,&lr);
    uint32_t begin=0,end=0; uc_mem_read(uc,r1,&begin,4); uc_mem_read(uc,r1+4,&end,4);
    uint32_t len=(end>=begin)?end-begin:0;
    fprintf(stderr,"[jsonparse#%d] len=%u caller=+0x%x",n,len,lr>=RG_ENGINE?lr-RG_ENGINE:lr);
    if(len>0){ uint8_t b[64]; uint32_t hn=len>64u?64u:len; if(uc_mem_read(uc,begin,b,hn)==UC_ERR_OK){ fprintf(stderr," head:'");
        for(uint32_t k=0;k<hn;k++) fputc((b[k]>=32&&b[k]<127)?b[k]:'.',stderr); fputc('\'',stderr); } }
    fputc('\n',stderr); }
/* hook the JSON sub-parser ENTRY (0x68e814): r0=parse ctx, [r0]=buf start, [r0+8]=buf end.
 * Dump each parse's input buffer+length so we see config.json's DECRYPTED content and exactly
 * where it truncates (short len => the decrypt-output length is wrong; that's the real bug). */
static void parsebuf_hook(uc_engine*uc,uint64_t a,uint32_t s,void*u){ (void)a;(void)s;(void)u;
    static int n=0; if(n++>=10) return;
    uint32_t r0=0; uc_reg_read(uc,UC_ARM_REG_R0,&r0);
    uint32_t start=0,end=0,lr=0; uc_mem_read(uc,r0,&start,4); uc_mem_read(uc,r0+8,&end,4); uc_reg_read(uc,UC_ARM_REG_LR,&lr);
    uint32_t len=(end>=start)?end-start:0;
    fprintf(stderr,"[parsebuf#%d] ctx=0x%x buf=[0x%x..0x%x] len=%u caller_lr=+0x%x\n",n,r0,start,end,len,lr>=RG_ENGINE?lr-RG_ENGINE:lr);
    uint32_t hn=len>160u?160u:len; uint8_t b[160];
    if(hn && uc_mem_read(uc,start,b,hn)==UC_ERR_OK){ fprintf(stderr,"  head:'");
        for(uint32_t k=0;k<hn;k++) fputc((b[k]>=32&&b[k]<127)?b[k]:'.',stderr); fprintf(stderr,"'\n"); }
    if(len>32u){ uint8_t t[32]; if(uc_mem_read(uc,end-32u,t,32)==UC_ERR_OK){ fprintf(stderr,"  tail:'");
        for(int k=0;k<32;k++) fputc((t[k]>=32&&t[k]<127)?t[k]:'.',stderr); fprintf(stderr,"'\n"); } }
    if(len==0){ uint32_t sp=0; uc_reg_read(uc,UC_ARM_REG_SP,&sp);  /* empty content: walk stack for the content-loader chain */
        fprintf(stderr,"  return-chain:"); int c=0;
        for(uint32_t o=0;o<640 && c<12;o+=4){ uint32_t w=0; if(uc_mem_read(uc,sp+o,&w,4))break; uint32_t e=w&~1u;
            if(e>=RG_ENGINE+0x1000u && e<RG_ENGINE+0xb00000u){ fprintf(stderr," +0x%x",e-RG_ENGINE); c++; } }
        fprintf(stderr,"\n"); } }
/* watchpoint on the game-object globals ([g+0x7c]=0xabb124, [g+0x80]=0xabb128 — computed from
 * nativeResize/nativeUpdate disasm). nativeUpdate/nativeResize gate on these; if nativeInit
 * never writes a non-zero value here, the game object is never created -> draws=0. */
static void gobj_wp(uc_engine*uc, uc_mem_type ty, uint64_t addr, int sz, int64_t val, void*u){
    (void)ty;(void)sz;(void)u; uint32_t pc=0; uc_reg_read(uc,UC_ARM_REG_PC,&pc);
    fprintf(stderr,"[gobj] WRITE [engine+0x%llx] = 0x%llx  from engine+0x%x\n",
        (unsigned long long)(addr-RG_ENGINE), (unsigned long long)(val & 0xffffffffu), pc>=RG_ENGINE?pc-RG_ENGINE:pc);
}
/* trace F's path through the [g+0x80]-setting region: 0x1de2f8=loop-exit, 0x1de310=bl new-obj,
 * 0x1de318=str [g+0x80], 0x1de4f0=skip path ([g+0x7c] seen ==0). Which fire tells us where F diverges. */
static void pc_hook(uc_engine*uc, uint64_t a, uint32_t s, void*u){ (void)uc;(void)s;(void)u;
    static int n=0; if(n++<400){ uint32_t lr=0; uc_reg_read(uc,UC_ARM_REG_LR,&lr);
        fprintf(stderr,"[pc] engine+0x%x (lr=+0x%x)\n",(uint32_t)(a-RG_ENGINE), lr>=RG_ENGINE?lr-RG_ENGINE:lr); } }
/* DIAG (cont.64): does the host harness reproduce the emulator's scene-ctor divergence?
 * g_visit counts the per-node tree-visitor 0x7d2b28 (millions => the corrupted-tree walk). */
static unsigned long g_visit=0;
static const char* g_phase="startup";   /* which native is currently executing (set by DRV/frame loop) */
static unsigned g_ctor_enter=0, g_ctor_ret=0;   /* count 0x6f370 entries vs 0x726a4 returns, per phase */
static void visit_cnt(uc_engine*uc,uint64_t a,uint32_t s,void*u){ (void)a;(void)s;(void)u; g_visit++;
    if(g_visit<=5 || (g_visit>=1178&&g_visit<=1190)){ uint32_t r1=0; uc_reg_read(uc,UC_ARM_REG_R1,&r1);
        char s[40]={0}; for(int k=0;k<39;k++){ uint8_t c=0; if(uc_mem_read(uc,r1+0x10u+(unsigned)k,&c,1)||!c)break; s[k]=(c>=32&&c<127)?(char)c:'.'; }
        fprintf(stderr,"node%lu r1=0x%x str='%s'\n",g_visit,r1,s); } }
/* forward-distinct-block trace of 0x653384's subtree (the CORRECT path, to diff vs the emulator). */
static unsigned char g_hseen[0xB00000u/16u];
static uint32_t g_hfwd[2000]; static unsigned g_hfwdn=0; static int g_hon=0;
static void hfwd_dump(void){ fprintf(stderr,"[hfwd] %u distinct blocks from 0x653384 to nativeInit-return:\n",g_hfwdn);
    for(unsigned k=0;k<g_hfwdn;k++) fprintf(stderr,"%x%c",g_hfwd[k],(k%16==15)?'\n':' '); fprintf(stderr,"\n"); }
static uint32_t g_ring2[256]; static uint32_t g_ring2sp[256]; static unsigned g_ring2pos=0;   /* last 256 blocks (non-distinct) = the deepest sequence + unwind */
static void ctor_bb(uc_engine*uc,uint64_t a,uint32_t s,void*u){ (void)uc;(void)s;(void)u;
    if(!g_hon) return; uint32_t off=(uint32_t)(a-RG_ENGINE);
    uint32_t sp=0; uc_reg_read(uc,UC_ARM_REG_SP,&sp); g_ring2sp[g_ring2pos & 255u]=sp;
    g_ring2[g_ring2pos++ & 255u]=off;
    if(off<0xB00000u){ unsigned bit=off>>1,idx=bit>>3,msk=1u<<(bit&7u);
        if(!(g_hseen[idx]&msk)){ g_hseen[idx]|=(unsigned char)msk; if(g_hfwdn<2000) g_hfwd[g_hfwdn++]=off; } } }
static void ring2_dump(void){ unsigned n=g_ring2pos<256u?g_ring2pos:256u, st=g_ring2pos<256u?0u:(g_ring2pos&255u);
    fprintf(stderr,"[ring2] last %u blocks before nativeInit returned (chronological):\n",n);
    for(unsigned i=0;i<n;i++) fprintf(stderr,"%x%c",g_ring2[(st+i)&255u],(i%16==15)?'\n':' '); fprintf(stderr,"\n"); }
/* dump the last N blocks WITH the SP at each — reveals HOW frame-1 nativeUpdate's ctor 0x6f370
 * exits to RG_RET: a normal deepening (SP monotonically low) vs a stack reset (SP jumps to top). */
static void ring2_dump_sp(const char*tag){ unsigned n=g_ring2pos<48u?g_ring2pos:48u, st=(g_ring2pos-n)&255u;
    fprintf(stderr,"[ring2sp %s] last %u blocks (off @ sp):\n",tag,n);
    for(unsigned i=0;i<n;i++){ unsigned k=(st+i)&255u; fprintf(stderr,"  0x%x @ sp=0x%x\n",g_ring2[k],g_ring2sp[k]); } }
/* watch 0x118f30's saved-LR slot (=0x6f3a8): if a store overwrites it with != 0x6f3a8, that store's
 * PC is the corruptor (a buffer overflow into the stack) — the ctor-non-return root cause. */
static uint32_t g_lrslot=0;
/* track the LAST basic block executed within 0x118f30's own body (0x118f30..0x11b9d0) = where the
 * 10KB VFS-setup fn exits/stalls (its epilogue => returned; a mid-fn call site => diverged there). */
static uint32_t g_last118=0; static unsigned long g_118n=0;
static void bb118(uc_engine*uc,uint64_t a,uint32_t s,void*u){ (void)uc;(void)s;(void)u; g_last118=(uint32_t)(a-RG_ENGINE); g_118n++; }
static void lr_watch(uc_engine*uc, uc_mem_type ty, uint64_t addr, int sz, int64_t val, void*u){
    (void)ty;(void)sz;(void)u; if(!g_lrslot || (uint32_t)addr!=g_lrslot) return;
    uint32_t pc=0; uc_reg_read(uc,UC_ARM_REG_PC,&pc);
    fprintf(stderr,"[LR-write] slot=0x%x <= 0x%x  from pc=engine+0x%x %s\n", g_lrslot,(uint32_t)val,
        pc>=RG_ENGINE?pc-RG_ENGINE:pc, ((uint32_t)val==0x6f3a8)?"(the push, ok)":"<<< CORRUPTOR"); }
/* catch the stack-corruption: a write of a bad 0x40aebXXX (engine BSS near the pthread_once
 * controls) value onto the worker stack — the block-trail shows pop{pc} loads 0x40aebdcc and
 * executes BSS as code. Logs the writing PC + target so we find the overflowing store. */
static void badret_watch(uc_engine*uc, uc_mem_type ty, uint64_t addr, int sz, int64_t val, void*u){
    (void)ty;(void)sz;(void)u; uint32_t v=(uint32_t)val;
    if(v>=RG_ENGINE+0xaeb000u && v<RG_ENGINE+0xaec000u){
        uint32_t pc=0,lr=0; uc_reg_read(uc,UC_ARM_REG_PC,&pc); uc_reg_read(uc,UC_ARM_REG_LR,&lr);
        fprintf(stderr,"[badret] write 0x%x -> [0x%llx] pc=engine+0x%x lr=+0x%x\n", v,
            (unsigned long long)addr, pc>=RG_ENGINE?pc-RG_ENGINE:pc, lr>=RG_ENGINE?lr-RG_ENGINE:lr); } }
/* catch the WILD JUMP into RG_GUESTDATA (0x12000000): PC executing there = a bad function-pointer
 * call; LR = the engine instruction that did it (the bad blx site). */
static void gd_hook(uc_engine*uc,uint64_t a,uint32_t s,void*u){ (void)s;(void)u; static int n=0; if(n++>=8) return;
    uint32_t lr=0,r0=0,r1=0; uc_reg_read(uc,UC_ARM_REG_LR,&lr); uc_reg_read(uc,UC_ARM_REG_R0,&r0); uc_reg_read(uc,UC_ARM_REG_R1,&r1);
    fprintf(stderr,"[GUESTDATA-exec] PC=0x%x  LR(bad-blx-site)=engine+0x%x  r0=0x%x r1=0x%x\n",
        (uint32_t)a, lr>=RG_ENGINE?lr-RG_ENGINE:lr, r0, r1); }
/* hook __gnu_ldivmod_helper@0x89c8cc entry: log SP+LR+args+the caller return-chain to name the
 * re-entry path that reset SP to the stack top. Only log when SP is up near the top (the collision). */
static void divmod_hook(uc_engine*uc,uint64_t a,uint32_t s,void*u){ (void)a;(void)s;(void)u; static int n=0;
    uint32_t sp=0,lr=0; uc_reg_read(uc,UC_ARM_REG_SP,&sp); uc_reg_read(uc,UC_ARM_REG_LR,&lr);
    if(sp < 0x7fffe000u || n>=2) return; n++;   /* only the near-top (colliding) calls */
    fprintf(stderr,"[divmod] SP=0x%x LR=+0x%x  — engine return-addrs up the stack (the caller chain):\n", sp, lr>=RG_ENGINE?lr-RG_ENGINE:lr);
    for(uint32_t k=0;k<160;k++){ uint32_t w=0; uc_mem_read(uc,sp+k*4,&w,4);
        if(w>=RG_ENGINE && w<RG_ENGINE+0xb00000u) fprintf(stderr,"  [sp+0x%x]=engine+0x%x\n",k*4,w-RG_ENGINE); } }
static void mark_hook(uc_engine*uc,uint64_t a,uint32_t s,void*u){ (void)s;(void)u;
    uint32_t off=(uint32_t)(a-RG_ENGINE); static unsigned char seen[10]={0};
    if(off==0x118f30 && !g_lrslot){ uint32_t sp=0; uc_reg_read(uc,UC_ARM_REG_SP,&sp); g_lrslot=sp-4; }
    int i = off==0x6f370?0: off==0x118f30?1: off==0x653384?2: off==0x7c4a1c?3: off==0x118fc4?4: off==0x6f3a8?5:
            off==0x726a4?6: off==0x1de318?7: off==0x1de4f0?8: 9;   /* 726a4=ctor returns; 1de318=STORE[g+0x80]; 1de4f0=throw path */
    if(i<9 && !seen[i]){ seen[i]=1; fprintf(stderr,"[mark] reached engine+0x%x (visits=%lu)\n",off,g_visit); }
    if(off==0x6f370){ uint32_t sp=0; uc_reg_read(uc,UC_ARM_REG_SP,&sp); g_ctor_enter++;
        fprintf(stderr,"[ctor-ENTER] 0x6f370 #%u phase=%s sp=0x%x\n",g_ctor_enter,g_phase,sp); }
    if(off==0x726a4){ uint32_t sp=0; uc_reg_read(uc,UC_ARM_REG_SP,&sp); g_ctor_ret++;
        fprintf(stderr,"[ctor-RETURN] 0x726a4 #%u phase=%s sp=0x%x\n",g_ctor_ret,g_phase,sp); }
    if(off==0x653384) g_hon=1; }
static int egl_up(int W,int H){
    EGLint mj,mn; EGLDisplay d=EGL_NO_DISPLAY;
    PFNEGLGETPLATFORMDISPLAYEXTPROC getPD=(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if(getPD){ d=getPD(EGL_PLATFORM_SURFACELESS_MESA,(void*)EGL_DEFAULT_DISPLAY,NULL);
               if(d!=EGL_NO_DISPLAY && eglInitialize(d,&mj,&mn)) goto got; d=EGL_NO_DISPLAY; }
    d=eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if(d==EGL_NO_DISPLAY || !eglInitialize(d,&mj,&mn)){ printf("  EGL init FAILED (err 0x%x)\n", eglGetError()); return 0; }
got:
    printf("  EGL %d.%d vendor=%s (surfaceless)\n",mj,mn,eglQueryString(d,EGL_VENDOR));
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint ca[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_DEPTH_SIZE,16,EGL_NONE};
    EGLConfig cfg; EGLint nc; if(!eglChooseConfig(d,ca,&cfg,1,&nc)||nc<1){ printf("  EGL chooseConfig FAILED\n"); return 0; }
    EGLint pa[]={EGL_WIDTH,W,EGL_HEIGHT,H,EGL_NONE}; EGLSurface s=eglCreatePbufferSurface(d,cfg,pa);
    if(s==EGL_NO_SURFACE){ printf("  EGL pbuffer FAILED\n"); return 0; }
    EGLint xa[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE}; EGLContext ctx=eglCreateContext(d,cfg,EGL_NO_CONTEXT,xa);
    if(ctx==EGL_NO_CONTEXT){ printf("  EGL context FAILED\n"); return 0; }
    if(!eglMakeCurrent(d,s,s,ctx)){ printf("  EGL makeCurrent FAILED\n"); return 0; }
    printf("  GL_RENDERER=%s  GL_VERSION=%s\n", (const char*)glGetString(GL_RENDERER),(const char*)glGetString(GL_VERSION));
    return 1;
}

int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    int W=1024,H=768;
    printf("=== boot-with-render drive (mesa GLES2 + real assets) ===\n");
    if(!getenv("ABSHIM_ASSET_DIR")){ printf("  SKIP: no ABSHIM_ASSET_DIR\n"); return 0; }
    if(!egl_up(W,H)){ printf("  SKIP: EGL/GL unavailable\n"); return 0; }
    const char *path=getenv("ABSHIM_ENGINE_SO"); long len; uint8_t *elf=load_file(path,&len);
    if(!elf){ printf("  SKIP: no engine\n"); return 0; }

    cpu_t cpu; cpu_create(&cpu); loader_t L; loader_load(&L,&cpu,elf,len);
    dispatch_t D; dispatch_install(&D,&cpu,&L); jni_state J; jni_install(&J,&cpu,1); D.jni=&J;
    /* install the REAL GEL scheduler so guest pthread_create/cond/mutex behave exactly as on
     * device (jni_entry.c:394-395): green threads actually run, task-queue invariants hold. */
    sched sch; sched_init(&sch,&cpu); D.sch=&sch; sch.fatal_ext=&D.fatal;
    g_sch=&sch; g_gt=sched_host_gthread(&sch);
    /* hook the engine's static __cxa_throw @0x85a1d0 (thumb): log thrown type + the caller. */
    uc_hook th; uc_hook_add(cpu.uc,&th,UC_HOOK_CODE,(void*)throw_hook,NULL,RG_ENGINE+0x85a1d0u,RG_ENGINE+0x85a1d0u);
    { static uc_hook pb; uc_hook_add(cpu.uc,&pb,UC_HOOK_CODE,(void*)parsebuf_hook,NULL,RG_ENGINE+0x68e814u,RG_ENGINE+0x68e814u); }
    { static uc_hook jp; uc_hook_add(cpu.uc,&jp,UC_HOOK_CODE,(void*)jsonparse_hook,NULL,RG_ENGINE+0x69814cu,RG_ENGINE+0x69814cu); }
    { static uc_hook sh; uc_hook_add(cpu.uc,&sh,UC_HOOK_CODE,(void*)scheme_hook,NULL,RG_ENGINE+0x6bd3e0u,RG_ENGINE+0x6bd3e0u); }
    uc_hook wp; uc_hook_add(cpu.uc,&wp,UC_HOOK_MEM_WRITE,(void*)gobj_wp,NULL,RG_ENGINE+0xabb120u,RG_ENGINE+0xabb12fu);
    uc_hook ph; uc_hook_add(cpu.uc,&ph,UC_HOOK_CODE,(void*)pc_hook,NULL,RG_ENGINE+0x1de2f8u,RG_ENGINE+0x1de31cu);
    uc_hook ph2; uc_hook_add(cpu.uc,&ph2,UC_HOOK_CODE,(void*)pc_hook,NULL,RG_ENGINE+0x1de4f0u,RG_ENGINE+0x1de4f0u);
    /* the [g+0x80] ctor: 0x72680 entry, 0x726a0=bl inner-ctor, 0x726a4=normal return-point.
     * if 0x726a4 fires but F never returns to 0x1de314 -> corrupted return (stack bug). */
    uc_hook ph3; uc_hook_add(cpu.uc,&ph3,UC_HOOK_CODE,(void*)pc_hook,NULL,RG_ENGINE+0x72680u,RG_ENGINE+0x726a8u);
    uc_hook vc; uc_hook_add(cpu.uc,&vc,UC_HOOK_CODE,(void*)visit_cnt,NULL,RG_ENGINE+0x7d2b28u,RG_ENGINE+0x7d2b28u);
    { static uc_hook mk[9]; uint32_t marks[9]={0x6f370,0x118f30,0x653384,0x7c4a1c,0x118fc4,0x6f3a8,0x726a4,0x1de318,0x1de4f0};
      for(int i=0;i<9;i++) uc_hook_add(cpu.uc,&mk[i],UC_HOOK_CODE,(void*)mark_hook,NULL,RG_ENGINE+marks[i],RG_ENGINE+marks[i]); }
    { static uc_hook lw; uc_hook_add(cpu.uc,&lw,UC_HOOK_MEM_WRITE,(void*)lr_watch,NULL,(uint64_t)RG_STACK,(uint64_t)RG_STACK+0x10000000ull); }
    { static uc_hook br; uc_hook_add(cpu.uc,&br,UC_HOOK_MEM_WRITE,(void*)badret_watch,NULL,(uint64_t)RG_STACK,(uint64_t)RG_STACK+0x10000000ull); }
    { static uc_hook gd; uc_hook_add(cpu.uc,&gd,UC_HOOK_CODE,(void*)gd_hook,NULL,(uint64_t)RG_GUESTDATA,(uint64_t)RG_GUESTDATA+0x2000ull); }
    { static uc_hook dm; uc_hook_add(cpu.uc,&dm,UC_HOOK_CODE,(void*)divmod_hook,NULL,RG_ENGINE+0x89c8ccull,RG_ENGINE+0x89c8ccull); }
    { static uc_hook b1; uc_hook_add(cpu.uc,&b1,UC_HOOK_BLOCK,(void*)bb118,NULL,RG_ENGINE+0x108064ull,RG_ENGINE+0x10a800ull); }
    { static uc_hook cbb; uc_hook_add(cpu.uc,&cbb,UC_HOOK_BLOCK,(void*)ctor_bb,NULL,(uint64_t)1,(uint64_t)0); } /* all blocks; gated by g_hon */
    int tot=0, ran=dispatch_run_init_array(&D,&tot); printf("  ctors %d/%d\n",ran,tot);
    int f=0;
    /* guest JNI_OnLoad runs via direct cpu_call, exactly like the real shim (jni_entry.c:405);
     * the scheduler is installed so any pthread_create here queues a green thread that the
     * first scheduler-driven native entry (nativeConfig/nativeInit) will run. */
    uint32_t onload=sym_addr(&L,"JNI_OnLoad"); if(onload){ D.fatal=0; cpu_call(&cpu,onload,J.vm,0,0,0,2000000000ull); }
    uint32_t thiz=ht_new_ref(J.ht,HK_LOCAL,(void*)0x1234);
    #define DRV(nm,a2,a3) do{ uint32_t s=sym_addr(&L,nm); if(s){ g_phase=nm+strlen("Java_com_rovio_fusion_NativeApplication_"); uint32_t r=call(&cpu,s,J.env,thiz,a2,a3,&f,&D); \
        uint32_t pc=0,lr=0; uc_reg_read(cpu.uc,UC_ARM_REG_PC,&pc); uc_reg_read(cpu.uc,UC_ARM_REG_LR,&lr); \
        printf("  %-14s -> r0=0x%x fatal=%d assetOK=%d GLerr=0x%x", nm+strlen("Java_com_rovio_fusion_NativeApplication_"), r, f, g_open_ok, glGetError()); \
        if(f){ const char*sn=loader_stub_name(&L,pc&~1u); printf("  [abort %s LR=+0x%x]", sn?sn:"?", lr>=RG_ENGINE?lr-RG_ENGINE:0);} printf("\n"); } }while(0)
    /* the Java layer calls nativeConfig(config) BEFORE nativeInit — it wires up the VFS /
     * config (likely where script_paths.json is produced/registered). Drive it first. */
    /* nativeConfig(String) takes the app's filesDir path (MySurfaceView.java:
     * new NativeApplication(this, app.getFilesDir().getAbsolutePath())). The engine uses it
     * as its base dir for the registry/save files + VFS. Pass a REAL writable host dir as a
     * genuine jstring (fake env now round-trips it) so the engine's file layer works. */
    { const char *fdir=getenv("ABSHIM_FILES_DIR"); if(!fdir||!*fdir) fdir="/tmp/abfiles";
      uint32_t cfg=jni_fake_intern_string(&J, fdir);
      uint32_t s=sym_addr(&L,"Java_com_rovio_fusion_NativeApplication_nativeConfig");
      if(s){ int ff=0; call(&cpu,s,J.env,thiz,cfg,0,&ff,&D); printf("  nativeConfig(filesDir=%s) -> fatal=%d assetOK=%d\n",fdir,ff,g_open_ok); } }
    /* which render model? nativeRenderThread()==1 => MultiThreadWrapper (nativeUpdate on a
     * game thread, nativeRender on the GL thread — separate threads with engine-internal
     * frame handoff); ==0 => SingleThreadWrapper (nativeUpdate does everything). */
    { uint32_t s=sym_addr(&L,"Java_com_rovio_fusion_NativeApplication_nativeRenderThread");
      if(s){ int ff=0; uint32_t r=call(&cpu,s,J.env,thiz,0,0,&ff,&D);
             printf("  nativeRenderThread -> %u (%s) fatal=%d\n", r, r?"MULTI-thread":"single-thread", ff); } }
    /* DEVICE ORDER (SingleThreadWrapper.initialize -> doInit): nativeInit -> nativeResume ->
     * nativeResize. The engine sets up its render/surface state during resize while RESUMED;
     * doing resize before resume (the old harness order) leaves it not-render-ready. */
    DRV("Java_com_rovio_fusion_NativeApplication_nativeInit",(uint32_t)W,(uint32_t)H);
    g_hon=0; ring2_dump(); ring2_dump_sp("nativeInit-exit");
    printf("  [visit] 0x7d2b28 per-node visitor called %lu times during nativeInit (millions => corrupted-tree walk reproduced)\n", g_visit);
    DRV("Java_com_rovio_fusion_NativeApplication_nativeResume",0,0);
    DRV("Java_com_rovio_fusion_NativeApplication_nativeResize",(uint32_t)W,(uint32_t)H);
    /* Run a realistic run of frames with ~16ms spacing so the engine's real-clock
     * (clock_gettime CLOCK_MONOTONIC) animation actually advances — a 3-frame burst in <1ms
     * of wall time can't get past a splash/loading fade. Sample non-black pixel count over
     * time to distinguish "still black" from "warming up". */
    unsigned char *px=malloc((size_t)W*H*4);
    #define NONBLACK(cnt) do{ glReadPixels(0,0,W,H,GL_RGBA,GL_UNSIGNED_BYTE,px); (cnt)=0; \
        for(long i=0;i<(long)W*H;i++) if(px[i*4]|px[i*4+1]|px[i*4+2]) (cnt)++; }while(0)
    int NF = getenv("ABSHIM_FRAMES")?atoi(getenv("ABSHIM_FRAMES")):120; if(NF<1)NF=1;
    uint32_t nu=sym_addr(&L,"Java_com_rovio_fusion_NativeApplication_nativeUpdate");
    uint32_t nr=sym_addr(&L,"Java_com_rovio_fusion_NativeApplication_nativeRender");
    long nb=0; int gerr=0;
    g_hon=0; g_ring2pos=0;   /* ctor_bb re-armed per-frame by 0x653384 (mark_hook); efficient: only the ctor frame records */
    static char phbuf[24];
    for(int i=0;i<NF;i++){
        int fu=0,fr=0;
        snprintf(phbuf,sizeof phbuf,"update#%d",i+1); g_phase=phbuf;
        unsigned ce0=g_ctor_enter; g_ring2pos=0;
        call(&cpu,nu,J.env,thiz,0,0,&fu,&D);
        if(g_ctor_enter>ce0){ char t[32]; snprintf(t,sizeof t,"update#%d-ctorframe",i+1); ring2_dump_sp(t); }
        if(i==0) ring2_dump_sp("frame1-nativeUpdate-exit");
        g_phase="render"; call(&cpu,nr,J.env,thiz,0,0,&fr,&D);
        gerr=glGetError();
        struct timespec ts={0,16*1000*1000}; nanosleep(&ts,0);
        if(i==0||i==9||i==29||i==59||i==NF-1){ NONBLACK(nb);
            printf("  frame %3d: non-black=%ld/%d (%.1f%%) fatal=%d GLerr=0x%x | GL draws=%lu clears=%lu useProgram=%lu\n",
                   i+1, nb, W*H, 100.0*nb/(W*H), fu|fr, gerr, g_gl_draws, g_gl_clears, g_gl_useprog); }
    }
    /* SANITY: prove the pbuffer readback path itself works — clear to red, read back.
     * If this shows ~100% red but the engine frames were black, the engine is drawing to an
     * FBO it never blits to fb0 (or is on a black splash), NOT a broken pbuffer/readback. */
    { GLint fb=-1; glGetIntegerv(GL_FRAMEBUFFER_BINDING,&fb); GLfloat cc[4]={0}; glGetFloatv(GL_COLOR_CLEAR_VALUE,cc);
      GLint vp[4]={0}; glGetIntegerv(GL_VIEWPORT,vp);
      printf("  GL state after loop: FRAMEBUFFER_BINDING=%d clearColor=(%.2f,%.2f,%.2f,%.2f) viewport=[%d,%d,%d,%d]\n",
             fb, cc[0],cc[1],cc[2],cc[3], vp[0],vp[1],vp[2],vp[3]);
      glBindFramebuffer(GL_FRAMEBUFFER,0); glViewport(0,0,W,H);
      glClearColor(1,0,0,1); glClear(GL_COLOR_BUFFER_BIT); long red=0; NONBLACK(red);
      printf("  pbuffer sanity (manual clear->red): non-black=%ld/%d (%.1f%%) -> readback %s\n",
             red, W*H, 100.0*red/(W*H), red>(long)W*H/2 ? "WORKS" : "BROKEN"); }
    /* re-render one engine frame (the sanity test clobbered fb0 with red) for the PPM dump */
    { int fu=0,fr=0; call(&cpu,nu,J.env,thiz,0,0,&fu,&D); call(&cpu,nr,J.env,thiz,0,0,&fr,&D); (void)fu;(void)fr; }
    long nonblack; NONBLACK(nonblack);
    printf("  framebuffer(final engine frame): %ld / %d non-black pixels (%.1f%%)\n", nonblack,W*H, 100.0*nonblack/(W*H));
    const char *out=getenv("ABSHIM_RENDER_OUT"); if(out){ FILE*o=fopen(out,"wb"); if(o){ fprintf(o,"P6\n%d %d\n255\n",W,H);
        for(long i=0;i<(long)W*H;i++) fwrite(px+i*4,1,3,o); fclose(o); printf("  wrote %s\n",out);} }
    printf("  assets OK=%d miss=%d; heap %s\n", g_open_ok,g_open_miss, galloc_check(cpu.heap)==0?"valid":"CORRUPT");
    printf("  --- final green-thread states (a worker stuck BLOCKED = the boot-stall culprit) ---\n");
    sched_dump_state(&sch);
    printf("\n=== boot-with-render drive complete ===\n");
    return 0;
}
