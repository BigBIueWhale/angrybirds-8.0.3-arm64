/* bridge_gl.c — GLES2 forwarding to the real arm64 libGLESv2 (Audit 07).
 * Real functions are resolved at runtime via dlsym (RTLD_DEFAULT) — so this
 * compiles for both host (dlsym -> NULL -> no-op) and device (real driver).
 * Marshalling per Audit 07: draws are VBO-offset passthrough (the "pointer" is
 * an integer offset, never copied); IN pointers (glBufferData/glTexImage2D/
 * glShaderSource/glUniform-fv) copy guest->host with the exact size; OUT
 * pointers (glGen-x, glGet-xv, InfoLog) copy host->guest; glGetString returns a
 * cached GUEST copy of the host C-string; all GL object IDs come from the driver. */
#include "cpu.h"
#include "marshal.h"
#include "galloc.h"
#include <dlfcn.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#ifdef __ANDROID__
#include <android/log.h>   /* device-only: one-line note when the client-array path engages */
#endif

static void *glsym(const char *n){
    void *p = dlsym(RTLD_DEFAULT, n);                 /* already-loaded libGLESv2 */
    if (!p){ static void *h = 0; if (!h) h = dlopen("libGLESv2.so", RTLD_NOW|RTLD_GLOBAL); if (h) p = dlsym(h, n); }
    return p;
}
#define W  marshal_pull_word(&c->mem, cur)
/* Zero-fill on a failed guest read rather than leaving the caller's buffer holding stack
 * residue - same contract as cpu.c's m_read. Every IN-parameter copy in this file uses RD. */
static void RD(cpu_t*c,uint32_t g,void*h,uint32_t n){ if(n && uc_mem_read(c->uc,g,h,n)!=UC_ERR_OK) memset(h,0,n); }
static void WR(cpu_t*c,uint32_t g,const void*h,uint32_t n){ if(n) uc_mem_write(c->uc,g,h,n); }

/* growable host scratch for IN/OUT pixel/buffer copies */
static uint8_t *g_tmp; static size_t g_tmpsz;
/* On realloc FAILURE the old code assigned NULL straight into g_tmp while g_tmpsz had already been
 * bumped to the requested size. That leaked the old block, and every later call - including small
 * ones the surviving buffer could have served - saw n <= g_tmpsz and returned the NULL. Keep the
 * old buffer and its true size on failure, and return NULL for THIS request only. Returning the
 * old (smaller) buffer would be worse than NULL: the caller would write n bytes into it. */
static uint8_t *tmp(size_t n){
    if(n>g_tmpsz){
        size_t want = n<4096?4096:n;
        uint8_t *p = realloc(g_tmp, want);
        if(!p) return NULL;
        g_tmp = p; g_tmpsz = want;
    }
    return g_tmp;
}
/* Guest-driven size for a scratch copy. Returns 0 if the product overflowed 32 bits or exceeds a
 * sane cap, so the caller can refuse rather than hand the GL driver a buffer smaller than the
 * dimensions it was also given - the driver would then read past the end of our allocation.
 * A texture or uniform array beyond this bound is not something this game produces. */
#define GL_SCRATCH_MAX (64u*1024u*1024u)
/* Returns 1 and writes the size, or 0 if the product overflowed / exceeded the cap. A size of ZERO
 * is a legitimate answer (a 0-width or 0-height texture is legal GL) and must be distinguishable
 * from a refusal - an earlier version folded both into "return 0", which would have silently
 * dropped zero-sized calls that previously reached the driver. */
/* External linkage so test/test_gl_sizes.c can link against it, but HIDDEN visibility so it does
 * not enter the shipped .so's dynamic symbol table - testability should not enlarge the ABI. */
__attribute__((visibility("hidden")))
int gl_gsize3(uint64_t a, uint64_t b, uint64_t c, uint32_t *out){
    uint64_t v = a*b*c;
    if(v > GL_SCRATCH_MAX) return 0;
    *out = (uint32_t)v; return 1;
}

static float f_of(uint32_t b){ float f; memcpy(&f,&b,4); return f; }
static uint32_t bpp(uint32_t fmt,uint32_t type){
    if(type==0x8363/*565*/||type==0x8033/*4444*/||type==0x8034/*5551*/) return 2;
    switch(fmt){ case 0x1908:/*RGBA*/return 4; case 0x1907:/*RGB*/return 3;
                 case 0x1909:/*LUMINANCE*/case 0x1906:/*ALPHA*/return 1; case 0x190A:/*LUMINANCE_ALPHA*/return 2; }
    return 4;
}

/* ---- client-side vertex-array support. GLES2 allows BOTH forms of glVertexAttribPointer:
 * with a VBO bound to GL_ARRAY_BUFFER the last arg is an integer offset INTO that buffer
 * (pass straight through — the driver reads its own buffer); with NO VBO bound it is a
 * CLIENT pointer into the app's (here: guest, emulated) memory, which the host driver
 * cannot read. So we classify each attrib at glVertexAttribPointer time by whether a VBO
 * is bound, and for client attribs we copy the guest vertex data to a host buffer at DRAW
 * time and hand the driver a host client pointer. The VBO path is byte-for-byte unchanged
 * (only genuine client arrays take the copy path). To size the copy we need the highest
 * vertex index touched: glDrawArrays gives it (first+count); glDrawElements needs the max
 * index — scanned from the index data (client indices copied here, VBO indices from a
 * snapshot we keep of every ELEMENT_ARRAY_BUFFER upload). ---- */
