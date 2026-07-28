/* bridge_asset.c — AAssetManager/AAsset forwarding to the real libandroid
 * (Audit 07). dlsym-based with void* typedefs (compiles host + device). The
 * engine reads whole assets via getBuffer (no read/seek). getBuffer returns a
 * HOST mmap pointer -> copied into a guest buffer (Unicorn can't alias host
 * memory); getLength64 returns 64-bit in r0:r1; AAssetManager and AAsset handles
 * cross as handle-table tokens. */
#include "cpu.h"
#include "marshal.h"
#include "galloc.h"
#include "jni_passthrough.h"
#include <dlfcn.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

typedef void*   (*fromJava_t)(void*, void*);
typedef void*   (*open_t)(void*, const char*, int);
typedef const void* (*getBuffer_t)(void*);
typedef int64_t (*getLength64_t)(void*);
typedef void    (*close_t)(void*);

#define WA marshal_pull_word(&c->mem, cur)
static void set_r0(cpu_t*c,uint32_t v){ uc_reg_write(c->uc,UC_ARM_REG_R0,&v); }
static void set_r1(cpu_t*c,uint32_t v){ uc_reg_write(c->uc,UC_ARM_REG_R1,&v); }
static void rd_str(cpu_t*c,uint32_t p,char*o,int max){ int i=0; if(!p){o[0]=0;return;} for(;i<max-1;i++){uint8_t ch=0;if(uc_mem_read(c->uc,p+i,&ch,1)!=UC_ERR_OK||!ch)break;o[i]=(char)ch;} o[i]=0; }
static void *asym(const char *n){ void *p=dlsym(RTLD_DEFAULT,n); if(!p){ static void*h=0; if(!h)h=dlopen("libandroid.so",RTLD_NOW|RTLD_GLOBAL); if(h)p=dlsym(h,n);} return p; }
#define RES(t,v,nm) static t v=0; if(!v)v=(t)asym(nm)

/* AAsset -> guest-buffer side table. getBuffer copies the (host mmap) asset bytes into a
 * guest buffer; we cache it per open AAsset so (a) repeat getBuffer returns the SAME guest
 * pointer (AAsset_getBuffer is idempotent) and (b) AAsset_close frees it. Without this every
 * asset load would leak the guest heap to OOM — and freeing at close also faithfully mirrors
 * the device, where close unmaps the asset and the buffer becomes invalid. Single guest under
 * the BEL, so a plain array is fine. */
#define ASSET_BUF_MAX 1024
static struct { uint32_t tok, gbuf; } g_abuf[ASSET_BUF_MAX]; static int g_nabuf;
static uint32_t abuf_find(uint32_t tok){ for(int i=0;i<g_nabuf;i++) if(g_abuf[i].tok==tok) return g_abuf[i].gbuf; return 0; }
static void     abuf_put (uint32_t tok, uint32_t gbuf){ if(g_nabuf<ASSET_BUF_MAX){ g_abuf[g_nabuf].tok=tok; g_abuf[g_nabuf].gbuf=gbuf; g_nabuf++; } }
static uint32_t abuf_take(uint32_t tok){ for(int i=0;i<g_nabuf;i++) if(g_abuf[i].tok==tok){ uint32_t g=g_abuf[i].gbuf; g_abuf[i]=g_abuf[--g_nabuf]; return g; } return 0; }