#define GL_MAX_VA 16
static struct { uint8_t en, client, norm; int size, stride; uint32_t type, gptr; } g_va[GL_MAX_VA];
static uint32_t g_arraybuf, g_elembuf;                    /* current ARRAY / ELEMENT_ARRAY bindings */
static uint8_t *g_vabuf[GL_MAX_VA]; static size_t g_vabufsz[GL_MAX_VA];   /* per-attrib host copies */
static uint8_t *g_idxbuf; static size_t g_idxbufsz;                       /* client-index host copy */
#define GL_MAX_EVBO 32
typedef struct { uint32_t id; uint8_t *data; uint32_t size; } gl_evbo_t;  /* snapshot of an index VBO */
static gl_evbo_t g_evbo[GL_MAX_EVBO]; static int g_nevbo;

static uint32_t gl_tsz(uint32_t t){ switch(t){ case 0x1400: case 0x1401: return 1;       /* (U)BYTE  */
    case 0x1402: case 0x1403: return 2; case 0x1406: case 0x140C: return 4; } return 4; } /* (U)SHORT / FLOAT|FIXED */
static uint32_t gl_isz(uint32_t t){ return t==0x1401?1u : t==0x1403?2u : t==0x1405?4u : 2u; }  /* index type size */
static int gl_any_client(void){ for(int i=0;i<GL_MAX_VA;i++) if(g_va[i].en && g_va[i].client) return 1; return 0; }
static uint32_t gl_idx_at(const uint8_t*p,uint32_t isz,uint32_t k){ return isz==1?p[k] : isz==2?((const uint16_t*)p)[k] : ((const uint32_t*)p)[k]; }
static gl_evbo_t *evbo_get(uint32_t id){
    for(int i=0;i<g_nevbo;i++) if(g_evbo[i].id==id) return &g_evbo[i];
    if(g_nevbo<GL_MAX_EVBO){ g_evbo[g_nevbo].id=id; g_evbo[g_nevbo].data=0; g_evbo[g_nevbo].size=0; return &g_evbo[g_nevbo++]; }
    return 0; }
/* copy every enabled CLIENT attrib's guest data [0,vhi) to a host buffer and point the
 * driver at it as a client array; VBO-backed attribs are left as the driver already has them. */
static void gl_setup_client(cpu_t*c, uint32_t vhi){
#ifdef __ANDROID__
    { static int logged; if(!logged){ logged=1; int n=0; for(int i=0;i<GL_MAX_VA;i++) if(g_va[i].en&&g_va[i].client)n++;
      __android_log_print(4,"abshim","GL: client-side vertex arrays engaged (%d client attribs, first vhi=%u)",n,vhi); } }
#endif
    void (*bb)(uint32_t,uint32_t) = (void(*)(uint32_t,uint32_t))glsym("glBindBuffer");
    void (*vap)(uint32_t,int,uint32_t,uint8_t,int,const void*) =
        (void(*)(uint32_t,int,uint32_t,uint8_t,int,const void*))glsym("glVertexAttribPointer");
    if(bb) bb(0x8892,0);                                   /* client pointers require ARRAY_BUFFER unbound */
    for(int i=0;i<GL_MAX_VA;i++) if(g_va[i].en && g_va[i].client){
        uint32_t str = g_va[i].stride ? (uint32_t)g_va[i].stride : (uint32_t)g_va[i].size*gl_tsz(g_va[i].type);
        uint32_t bytes = vhi*str; if(!bytes) continue;
        if(bytes>g_vabufsz[i]){ uint8_t *nb=realloc(g_vabuf[i],bytes); if(!nb) continue; g_vabuf[i]=nb; g_vabufsz[i]=bytes; }
        if(!g_vabuf[i]) continue;
        RD(c, g_va[i].gptr, g_vabuf[i], bytes);
        if(vap) vap((uint32_t)i,g_va[i].size,g_va[i].type,g_va[i].norm,g_va[i].stride,g_vabuf[i]);
    }
}
static void gl_restore_arraybuf(void){ if(g_arraybuf){ void(*bb)(uint32_t,uint32_t)=(void(*)(uint32_t,uint32_t))glsym("glBindBuffer"); if(bb) bb(0x8892,g_arraybuf); } }

typedef uint64_t (*glh)(cpu_t*, mcur*);
#define DEF(name) static uint64_t name(cpu_t*c, mcur*cur)
#define REAL(rt,args,nm) static rt (*f)args=0; if(!f)f=(rt(*)args)glsym(nm); if(!f) return 0

/* diagnostic counters — how many draws/clears/program-binds the engine issued. Read by the
 * host render harness to distinguish "drawing but black" from "not drawing (awaiting content/
 * lifecycle)". A plain increment per call, negligible cost. */
unsigned long g_gl_draws=0, g_gl_clears=0, g_gl_useprog=0;

/* --- scalar passthrough (ints) --- */
DEF(h_1u){ (void)c; uint32_t a=W; (void)a; return 0; }   /* placeholder, unused */
DEF(h_glClear){ REAL(void,(uint32_t),"glClear"); g_gl_clears++; f(W); return 0; }
DEF(h_glEnable){ REAL(void,(uint32_t),"glEnable"); f(W); return 0; }
DEF(h_glDisable){ REAL(void,(uint32_t),"glDisable"); f(W); return 0; }
DEF(h_glActiveTexture){ REAL(void,(uint32_t),"glActiveTexture"); f(W); return 0; }
DEF(h_glCullFace){ REAL(void,(uint32_t),"glCullFace"); f(W); return 0; }
DEF(h_glFrontFace){ REAL(void,(uint32_t),"glFrontFace"); f(W); return 0; }
DEF(h_glDepthFunc){ REAL(void,(uint32_t),"glDepthFunc"); f(W); return 0; }
DEF(h_glDepthMask){ REAL(void,(uint32_t),"glDepthMask"); f(W); return 0; }
DEF(h_glBlendEquation){ REAL(void,(uint32_t),"glBlendEquation"); f(W); return 0; }
DEF(h_glUseProgram){ REAL(void,(uint32_t),"glUseProgram"); g_gl_useprog++; f(W); return 0; }
DEF(h_glEnableVAA){ REAL(void,(uint32_t),"glEnableVertexAttribArray"); uint32_t i=W; if(i<GL_MAX_VA)g_va[i].en=1; f(i); return 0; }
DEF(h_glDisableVAA){ REAL(void,(uint32_t),"glDisableVertexAttribArray"); uint32_t i=W; if(i<GL_MAX_VA)g_va[i].en=0; f(i); return 0; }
DEF(h_glCompileShader){ REAL(void,(uint32_t),"glCompileShader"); f(W); return 0; }
DEF(h_glLinkProgram){ REAL(void,(uint32_t),"glLinkProgram"); f(W); return 0; }
DEF(h_glDeleteShader){ REAL(void,(uint32_t),"glDeleteShader"); f(W); return 0; }
DEF(h_glDeleteProgram){ REAL(void,(uint32_t),"glDeleteProgram"); f(W); return 0; }
DEF(h_glGenerateMipmap){ REAL(void,(uint32_t),"glGenerateMipmap"); f(W); return 0; }
DEF(h_glBind2){ /* generic (target,id) */ (void)c; return 0; }
DEF(h_glBindBuffer){ REAL(void,(uint32_t,uint32_t),"glBindBuffer"); uint32_t a=W,b=W;
    if(a==0x8892) g_arraybuf=b; else if(a==0x8893) g_elembuf=b; f(a,b); return 0; }
DEF(h_glBindTexture){ REAL(void,(uint32_t,uint32_t),"glBindTexture"); uint32_t a=W,b=W; f(a,b); return 0; }
DEF(h_glBindFramebuffer){ REAL(void,(uint32_t,uint32_t),"glBindFramebuffer"); uint32_t a=W,b=W; f(a,b); return 0; }
DEF(h_glBindRenderbuffer){ REAL(void,(uint32_t,uint32_t),"glBindRenderbuffer"); uint32_t a=W,b=W; f(a,b); return 0; }
DEF(h_glBlendFunc){ REAL(void,(uint32_t,uint32_t),"glBlendFunc"); uint32_t a=W,b=W; f(a,b); return 0; }
DEF(h_glAttachShader){ REAL(void,(uint32_t,uint32_t),"glAttachShader"); uint32_t a=W,b=W; f(a,b); return 0; }
DEF(h_glDetachShader){ REAL(void,(uint32_t,uint32_t),"glDetachShader"); uint32_t a=W,b=W; f(a,b); return 0; }
DEF(h_glHint){ REAL(void,(uint32_t,uint32_t),"glHint"); uint32_t a=W,b=W; f(a,b); return 0; }
DEF(h_glPixelStorei){ REAL(void,(uint32_t,int),"glPixelStorei"); uint32_t a=W,b=W; f(a,(int)b); return 0; }
DEF(h_glUniform1i){ REAL(void,(int,int),"glUniform1i"); uint32_t a=W,b=W; f((int)a,(int)b); return 0; }
DEF(h_glTexParameteri){ REAL(void,(uint32_t,uint32_t,int),"glTexParameteri"); uint32_t a=W,b=W,d=W; f(a,b,(int)d); return 0; }
DEF(h_glStencilOp){ REAL(void,(uint32_t,uint32_t,uint32_t),"glStencilOp"); uint32_t a=W,b=W,d=W; f(a,b,d); return 0; }
DEF(h_glDrawArrays){ REAL(void,(uint32_t,int,int),"glDrawArrays"); g_gl_draws++; uint32_t a=W; int b=(int)W,d=(int)W;
    if(d>0 && gl_any_client()){ gl_setup_client(c,(uint32_t)(b+d)); f(a,b,d); gl_restore_arraybuf(); }
    else f(a,b,d);
    return 0; }
DEF(h_glViewport){ REAL(void,(int,int,int,int),"glViewport"); int a=W,b=W,d=W,e=W; f(a,b,d,e); return 0; }
DEF(h_glScissor){ REAL(void,(int,int,int,int),"glScissor"); int a=W,b=W,d=W,e=W; f(a,b,d,e); return 0; }
DEF(h_glColorMask){ REAL(void,(uint32_t,uint32_t,uint32_t,uint32_t),"glColorMask"); uint32_t a=W,b=W,d=W,e=W; f(a,b,d,e); return 0; }
DEF(h_glFramebufferRenderbuffer){ REAL(void,(uint32_t,uint32_t,uint32_t,uint32_t),"glFramebufferRenderbuffer"); uint32_t a=W,b=W,d=W,e=W; f(a,b,d,e); return 0; }
DEF(h_glRenderbufferStorage){ REAL(void,(uint32_t,uint32_t,int,int),"glRenderbufferStorage"); uint32_t a=W,b=W,d=W,e=W; f(a,b,(int)d,(int)e); return 0; }
DEF(h_glFramebufferTexture2D){ REAL(void,(uint32_t,uint32_t,uint32_t,uint32_t,int),"glFramebufferTexture2D"); uint32_t a=W,b=W,d=W,e=W,g=W; f(a,b,d,e,(int)g); return 0; }

/* --- float args (soft-float bit patterns -> float regs) --- */
DEF(h_glClearColor){ REAL(void,(float,float,float,float),"glClearColor"); uint32_t a=W,b=W,d=W,e=W; f(f_of(a),f_of(b),f_of(d),f_of(e)); return 0; }
DEF(h_glClearDepthf){ REAL(void,(float),"glClearDepthf"); f(f_of(W)); return 0; }
DEF(h_glLineWidth){ REAL(void,(float),"glLineWidth"); f(f_of(W)); return 0; }
DEF(h_glUniform1f){ REAL(void,(int,float),"glUniform1f"); uint32_t a=W,b=W; f((int)a,f_of(b)); return 0; }
DEF(h_glUniform4f){ REAL(void,(int,float,float,float,float),"glUniform4f"); uint32_t a=W,b=W,d=W,e=W,g=W; f((int)a,f_of(b),f_of(d),f_of(e),f_of(g)); return 0; }