static char g_last_asset[300]={0};   /* DIAG: last AAssetManager_open path, for getBuffer correlation */
int asset_try(cpu_t *c, jni_state *J, const char *name, mcur *cur){
    if(!strcmp(name,"AAssetManager_fromJava")){
        (void)WA; uint32_t mgrobj=WA;                       /* env, jobject */
        RES(fromJava_t,fj,"AAssetManager_fromJava");
        void*mgr = fj? fj(J->real_env, ht_resolve(J->ht,mgrobj)) : 0;
        set_r0(c, mgr? ht_new_ref(J->ht,HK_GLOBAL,mgr) : 0); return 1;
    }
    if(!strcmp(name,"AAssetManager_open")){
        uint32_t mt=WA,pp=WA,mode=WA; char path[600]; rd_str(c,pp,path,sizeof path);
        /* (Audio is silenced by the nativeMixData no-op, but the Lua SoundManager ERRORS (lua_longjmp) if the
         * audio open FAILS, so we must let audio open normally; the 256MB heap absorbs the decoded PCM.) */
        RES(open_t,op,"AAssetManager_open"); void*mgr=ht_resolve(J->ht,mt);
        void*as=(op&&mgr)? op(mgr,path,(int)mode):0;
        snprintf(g_last_asset,sizeof g_last_asset,"%s",path);   /* DIAG: for getBuffer correlation */
        { static int (*rl)(int,const char*,const char*,...)=0; static int t=0;   /* DIAG: log asset opens to logcat */
          if(!t){t=1; rl=(int(*)(int,const char*,const char*,...))asym("__android_log_print");}
          if(rl) rl(as?4:6,"abshim","[AAsset_open] '%s' -> %s",path,as?"ok":"FAIL(->IOException)"); }
        set_r0(c, as? ht_new_ref(J->ht,HK_GLOBAL,as):0); return 1;
    }
    if(!strcmp(name,"AAsset_getBuffer")){
        uint32_t at=WA; void*as=ht_resolve(J->ht,at);
        uint32_t cached=abuf_find(at); if(cached){ set_r0(c,cached); return 1; }   /* idempotent: reuse the copy */
        RES(getBuffer_t,gb,"AAsset_getBuffer"); RES(getLength64_t,gl,"AAsset_getLength64");
        const void*buf=(gb&&as)?gb(as):0; int64_t len=(gl&&as)?gl(as):0;
        /* galloc len+1 and NUL-terminate: the engine treats text assets (config.json/assets.list/
         * *.lua) as C strings and over-reads past `len` if there's no terminator; the trailing
         * byte lands in uninitialised guest heap (benign zeros in a fresh process, GARBAGE in a
         * long-running one) -> non-deterministic runaway parse. The NUL bounds the parser. */
        uint32_t g=0; if(buf&&len>0){ g=galloc_malloc(c->asset_heap,(uint32_t)len+1u); if(g){ uc_mem_write(c->uc,g,buf,(uint32_t)len); uint8_t z=0; uc_mem_write(c->uc,g+(uint32_t)len,&z,1); abuf_put(at,g); } }
        { static int (*rl)(int,const char*,const char*,...)=0; static int t=0;   /* DIAG: getBuffer length -> pin empty (len=0) assets */
          if(!t){t=1; rl=(int(*)(int,const char*,const char*,...))asym("__android_log_print");}
          if(rl) rl((len>0&&g)?4:6,"abshim","[AAsset_getBuffer] '%s' len=%lld buf=%s g=0x%x%s",g_last_asset,(long long)len,buf?"ok":"NULL",g,(len>0&&!g)?" <<< GALLOC-FAIL(null data ptr!)":((len>0)?"":" <<< EMPTY")); }
        set_r0(c,g); return 1;
    }
    if(!strcmp(name,"AAsset_getLength64")||!strcmp(name,"AAsset_getLength")){
        uint32_t at=WA; void*as=ht_resolve(J->ht,at);
        RES(getLength64_t,gl,"AAsset_getLength64"); int64_t len=(gl&&as)?gl(as):0;
        set_r0(c,(uint32_t)len); set_r1(c,(uint32_t)((uint64_t)len>>32)); return 1;
    }
    if(!strcmp(name,"AAsset_close")){
        uint32_t at=WA; void*as=ht_resolve(J->ht,at); RES(close_t,cl,"AAsset_close"); if(cl&&as)cl(as);
        uint32_t g=abuf_take(at); if(g) galloc_free(c->asset_heap,g);   /* free the copied guest buffer (mirrors device unmap) */
        ht_delete_ref(J->ht,at); set_r0(c,0); return 1;
    }
    if(!strncmp(name,"AAsset",6)){ set_r0(c,0); return 1; }  /* read/seek/etc: unused, swallow */
    return 0;
}