/* --- create / query returning a value --- */
DEF(h_glCreateShader){ REAL(uint32_t,(uint32_t),"glCreateShader"); return f(W); }
DEF(h_glCreateProgram){ REAL(uint32_t,(void),"glCreateProgram"); (void)c;(void)cur; return f(); }
DEF(h_glCheckFramebufferStatus){ REAL(uint32_t,(uint32_t),"glCheckFramebufferStatus"); return f(W); }
DEF(h_glGetError){ REAL(uint32_t,(void),"glGetError"); (void)cur; return f(); }
DEF(h_glGetAttribLocation){ REAL(int,(uint32_t,const char*),"glGetAttribLocation"); uint32_t p=W,s=W; char nm[256]; uint32_t i=0; for(;i<255;i++){uint8_t ch=0;if(uc_mem_read(c->uc,s+i,&ch,1)!=UC_ERR_OK||!ch)break;nm[i]=ch;} nm[i]=0; return (uint32_t)f(p,nm); }
DEF(h_glGetUniformLocation){ REAL(int,(uint32_t,const char*),"glGetUniformLocation"); uint32_t p=W,s=W; char nm[256]; uint32_t i=0; for(;i<255;i++){uint8_t ch=0;if(uc_mem_read(c->uc,s+i,&ch,1)!=UC_ERR_OK||!ch)break;nm[i]=ch;} nm[i]=0; return (uint32_t)f(p,nm); }

/* --- VBO-offset passthrough draws (Audit 07: offset is an integer, never copied) --- */
DEF(h_glVertexAttribPointer){ REAL(void,(uint32_t,int,uint32_t,uint8_t,int,const void*),"glVertexAttribPointer");
    uint32_t idx=W; int sz=(int)W; uint32_t ty=W; uint8_t nrm=(uint8_t)W; int str=(int)W; uint32_t off=W;
    if(idx<GL_MAX_VA){ g_va[idx].size=sz; g_va[idx].type=ty; g_va[idx].norm=nrm; g_va[idx].stride=str;
                       g_va[idx].gptr=off; g_va[idx].client=(g_arraybuf==0); }
    if(g_arraybuf) f(idx,sz,ty,nrm,str,(const void*)(size_t)off);  /* VBO: offset passthrough (unchanged); client: set up at draw */
    return 0; }
DEF(h_glDrawElements){ REAL(void,(uint32_t,int,uint32_t,const void*),"glDrawElements"); g_gl_draws++;
    uint32_t mode=W; int cnt=(int)W; uint32_t ty=W; uint32_t off=W;
    if(cnt<=0){ f(mode,cnt,ty,(const void*)(size_t)off); return 0; }
    const void *idxp=(const void*)(size_t)off; uint32_t isz=gl_isz(ty);
    if(g_elembuf==0){                                   /* client-side indices: copy guest -> host */
        uint32_t ib=(uint32_t)cnt*isz;
        /* Same shape as the tmp() bug fixed earlier: the size was raised BEFORE the realloc, so a
         * failure left g_idxbufsz claiming a capacity the (now NULL) buffer did not have, and every
         * later smaller request skipped the realloc and found NULL forever. Commit only on success. */
        if(ib>g_idxbufsz){ uint8_t *nb=realloc(g_idxbuf,ib); if(nb){ g_idxbuf=nb; g_idxbufsz=ib; } }
        if(g_idxbuf){ RD(c,off,g_idxbuf,ib); idxp=g_idxbuf; }
    }
    if(gl_any_client()){
        uint32_t vhi=0;
        if(g_elembuf==0 && g_idxbuf){                   /* scan the client indices we just copied */
            for(int k=0;k<cnt;k++){ uint32_t v=gl_idx_at(g_idxbuf,isz,(uint32_t)k)+1; if(v>vhi)vhi=v; }
        } else if(g_elembuf){                           /* VBO indices: scan our snapshot of that buffer */
            gl_evbo_t *e=evbo_get(g_elembuf);
            if(e && e->data){ for(int k=0;k<cnt;k++){ uint32_t bo=off+(uint32_t)k*isz; if(bo+isz>e->size)break;
                uint32_t v=gl_idx_at(e->data+bo,isz,0)+1; if(v>vhi)vhi=v; } }
        }
        if(vhi){ gl_setup_client(c,vhi); f(mode,cnt,ty,idxp); gl_restore_arraybuf(); return 0; }
    }
    f(mode,cnt,ty,idxp); return 0; }

/* --- IN pointer, explicit size --- */
DEF(h_glBufferData){ REAL(void,(uint32_t,long,const void*,uint32_t),"glBufferData");
    uint32_t tgt=W,size=W,data=W,usage=W; void*h=0; if(data){ h=tmp(size); if(!h) return 0; RD(c,data,h,size);} f(tgt,(long)size,h,usage);
    if(tgt==0x8893 && g_elembuf){                       /* snapshot the index buffer for client-vertex glDrawElements */
        gl_evbo_t *e=evbo_get(g_elembuf);
        if(e){ e->data=realloc(e->data,size?size:1); if(e->data){ e->size=size; if(h&&size)memcpy(e->data,h,size); else if(size)memset(e->data,0,size);} else e->size=0; }
    }
    return 0; }
DEF(h_glBufferSubData){ REAL(void,(uint32_t,long,long,const void*),"glBufferSubData");
    uint32_t tgt=W,off=W,size=W,data=W; void*h=0; if(data){ h=tmp(size); if(!h) return 0; RD(c,data,h,size);} f(tgt,(long)off,(long)size,h);
    if(tgt==0x8893 && g_elembuf && h){                  /* keep the index snapshot current */
        gl_evbo_t *e=evbo_get(g_elembuf);
        if(e && e->data && (uint64_t)off+size<=e->size) memcpy(e->data+off,h,size);
    }
    return 0; }
DEF(h_glTexImage2D){ REAL(void,(uint32_t,int,int,int,int,int,uint32_t,uint32_t,const void*),"glTexImage2D");
    uint32_t tgt=W; int lvl=(int)W,ifmt=(int)W,w=(int)W,ht=(int)W,bd=(int)W; uint32_t fmt=W,ty=W,px=W;
    void*h=0; uint32_t n=0;
    if(px){ if(!gl_gsize3((uint32_t)w,bpp(fmt,ty),(uint32_t)ht,&n)) return 0; h=tmp(n?n:1); if(!h) return 0; RD(c,px,h,n);} f(tgt,lvl,ifmt,w,ht,bd,fmt,ty,h); return 0; }
DEF(h_glTexSubImage2D){ REAL(void,(uint32_t,int,int,int,int,int,uint32_t,uint32_t,const void*),"glTexSubImage2D");
    uint32_t tgt=W; int lvl=(int)W,xo=(int)W,yo=(int)W,w=(int)W,ht=(int)W; uint32_t fmt=W,ty=W,px=W;
    void*h=0; uint32_t n=0;
    if(px){ if(!gl_gsize3((uint32_t)w,bpp(fmt,ty),(uint32_t)ht,&n)) return 0; h=tmp(n?n:1); if(!h) return 0; RD(c,px,h,n);} f(tgt,lvl,xo,yo,w,ht,fmt,ty,h); return 0; }
DEF(h_glCompressedTexImage2D){ REAL(void,(uint32_t,int,uint32_t,int,int,int,int,const void*),"glCompressedTexImage2D");
    uint32_t tgt=W; int lvl=(int)W; uint32_t ifmt=W; int w=(int)W,ht=(int)W,bd=(int)W,isz=(int)W; uint32_t data=W;
    void*h=0; if(data){ h=tmp(isz); if(!h) return 0; RD(c,data,h,isz);} f(tgt,lvl,ifmt,w,ht,bd,isz,h); return 0; }
DEF(h_glUniformMatrix4fv){ REAL(void,(int,int,uint8_t,const float*),"glUniformMatrix4fv");
    int loc=(int)W,cnt=(int)W; uint8_t tr=(uint8_t)W; uint32_t v=W; uint32_t n=0; if(!gl_gsize3((uint32_t)cnt,16,4,&n)) return 0; float*h=(float*)tmp(n?n:1); if(!h) return 0; RD(c,v,h,n); f(loc,cnt,tr,h); return 0; }
DEF(h_glShaderSource){ REAL(void,(uint32_t,int,const char*const*,const int*),"glShaderSource");
    uint32_t sh=W; int cnt=(int)W; uint32_t strp=W,lenp=W;
    /* cnt and every length come from the guest. A negative or absurd cnt made calloc fail (or be
     * called with a wrapped size), and the NULL was then indexed. Bound it: real shader sources are
     * a handful of strings of a few KB. */
    #define GL_SRC_MAXCNT 4096u
    #define GL_SRC_MAXLEN (1u<<20)
    if(cnt<=0 || (unsigned)cnt>GL_SRC_MAXCNT) return 0;
    char**hs=calloc((size_t)cnt,sizeof(char*)); int*hl=lenp?calloc((size_t)cnt,sizeof(int)):0;
    if(!hs){ free(hl); return 0; }
    for(int i=0;i<cnt;i++){ uint32_t sp=0; RD(c,strp+(uint32_t)i*4,&sp,4); int L=-1; if(lenp){ RD(c,lenp+(uint32_t)i*4,&L,4);}
        uint32_t sl = L>=0? (uint32_t)L : 0;
        if(L<0){
            /* NUL-scan, now BOUNDED and error-checked. uc_mem_read's result was ignored here, so on
             * an unmapped page `ch` was uninitialized: the scan could run far past the string and
             * sl could keep growing (and wrap), after which malloc(sl+1) wrapped to 0 and the
             * hs[i][sl] terminator became a wild write. */
            for(; sl<GL_SRC_MAXLEN; sl++){ uint8_t ch=0; if(uc_mem_read(c->uc,sp+sl,&ch,1)!=UC_ERR_OK || !ch) break; }
        }
        if(sl>GL_SRC_MAXLEN) sl=GL_SRC_MAXLEN;
        hs[i]=malloc((size_t)sl+1);
        /* Bail out entirely rather than hand the driver a NULL entry - whether a GL implementation
         * tolerates that is not something to assume. The shader simply is not sourced. */
        if(!hs[i]){ for(int j=0;j<i;j++) free(hs[j]); free(hs); free(hl); return 0; }
        RD(c,sp,hs[i],sl); hs[i][sl]=0; if(hl)hl[i]=(int)sl; }
#ifndef ABSHIM_RELEASE
    /* DIAG: dump the shader the guest is about to compile. The A56's Mali/Xclipse driver is the one
     * surface no emulator here stands in for (OPEN_FINDINGS R9), and shader compilation is its most
     * likely failure: SwiftShader is lenient, a real GLES driver is not. The source cannot be read
     * statically — there is no .vsh/.fsh in the 3217 asset entries and no gl_Position/varying string
     * in the engine or libjs, so it is assembled at runtime and this bridge is the only place the
     * final text exists. Dumping it here is what makes a portability screen possible at all.
     * Non-release only: the shipped APK is unchanged. */
    /* CHUNKED. A first version logged %.700s and captured only the leading #define block — the
     * shaders here are ONE part of 6000-9150 bytes, so the body (precision qualifiers, gl_FragColor,
     * texture2D) fell outside the window. Screening that capture found zero of every construct,
     * which reads like "no portability risks" and actually meant "no data". Emit the whole thing in
     * pieces small enough that logcat cannot truncate them, tagged so they reassemble in order. */
    { static int n=0;
      if(n++ < 8){
          for(int i=0;i<cnt && i<2;i++){
              const char *t = hs[i] ? hs[i] : "";
              int L = (int)strlen(t);
              for(int off=0, k=0; off<L; off+=480, k++){
                  char buf[512];
                  int m = L-off; if(m>480) m=480;
                  memcpy(buf,t+off,(size_t)m); buf[m]=0;
                  for(int j=0;j<m;j++) if(buf[j]=='\n'||buf[j]=='\r') buf[j]=' ';  /* keep one logcat line */
#ifdef __ANDROID__
                  __android_log_print(4,"abshim","[shader-src] sh=%u p=%d c=%d len=%d :%s", sh, i, k, L, buf);
#else
                  fprintf(stderr,"[shader-src] sh=%u p=%d c=%d len=%d :%s\n", sh, i, k, L, buf);
#endif
              }
          }
      } }
#endif
    f(sh,cnt,(const char*const*)hs,hl);
    for(int i=0;i<cnt;i++) free(hs[i]);
    free(hs); free(hl); return 0; }

/* --- OUT arrays / scalars --- */
DEF(h_glGenBuffers){ REAL(void,(int,uint32_t*),"glGenBuffers"); int n=(int)W; uint32_t g=W; uint32_t*h=(uint32_t*)tmp(n*4); if(!h) return 0; f(n,h); WR(c,g,h,n*4); return 0; }
DEF(h_glGenTextures){ REAL(void,(int,uint32_t*),"glGenTextures"); int n=(int)W; uint32_t g=W; uint32_t*h=(uint32_t*)tmp(n*4); if(!h) return 0; f(n,h); WR(c,g,h,n*4); return 0; }
DEF(h_glGenFramebuffers){ REAL(void,(int,uint32_t*),"glGenFramebuffers"); int n=(int)W; uint32_t g=W; uint32_t*h=(uint32_t*)tmp(n*4); if(!h) return 0; f(n,h); WR(c,g,h,n*4); return 0; }
DEF(h_glGenRenderbuffers){ REAL(void,(int,uint32_t*),"glGenRenderbuffers"); int n=(int)W; uint32_t g=W; uint32_t*h=(uint32_t*)tmp(n*4); if(!h) return 0; f(n,h); WR(c,g,h,n*4); return 0; }
DEF(h_glDeleteBuffers){ REAL(void,(int,const uint32_t*),"glDeleteBuffers"); int n=(int)W; uint32_t g=W; uint32_t*h=(uint32_t*)tmp(n*4); if(!h) return 0; RD(c,g,h,n*4); f(n,h); return 0; }
DEF(h_glDeleteTextures){ REAL(void,(int,const uint32_t*),"glDeleteTextures"); int n=(int)W; uint32_t g=W; uint32_t*h=(uint32_t*)tmp(n*4); if(!h) return 0; RD(c,g,h,n*4); f(n,h); return 0; }
DEF(h_glDeleteFramebuffers){ REAL(void,(int,const uint32_t*),"glDeleteFramebuffers"); int n=(int)W; uint32_t g=W; uint32_t*h=(uint32_t*)tmp(n*4); if(!h) return 0; RD(c,g,h,n*4); f(n,h); return 0; }
DEF(h_glDeleteRenderbuffers){ REAL(void,(int,const uint32_t*),"glDeleteRenderbuffers"); int n=(int)W; uint32_t g=W; uint32_t*h=(uint32_t*)tmp(n*4); if(!h) return 0; RD(c,g,h,n*4); f(n,h); return 0; }
DEF(h_glGetShaderiv){ REAL(void,(uint32_t,uint32_t,int*),"glGetShaderiv"); uint32_t s=W,pn=W,p=W; int v=0; f(s,pn,&v); WR(c,p,&v,4); return 0; }
DEF(h_glGetProgramiv){ REAL(void,(uint32_t,uint32_t,int*),"glGetProgramiv"); uint32_t s=W,pn=W,p=W; int v=0; f(s,pn,&v); WR(c,p,&v,4); return 0; }
DEF(h_glGetIntegerv){ REAL(void,(uint32_t,int*),"glGetIntegerv"); uint32_t pn=W,p=W;
    /* CRITICAL: write back ONLY the number of GLints this pname returns. The old code always wrote 16 bytes
     * (4 GLints), so a single-value query (e.g. glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &local))
     * overflowed the caller's 4-byte stack slot by 12 bytes -> STACK-CANARY SMASH -> __stack_chk_fail -> abort
     * at the level-load (Renderer/texture setup). Nearly all pnames return 1; a handful return 2 or 4. */
    int n=1;
    switch(pn){
    case 0x0BA2: case 0x0C10: case 0x0C22: case 0x0C23: case 0x8005: n=4; break; /* VIEWPORT/SCISSOR_BOX/COLOR_CLEAR_VALUE/COLOR_WRITEMASK/BLEND_COLOR */
    case 0x0D3A: case 0x846D: case 0x846E: case 0x0B70: n=2; break;              /* MAX_VIEWPORT_DIMS/ALIASED_POINT_SIZE_RANGE/ALIASED_LINE_WIDTH_RANGE/DEPTH_RANGE */
    case 0x86A3: { int nf=0; f(0x86A2,&nf); n=(nf>0&&nf<=256)?nf:0; } break;      /* COMPRESSED_TEXTURE_FORMATS: NUM_COMPRESSED_TEXTURE_FORMATS values */
    default: n=1; break;
    }
    int v[256]; memset(v,0,sizeof v);
    if(n>0){ f(pn,v); if(n>256)n=256; WR(c,p,v,(uint32_t)n*4u); }
    return 0; }
DEF(h_glGetShaderInfoLog){ REAL(void,(uint32_t,int,int*,char*),"glGetShaderInfoLog"); uint32_t s=W; int bs=(int)W; uint32_t lp=W,sp=W; char*h=(char*)tmp(bs+1); if(!h) return 0; int L=0; f(s,bs,&L,h); if(lp)WR(c,lp,&L,4); WR(c,sp,h,(uint32_t)L+1); return 0; }
DEF(h_glGetProgramInfoLog){ REAL(void,(uint32_t,int,int*,char*),"glGetProgramInfoLog"); uint32_t s=W; int bs=(int)W; uint32_t lp=W,sp=W; char*h=(char*)tmp(bs+1); if(!h) return 0; int L=0; f(s,bs,&L,h); if(lp)WR(c,lp,&L,4); WR(c,sp,h,(uint32_t)L+1); return 0; }

/* --- frame sync / readback / uniform introspection (Audit 07 gap: were UNIMPL->0) --- */
DEF(h_glFinish){ REAL(void,(void),"glFinish"); (void)cur; f(); return 0; }
DEF(h_glFlush){ REAL(void,(void),"glFlush"); (void)cur; f(); return 0; }
DEF(h_glValidateProgram){ REAL(void,(uint32_t),"glValidateProgram"); f(W); return 0; }
DEF(h_glCopyTexImage2D){ REAL(void,(uint32_t,int,uint32_t,int,int,int,int,int),"glCopyTexImage2D");
    uint32_t t=W; int l=(int)W; uint32_t i=W; int x=(int)W,y=(int)W,w=(int)W,h=(int)W,b=(int)W; f(t,l,i,x,y,w,h,b); return 0; }
DEF(h_glReadPixels){ REAL(void,(int,int,int,int,uint32_t,uint32_t,void*),"glReadPixels");
    int x=(int)W,y=(int)W,w=(int)W,h=(int)W; uint32_t fmt=W,type=W,p=W;
    size_t n=(size_t)(w<0?0:w)*(size_t)(h<0?0:h)*bpp(fmt,type); if(n>32u*1024*1024) return 0;
    uint8_t*hb=tmp(n?n:1); if(!hb) return 0; f(x,y,w,h,fmt,type,hb); WR(c,p,hb,(uint32_t)n); return 0; }
DEF(h_glGetActiveUniform){ REAL(void,(uint32_t,uint32_t,int,int*,int*,uint32_t*,char*),"glGetActiveUniform");
    uint32_t prog=W,idx=W; int bs=(int)W; uint32_t lp=W,szp=W,tp=W,np=W; if(bs<0)bs=0;
    int len=0,sz=0; uint32_t typ=0; char*nm=(char*)tmp((size_t)bs+1); if(!nm) return 0;
    f(prog,idx,bs,&len,&sz,&typ,nm); if(lp)WR(c,lp,&len,4); if(szp)WR(c,szp,&sz,4); if(tp)WR(c,tp,&typ,4); if(np)WR(c,np,nm,(uint32_t)len+1); return 0; }
DEF(h_glGetUniformfv){ REAL(void,(uint32_t,int,float*),"glGetUniformfv");
    uint32_t prog=W; int loc=(int)W; uint32_t p=W; float v[16]; memset(v,0,sizeof v); f(prog,loc,v); WR(c,p,v,sizeof v); return 0; }

/* --- glGetString: host char* -> cached guest copy --- */
static uint32_t g_strcache[16]; static uint32_t g_strkey[16]; static int g_nstr;
DEF(h_glGetString){ REAL(const char*,(uint32_t),"glGetString"); uint32_t name=W;
    for(int i=0;i<g_nstr;i++) if(g_strkey[i]==name) return g_strcache[i];
    const char*s=f(name); if(!s) return 0; uint32_t L=strlen(s); uint32_t g=galloc_malloc(c->heap,L+1);
    if(g){ WR(c,g,s,L); uint8_t z=0; WR(c,g+L,&z,1); if(g_nstr<16){ g_strkey[g_nstr]=name; g_strcache[g_nstr]=g; g_nstr++; } }
    return g; }

/* --- name -> handler + return class (0 void/word) --- */
static const struct { const char*n; glh h; } GLT[] = {
 {"glClear",h_glClear},{"glEnable",h_glEnable},{"glDisable",h_glDisable},{"glActiveTexture",h_glActiveTexture},
 {"glCullFace",h_glCullFace},{"glFrontFace",h_glFrontFace},{"glDepthFunc",h_glDepthFunc},{"glDepthMask",h_glDepthMask},
 {"glBlendEquation",h_glBlendEquation},{"glUseProgram",h_glUseProgram},{"glEnableVertexAttribArray",h_glEnableVAA},
 {"glDisableVertexAttribArray",h_glDisableVAA},{"glCompileShader",h_glCompileShader},{"glLinkProgram",h_glLinkProgram},
 {"glDeleteShader",h_glDeleteShader},{"glDeleteProgram",h_glDeleteProgram},{"glGenerateMipmap",h_glGenerateMipmap},
 {"glBindBuffer",h_glBindBuffer},{"glBindTexture",h_glBindTexture},{"glBindFramebuffer",h_glBindFramebuffer},
 {"glBindRenderbuffer",h_glBindRenderbuffer},{"glBlendFunc",h_glBlendFunc},{"glAttachShader",h_glAttachShader},
 {"glDetachShader",h_glDetachShader},{"glHint",h_glHint},{"glPixelStorei",h_glPixelStorei},{"glUniform1i",h_glUniform1i},
 {"glTexParameteri",h_glTexParameteri},{"glStencilOp",h_glStencilOp},{"glDrawArrays",h_glDrawArrays},
 {"glViewport",h_glViewport},{"glScissor",h_glScissor},{"glColorMask",h_glColorMask},
 {"glFramebufferRenderbuffer",h_glFramebufferRenderbuffer},{"glRenderbufferStorage",h_glRenderbufferStorage},
 {"glFramebufferTexture2D",h_glFramebufferTexture2D},{"glClearColor",h_glClearColor},{"glClearDepthf",h_glClearDepthf},
 {"glLineWidth",h_glLineWidth},{"glUniform1f",h_glUniform1f},{"glUniform4f",h_glUniform4f},
 {"glCreateShader",h_glCreateShader},{"glCreateProgram",h_glCreateProgram},{"glCheckFramebufferStatus",h_glCheckFramebufferStatus},
 {"glGetError",h_glGetError},{"glGetAttribLocation",h_glGetAttribLocation},{"glGetUniformLocation",h_glGetUniformLocation},
 {"glVertexAttribPointer",h_glVertexAttribPointer},{"glDrawElements",h_glDrawElements},{"glBufferData",h_glBufferData},
 {"glBufferSubData",h_glBufferSubData},{"glTexImage2D",h_glTexImage2D},{"glTexSubImage2D",h_glTexSubImage2D},
 {"glCompressedTexImage2D",h_glCompressedTexImage2D},{"glUniformMatrix4fv",h_glUniformMatrix4fv},{"glShaderSource",h_glShaderSource},
 {"glGenBuffers",h_glGenBuffers},{"glGenTextures",h_glGenTextures},{"glGenFramebuffers",h_glGenFramebuffers},
 {"glGenRenderbuffers",h_glGenRenderbuffers},{"glDeleteBuffers",h_glDeleteBuffers},{"glDeleteTextures",h_glDeleteTextures},
 {"glDeleteFramebuffers",h_glDeleteFramebuffers},{"glDeleteRenderbuffers",h_glDeleteRenderbuffers},
 {"glGetShaderiv",h_glGetShaderiv},{"glGetProgramiv",h_glGetProgramiv},{"glGetIntegerv",h_glGetIntegerv},
 {"glGetShaderInfoLog",h_glGetShaderInfoLog},{"glGetProgramInfoLog",h_glGetProgramInfoLog},{"glGetString",h_glGetString},
 {"glFinish",h_glFinish},{"glFlush",h_glFlush},{"glValidateProgram",h_glValidateProgram},{"glCopyTexImage2D",h_glCopyTexImage2D},
 {"glReadPixels",h_glReadPixels},{"glGetActiveUniform",h_glGetActiveUniform},{"glGetUniformfv",h_glGetUniformfv},
 {0,0}
};

/* Handle a gl* call if known: pulls args, forwards, sets r0. Returns 1 if handled. */
#if !defined(ABSHIM_RELEASE) || defined(ABSHIM_PERF)
/* Time spent INSIDE the GL bridge, i.e. in the real driver plus our marshalling - everything that
 * is NOT emulating ARM32. Measuring the emulation directly does not work: after boot the guest's
 * whole render loop runs inside ONE long-lived uc_emu_start, and the bridges are called from hooks
 * during it, so a timer around uc_emu_start reports the entire run. This is the complement.
 *
 * This comment used to end "under SwiftShader it is dominated by software rasterisation". Measured,
 * that was wrong twice over: forwarded GL is ~0.8% of frame time, and the rasterisation it was
 * meant to account for happens OUTSIDE this timer anyway — in eglSwapBuffers on the Java side,
 * which the split reports as OUT-shim. The useful separation this timer provides is GL-bridge cost
 * from the OTHER native bridges (libc et al, timed at stub_cb), which turn out to be ~20x larger. */
/* __thread for the same reason as g_stub_ns in dispatch.c: this is reported as a fraction of a
 * per-thread in-shim total, and it must be a SUBSET of the per-thread bridge total for the
 * stub >= GL invariant in the [perf] line to mean anything. As a global it was compared against
 * one thread's time while summing all of them. In practice GL is called only from the render
 * thread so the published ~0.9% is unaffected, but the comparison was not a sound one. */
static __thread uint64_t g_gl_ns = 0, g_gl_n = 0;
uint64_t gl_bridge_ns(void){ return g_gl_ns; }
uint64_t gl_bridge_calls(void){ return g_gl_n; }
static uint64_t gl_now_ns(void){
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif
int gl_try(cpu_t *c, const char *name, mcur *cur){
    (void)h_1u; (void)h_glBind2;
    for(int i=0; GLT[i].n; i++) if(!strcmp(GLT[i].n,name)){
#if !defined(ABSHIM_RELEASE) || defined(ABSHIM_PERF)
        uint64_t _g0 = gl_now_ns();
#endif
        uint64_t r = GLT[i].h(c, cur);
#if !defined(ABSHIM_RELEASE) || defined(ABSHIM_PERF)
        g_gl_ns += gl_now_ns() - _g0; g_gl_n++;
#endif
        uint32_t lo=(uint32_t)r; uc_reg_write(c->uc, UC_ARM_REG_R0, &lo);
        return 1;
    }
    /* unknown gl* : swallow (scalar no-op) so an unhandled call can't crash the driver */
    if(name[0]=='g'&&name[1]=='l'){ uint32_t z=0; uc_reg_write(c->uc,UC_ARM_REG_R0,&z); return 1; }
    return 0;
}
