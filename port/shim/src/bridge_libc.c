/* bridge_libc.c — runtime libc/libm bridges (Audit 06 surface; see RUNTIME_BRIDGES.md).
 *
 * These are the ~180 UND FUNC imports the engine calls at RUNTIME (physics/render/
 * parse/time) that the init-time dispatch BR[] table does not cover. Routed from
 * dispatch's stub_cb via libc_try() (like gl_try/asset_try), BEFORE the UNIMPL fall-
 * through, so a missing math/net function can never silently become 0.
 *
 * Return convention mirrors dispatch: a handler returns its value; libc_try writes r0
 * (ret>=1) and r1 (ret==2, i64/f64). Soft-float ABI: a double occupies two core regs
 * (r0:r1 then, 8-aligned, r2:r3) pulled via DW; a float is one core reg pulled via W.
 *
 * All handlers run under the GEL (one guest thread at a time) so static scratch is safe.
 * NETWORK + dl are a HARD-FAIL facade — never a real socket (de-phone-home + the public-
 * IPv4 box rule): they return the libc failure value, not 0. */
#include "bridge.h"
#include "regions.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <wchar.h>
#include <wctype.h>
#include <time.h>
#include <fnmatch.h>
#include <dlfcn.h>
#include "sched.h"    /* g_sched + sched_errno_addr: set the CURRENT guest thread's errno */

/* ---- arg pulls + soft-float packing ---- */
static uint32_t W (cpu_t*c, mcur*u){ return marshal_pull_word (&c->mem,u); }
static uint64_t DW(cpu_t*c, mcur*u){ return marshal_pull_dword(&c->mem,u); }
static double   d_in (uint64_t b){ double x; memcpy(&x,&b,8); return x; }
static uint64_t d_out(double  x){ uint64_t b; memcpy(&b,&x,8); return b; }
static float    f_in (uint32_t b){ float  x; memcpy(&x,&b,4); return x; }
static uint32_t f_out(float   x){ uint32_t b; memcpy(&b,&x,4); return b; }
static int gstr(cpu_t*c, uint32_t p, char*o, int max){
    int i=0; if(!p){ if(max)o[0]=0; return 0; }
    for(;i<max-1;i++){ uint8_t ch; c->mem.read(&c->mem,&ch,p+(uint32_t)i,1); if(!ch)break; o[i]=(char)ch; }
    o[i]=0; return i;
}

/* ---- libm: host passthrough (double / float / mixed) ---- */
#define D1(nm,fn) static uint64_t nm(cpu_t*c,mcur*u){ return d_out(fn(d_in(DW(c,u)))); }
#define D2(nm,fn) static uint64_t nm(cpu_t*c,mcur*u){ double a=d_in(DW(c,u)),b=d_in(DW(c,u)); return d_out(fn(a,b)); }
#define F1(nm,fn) static uint64_t nm(cpu_t*c,mcur*u){ return f_out(fn(f_in(W(c,u)))); }
#define F2(nm,fn) static uint64_t nm(cpu_t*c,mcur*u){ float a=f_in(W(c,u)),b=f_in(W(c,u)); return f_out(fn(a,b)); }
D1(l_sin,sin)   D1(l_cos,cos)   D1(l_tan,tan)   D1(l_asin,asin) D1(l_acos,acos) D1(l_atan,atan)
D1(l_sinh,sinh) D1(l_cosh,cosh) D1(l_tanh,tanh) D1(l_exp,exp)   D1(l_log,log)   D1(l_log10,log10)
D1(l_sqrt,sqrt) D1(l_rint,rint)
D2(l_atan2,atan2) D2(l_pow,pow) D2(l_fmod,fmod)
F1(l_sinf,sinf) F1(l_cosf,cosf) F1(l_tanf,tanf) F1(l_acosf,acosf) F1(l_asinf,asinf)
F1(l_ceilf,ceilf) F1(l_floorf,floorf) F1(l_sqrtf,sqrtf)
F2(l_atan2f,atan2f) F2(l_fmodf,fmodf)
static uint64_t l_ldexp(cpu_t*c,mcur*u){ double x=d_in(DW(c,u)); int e=(int)W(c,u); return d_out(ldexp(x,e)); }
static uint64_t l_frexp(cpu_t*c,mcur*u){ double x=d_in(DW(c,u)); uint32_t ep=W(c,u); int e=0; double m=frexp(x,&e); if(ep) gm_wr32(&c->mem,ep,(uint32_t)e); return d_out(m); }
static uint64_t l_modf (cpu_t*c,mcur*u){ double x=d_in(DW(c,u)); uint32_t ip=W(c,u); double i=0; double f=modf(x,&i); if(ip){ uint64_t b=d_out(i); gm_wr32(&c->mem,ip,(uint32_t)b); gm_wr32(&c->mem,ip+4,(uint32_t)(b>>32)); } return d_out(f); }
static uint64_t l_modff(cpu_t*c,mcur*u){ float x=f_in(W(c,u)); uint32_t ip=W(c,u); float i=0; float f=modff(x,&i); if(ip) gm_wr32(&c->mem,ip,f_out(i)); return f_out(f); }
static uint64_t l_difftime(cpu_t*c,mcur*u){ uint32_t a=W(c,u),b=W(c,u); return d_out(difftime((time_t)a,(time_t)b)); }
static uint64_t l_lrint (cpu_t*c,mcur*u){ return (uint32_t)(long)lrint (d_in(DW(c,u))); }
static uint64_t l_lrintf(cpu_t*c,mcur*u){ return (uint32_t)(long)lrintf(f_in(W(c,u))); }

/* ---- ctype (host) ---- */
#define CT(nm,fn) static uint64_t nm(cpu_t*c,mcur*u){ (void)c; return (uint32_t)fn((int)W(c,u)); }
CT(l_isdigit,isdigit) CT(l_isspace,isspace) CT(l_isupper,isupper) CT(l_isxdigit,isxdigit)
CT(l_tolower,tolower) CT(l_towlower,towlower) CT(l_towupper,towupper)
static uint64_t l_iswctype(cpu_t*c,mcur*u){ (void)c; wint_t w=(wint_t)W(c,u); wctype_t t=(wctype_t)W(c,u); return (uint32_t)iswctype(w,t); }

/* ---- string / conversion (host, over copied guest strings) ---- */
static uint64_t l_atoi(cpu_t*c,mcur*u){ char b[160]; gstr(c,W(c,u),b,sizeof b); return (uint32_t)atoi(b); }
static uint64_t l_atol(cpu_t*c,mcur*u){ char b[160]; gstr(c,W(c,u),b,sizeof b); return (uint32_t)(long)atol(b); }
static uint64_t l_strtol(cpu_t*c,mcur*u){ uint32_t np=W(c,u),ep=W(c,u); int base=(int)W(c,u); char b[320]; gstr(c,np,b,sizeof b); char*e=b; long v=strtol(b,&e,base); if(ep) gm_wr32(&c->mem,ep,np+(uint32_t)(e-b)); return (uint32_t)v; }
static uint64_t l_strtoul(cpu_t*c,mcur*u){ uint32_t np=W(c,u),ep=W(c,u); int base=(int)W(c,u); char b[320]; gstr(c,np,b,sizeof b); char*e=b; unsigned long v=strtoul(b,&e,base); if(ep) gm_wr32(&c->mem,ep,np+(uint32_t)(e-b)); return (uint32_t)v; }
static uint64_t l_strtoll(cpu_t*c,mcur*u){ uint32_t np=W(c,u),ep=W(c,u); int base=(int)W(c,u); char b[320]; gstr(c,np,b,sizeof b); char*e=b; long long v=strtoll(b,&e,base); if(ep) gm_wr32(&c->mem,ep,np+(uint32_t)(e-b)); return (uint64_t)v; }
static uint64_t l_strtoull(cpu_t*c,mcur*u){ uint32_t np=W(c,u),ep=W(c,u); int base=(int)W(c,u); char b[320]; gstr(c,np,b,sizeof b); char*e=b; unsigned long long v=strtoull(b,&e,base); if(ep) gm_wr32(&c->mem,ep,np+(uint32_t)(e-b)); return (uint64_t)v; }
static uint64_t l_strtod(cpu_t*c,mcur*u){ uint32_t np=W(c,u),ep=W(c,u); char b[320]; gstr(c,np,b,sizeof b); char*e=b; double v=strtod(b,&e); if(ep) gm_wr32(&c->mem,ep,np+(uint32_t)(e-b)); return d_out(v); }
static uint64_t l_strcasecmp (cpu_t*c,mcur*u){ char a[1024],b[1024]; gstr(c,W(c,u),a,sizeof a); gstr(c,W(c,u),b,sizeof b); return (uint32_t)strcasecmp(a,b); }
static uint64_t l_strncasecmp(cpu_t*c,mcur*u){ uint32_t pa=W(c,u),pb=W(c,u),n=W(c,u); char a[1024],b[1024]; gstr(c,pa,a,sizeof a); gstr(c,pb,b,sizeof b); return (uint32_t)strncasecmp(a,b,n); }
static uint64_t l_strcspn(cpu_t*c,mcur*u){ static char a[4096]; char s[256]; uint32_t pa=W(c,u),ps=W(c,u); gstr(c,pa,a,sizeof a); gstr(c,ps,s,sizeof s); return (uint32_t)strcspn(a,s); }
static uint64_t l_strspn (cpu_t*c,mcur*u){ static char a[4096]; char s[256]; uint32_t pa=W(c,u),ps=W(c,u); gstr(c,pa,a,sizeof a); gstr(c,ps,s,sizeof s); return (uint32_t)strspn(a,s); }
static uint64_t l_strpbrk(cpu_t*c,mcur*u){ static char a[4096]; char s[256]; uint32_t pa=W(c,u),ps=W(c,u); gstr(c,pa,a,sizeof a); gstr(c,ps,s,sizeof s); char*p=strpbrk(a,s); return p? pa+(uint32_t)(p-a):0; }
static uint64_t l_strncat(cpu_t*c,mcur*u){ uint32_t d=W(c,u),s=W(c,u),n=W(c,u); char db[4096],sb[4096]; int dl=gstr(c,d,db,sizeof db); int sl=gstr(c,s,sb,sizeof sb); uint32_t k=((uint32_t)sl<n)?(uint32_t)sl:n; for(uint32_t i=0;i<k;i++){ uint8_t ch=(uint8_t)sb[i]; c->mem.write(&c->mem,d+(uint32_t)dl+i,&ch,1);} uint8_t z=0; c->mem.write(&c->mem,d+(uint32_t)dl+k,&z,1); return d; }
static uint64_t l_memmem(cpu_t*c,mcur*u){ static unsigned char H[65536],N[4096]; uint32_t hp=W(c,u),hl=W(c,u),np=W(c,u),nl=W(c,u); if(hl>sizeof H||nl>sizeof N) return 0; c->mem.read(&c->mem,H,hp,hl); c->mem.read(&c->mem,N,np,nl); void*p=memmem(H,hl,N,nl); return p? hp+(uint32_t)((unsigned char*)p-H):0; }
static uint64_t l_memrchr(cpu_t*c,mcur*u){ uint32_t p=W(c,u),ch=W(c,u),n=W(c,u); for(uint32_t i=n;i>0;i--){ uint8_t b; c->mem.read(&c->mem,&b,p+i-1,1); if(b==(ch&0xff)) return p+i-1; } return 0; }
static uint64_t l_basename(cpu_t*c,mcur*u){ uint32_t p=W(c,u); if(!p) return 0; static char b[4096]; int L=gstr(c,p,b,sizeof b); char*s=strrchr(b,'/'); return s? p+(uint32_t)((s+1)-b) : p; (void)L; }

/* ---- wide char (wchar_t = 4 bytes on arm32) ---- */
static uint64_t l_wcslen (cpu_t*c,mcur*u){ uint32_t p=W(c,u),n=0; while(gm_rd32(&c->mem,p+n*4)) n++; return n; }
static uint64_t l_wmemcpy(cpu_t*c,mcur*u){ uint32_t d=W(c,u),s=W(c,u),n=W(c,u); for(uint32_t i=0;i<n;i++) gm_wr32(&c->mem,d+i*4,gm_rd32(&c->mem,s+i*4)); return d; }
static uint64_t l_wmemmove(cpu_t*c,mcur*u){ uint32_t d=W(c,u),s=W(c,u),n=W(c,u); if(d<=s||d>=s+n*4){ for(uint32_t i=0;i<n;i++) gm_wr32(&c->mem,d+i*4,gm_rd32(&c->mem,s+i*4)); } else { for(uint32_t i=n;i>0;i--) gm_wr32(&c->mem,d+(i-1)*4,gm_rd32(&c->mem,s+(i-1)*4)); } return d; }
static uint64_t l_wmemset(cpu_t*c,mcur*u){ uint32_t d=W(c,u),wc=W(c,u),n=W(c,u); for(uint32_t i=0;i<n;i++) gm_wr32(&c->mem,d+i*4,wc); return d; }
static uint64_t l_wmemchr(cpu_t*c,mcur*u){ uint32_t p=W(c,u),wc=W(c,u),n=W(c,u); for(uint32_t i=0;i<n;i++) if(gm_rd32(&c->mem,p+i*4)==wc) return p+i*4; return 0; }
static uint64_t l_wmemcmp(cpu_t*c,mcur*u){ uint32_t a=W(c,u),b=W(c,u),n=W(c,u); for(uint32_t i=0;i<n;i++){ uint32_t x=gm_rd32(&c->mem,a+i*4),y=gm_rd32(&c->mem,b+i*4); if(x!=y) return (x<y)?(uint32_t)-1:1; } return 0; }

/* ---- rand48 (shared 48-bit LCG; POSIX drand48 state is global, so sharing is correct) ---- */
static uint64_t g_r48=0x1234ABCD330Eull;
static uint64_t l_srand48(cpu_t*c,mcur*u){ (void)c; uint32_t s=W(c,u); g_r48=((uint64_t)s<<16)|0x330Eull; return 0; }
static uint64_t l_lrand48(cpu_t*c,mcur*u){ (void)c;(void)u; g_r48=(0x5DEECE66Dull*g_r48+0xBull)&0xFFFFFFFFFFFFull; return (uint32_t)((g_r48>>17)&0x7FFFFFFFu); }

/* ---- sysinfo (safe defaults) ---- */
static uint64_t l_getpid (cpu_t*c,mcur*u){ (void)c;(void)u; return 12345; }
static uint64_t l_geteuid(cpu_t*c,mcur*u){ (void)c;(void)u; return 10123; }   /* an app uid */
static uint64_t l_getenv (cpu_t*c,mcur*u){ (void)c;(void)u; return 0; }        /* NULL: no environment */
static uint64_t l_getpwuid(cpu_t*c,mcur*u){ (void)c;(void)u; return 0; }       /* NULL */
static uint64_t l_sysprop(cpu_t*c,mcur*u){ uint32_t nm=W(c,u),val=W(c,u); (void)nm; if(val){ uint8_t z=0; c->mem.write(&c->mem,val,&z,1); } return 0; }
static uint64_t l_uname (cpu_t*c,mcur*u){ uint32_t b=W(c,u); if(b){ for(uint32_t i=0;i<390;i+=1){ uint8_t z=0; c->mem.write(&c->mem,b+i,&z,1);} const char*s="Linux"; for(int i=0;s[i];i++){ uint8_t ch=(uint8_t)s[i]; c->mem.write(&c->mem,b+(uint32_t)i,&ch,1);} } return 0; }

/* ---- NETWORK: hard-fail facade (never a real socket; de-phone-home + public-IPv4 box rule) ----
 * CRITICAL: return the libc failure value AND set the current guest thread's errno to a PERMANENT
 * failure (ENETUNREACH/ECONNREFUSED/ECONNRESET). Returning -1 with a STALE errno let libcurl read
 * EINPROGRESS/EAGAIN and spin its connect/poll retry loop forever — observed: the engine's init-time
 * phone-home (libcurl, cacert.pem) hung nativeInit for minutes right after the CA bundle loaded. A
 * hard errno makes libcurl abort the transfer immediately (CURLE_COULDNT_CONNECT), exactly as on a
 * real offline device; the engine then continues down its normal io::IOException path. */
static void nseterr(cpu_t*c, uint32_t e){
    if(g_sched && sched_current(g_sched)){ uint32_t a=sched_errno_addr(g_sched); if(a) c->mem.write(&c->mem,a,&e,4); }
}
static void netlog(const char*nm){   /* one line per distinct net fn: name what the phone-home actually calls */
    static const char*seen[40]; static int ns=0; for(int i=0;i<ns;i++) if(seen[i]==nm) return; if(ns<40) seen[ns++]=nm;
    static int(*rl)(int,const char*,const char*,...)=0; static int t=0;
    if(!t){ t=1; rl=(int(*)(int,const char*,const char*,...))dlsym(RTLD_DEFAULT,"__android_log_print"); }
    if(rl) rl(4,"abshim","[net] %s -> hard-fail (de-phone-home)",nm);
}
#define NETERR(nm,ev) static uint64_t nm(cpu_t*c,mcur*u){ (void)u; nseterr(c,(ev)); netlog(#nm); return 0xffffffffu; }
NETERR(l_socket,101)  NETERR(l_connect,111) NETERR(l_bind,111)   NETERR(l_send,104) NETERR(l_sendto,104)
NETERR(l_recv,104)    NETERR(l_recvfrom,104) NETERR(l_getpeername,107) NETERR(l_getsockname,107)
NETERR(l_getsockopt,111) NETERR(l_setsockopt,111) NETERR(l_gethostname,111) NETERR(l_if_indextoname,111)
/* poll: no bridged socket ever yields a real fd (network is cut), so poll can only ever time out.
 * A phone-home/heartbeat thread polls its dead socket forever; returning 0 INSTANTLY made it
 * busy-spin and starve the cooperative GEL (observed: poll/clock_gettime/mutex hammered millions
 * of times while nativeInit+render crawled). Emulate the timeout as a scheduler-YIELDING sleep so
 * that loop parks and the render thread runs. Clear revents (nothing ready); cap the nap so an
 * infinite/long timeout still wakes periodically. r0=0 (0 fds ready) is injected on wake. */
static uint64_t l_poll(cpu_t*c,mcur*u){
    uint32_t fds=W(c,u); uint32_t nfds=W(c,u); int32_t tmo=(int32_t)W(c,u);
    netlog("l_poll");
    if(fds && nfds){ uint16_t z=0; for(uint32_t i=0;i<nfds && i<512u;i++) c->mem.write(&c->mem, fds+i*8u+6u, &z, 2); } /* pollfd.revents=0 */
    uint32_t ms=(tmo<0||tmo>200)?200u:(tmo==0?1u:(uint32_t)tmo);
    if(g_sched && sched_current(g_sched)) sched_sleep(g_sched,(uint64_t)ms*1000000ull);
    return 0;
}
static uint64_t l_getaddrinfo (cpu_t*c,mcur*u){ (void)c;(void)u; return (uint32_t)(-2); }  /* EAI_FAIL */
static uint64_t l_freeaddrinfo(cpu_t*c,mcur*u){ (void)c;(void)u; return 0; }
static uint64_t l_gai_strerror(cpu_t*c,mcur*u){ (void)c;(void)u; return 0; }
static uint64_t l_inet_addr   (cpu_t*c,mcur*u){ (void)c;(void)u; return 0xffffffffu; }       /* INADDR_NONE */
static uint64_t l_inet_pton   (cpu_t*c,mcur*u){ (void)c;(void)u; return 0; }
static uint64_t l_inet_ntop   (cpu_t*c,mcur*u){ (void)c;(void)u; return 0; }
static uint64_t l_if_nametoindex(cpu_t*c,mcur*u){ (void)c;(void)u; return 0; }
static uint64_t l_getservbyport(cpu_t*c,mcur*u){ (void)c;(void)u; return 0; }

/* ---- dl: single-lib hard-fail ---- */
static uint64_t l_dlopen (cpu_t*c,mcur*u){ (void)c;(void)u; return 0; }
static uint64_t l_dlsym  (cpu_t*c,mcur*u){ (void)c;(void)u; return 0; }
static uint64_t l_dlclose(cpu_t*c,mcur*u){ (void)c;(void)u; return 0; }
static uint64_t l_dlerror(cpu_t*c,mcur*u){ (void)c;(void)u; return 0; }

/* ---- time (real; matches the dispatch clock so timed waits agree) ---- */
static uint64_t l_clock(cpu_t*c,mcur*u){ (void)c;(void)u; return (uint32_t)clock(); }
static uint64_t l_time (cpu_t*c,mcur*u){ uint32_t p=W(c,u); time_t t=time(0); if(p) gm_wr32(&c->mem,p,(uint32_t)t); return (uint32_t)t; }
static uint64_t l_nanosleep(cpu_t*c,mcur*u){ uint32_t rq=W(c,u),rm=W(c,u); (void)rm;
    uint64_t sec=rq?gm_rd32(&c->mem,rq):0, ns=rq?gm_rd32(&c->mem,rq+4):0;
    uint64_t total=sec*1000000000ull+ns; if(total>100000000ull) total=100000000ull;  /* cap 100ms: we hold the GEL */
    struct timespec t; t.tv_sec=(time_t)(total/1000000000ull); t.tv_nsec=(long)(total%1000000000ull); nanosleep(&t,0); return 0; }

/* ---- signals: no-op (a de-networked single-purpose game needs no handlers) ---- */
static uint64_t l_sigaction  (cpu_t*c,mcur*u){ (void)c;(void)u; return 0; }
static uint64_t l_sigprocmask(cpu_t*c,mcur*u){ (void)c;(void)u; return 0; }

/* ---- more string / locale (C-locale) ---- */
static uint64_t l_strcoll(cpu_t*c,mcur*u){ char a[1024],b[1024]; gstr(c,W(c,u),a,sizeof a); gstr(c,W(c,u),b,sizeof b); return (uint32_t)strcmp(a,b); }
static uint64_t l_strxfrm(cpu_t*c,mcur*u){ uint32_t d=W(c,u),s=W(c,u),n=W(c,u); char b[4096]; int L=gstr(c,s,b,sizeof b);
    uint32_t k=((uint32_t)(L+1)<n)?(uint32_t)(L+1):n; for(uint32_t i=0;i<k;i++){ uint8_t ch=(i<(uint32_t)L)?(uint8_t)b[i]:0; c->mem.write(&c->mem,d+i,&ch,1);} return (uint32_t)L; }
static uint64_t l_fnmatch(cpu_t*c,mcur*u){ char p[1024],s[1024]; uint32_t pp=W(c,u),sp=W(c,u); int fl=(int)W(c,u); gstr(c,pp,p,sizeof p); gstr(c,sp,s,sizeof s); return (uint32_t)fnmatch(p,s,fl); }
static uint64_t l_mbrtowc(cpu_t*c,mcur*u){ uint32_t pwc=W(c,u),s=W(c,u),n=W(c,u),ps=W(c,u); (void)ps;
    if(!s) return 0;
    if(n>16) n=16;
    char buf[16]; for(uint32_t i=0;i<n;i++){ uint8_t ch; c->mem.read(&c->mem,&ch,s+i,1); buf[i]=(char)ch; }
    wchar_t wc=0; mbstate_t st; memset(&st,0,sizeof st); size_t r=mbrtowc(&wc,buf,n,&st);
    if(r==(size_t)-1||r==(size_t)-2) return 0xffffffffu;
    if(pwc) gm_wr32(&c->mem,pwc,(uint32_t)wc);
    return (uint32_t)r; }
static uint64_t l_strtok_r(cpu_t*c,mcur*u){ uint32_t str=W(c,u),delim=W(c,u),sp=W(c,u);
    char dl[64]; gstr(c,delim,dl,sizeof dl);
    uint32_t p = str ? str : (sp?gm_rd32(&c->mem,sp):0);
    for(;;){ if(!p){ if(sp)gm_wr32(&c->mem,sp,0); return 0; } uint8_t ch; c->mem.read(&c->mem,&ch,p,1);
        if(!ch){ if(sp)gm_wr32(&c->mem,sp,p); return 0; } if(!strchr(dl,ch)) break; p++; }
    uint32_t tok=p;
    for(;;){ uint8_t ch; c->mem.read(&c->mem,&ch,p,1);
        if(!ch){ if(sp)gm_wr32(&c->mem,sp,p); return tok; }
        if(strchr(dl,ch)){ uint8_t z=0; c->mem.write(&c->mem,p,&z,1); if(sp)gm_wr32(&c->mem,sp,p+1); return tok; } p++; }
}

/* ---- fs: safe no-op / not-present defaults (real fd-backed layer is a later pass) ---- */
static uint64_t l_getcwd(cpu_t*c,mcur*u){ uint32_t b=W(c,u),sz=W(c,u); if(b&&sz>=2){ uint8_t s[2]={'/',0}; c->mem.write(&c->mem,b,s,2); return b; } return 0; }
static uint64_t l_access(cpu_t*c,mcur*u){ (void)c;(void)u; return 0xffffffffu; }   /* ENOENT: engine falls back to AAsset */

/* ---- struct tm <-> guest (bionic arm32: 9 ints, then long tm_gmtoff, ptr tm_zone;
 * written field-by-field since host long/ptr are 8B but guest LP32 is 4B) ---- */
static uint32_t g_tmbuf=0;
static uint32_t tm_scratch(cpu_t*c){ if(!g_tmbuf) g_tmbuf=galloc_malloc(c->heap,64); return g_tmbuf; }
static void tm_to_guest(cpu_t*c, uint32_t g, const struct tm*t){
    gm_wr32(&c->mem,g+0,(uint32_t)t->tm_sec);  gm_wr32(&c->mem,g+4,(uint32_t)t->tm_min);
    gm_wr32(&c->mem,g+8,(uint32_t)t->tm_hour); gm_wr32(&c->mem,g+12,(uint32_t)t->tm_mday);
    gm_wr32(&c->mem,g+16,(uint32_t)t->tm_mon); gm_wr32(&c->mem,g+20,(uint32_t)t->tm_year);
    gm_wr32(&c->mem,g+24,(uint32_t)t->tm_wday);gm_wr32(&c->mem,g+28,(uint32_t)t->tm_yday);
    gm_wr32(&c->mem,g+32,(uint32_t)t->tm_isdst);gm_wr32(&c->mem,g+36,(uint32_t)t->tm_gmtoff);
    gm_wr32(&c->mem,g+40,0);
}
static void tm_from_guest(cpu_t*c, uint32_t g, struct tm*t){
    memset(t,0,sizeof *t);
    t->tm_sec=(int)gm_rd32(&c->mem,g+0);  t->tm_min=(int)gm_rd32(&c->mem,g+4);  t->tm_hour=(int)gm_rd32(&c->mem,g+8);
    t->tm_mday=(int)gm_rd32(&c->mem,g+12);t->tm_mon=(int)gm_rd32(&c->mem,g+16); t->tm_year=(int)gm_rd32(&c->mem,g+20);
    t->tm_wday=(int)gm_rd32(&c->mem,g+24);t->tm_yday=(int)gm_rd32(&c->mem,g+28);t->tm_isdst=(int)gm_rd32(&c->mem,g+32);
}
static uint64_t l_gmtime(cpu_t*c,mcur*u){ uint32_t tp=W(c,u); time_t t=tp?(time_t)gm_rd32(&c->mem,tp):0; struct tm r; gmtime_r(&t,&r); uint32_t g=tm_scratch(c); if(g)tm_to_guest(c,g,&r); return g; }
static uint64_t l_localtime(cpu_t*c,mcur*u){ uint32_t tp=W(c,u); time_t t=tp?(time_t)gm_rd32(&c->mem,tp):0; struct tm r; localtime_r(&t,&r); uint32_t g=tm_scratch(c); if(g)tm_to_guest(c,g,&r); return g; }
static uint64_t l_gmtime_r(cpu_t*c,mcur*u){ uint32_t tp=W(c,u),rp=W(c,u); time_t t=tp?(time_t)gm_rd32(&c->mem,tp):0; struct tm r; gmtime_r(&t,&r); if(rp)tm_to_guest(c,rp,&r); return rp; }
static uint64_t l_localtime_r(cpu_t*c,mcur*u){ uint32_t tp=W(c,u),rp=W(c,u); time_t t=tp?(time_t)gm_rd32(&c->mem,tp):0; struct tm r; localtime_r(&t,&r); if(rp)tm_to_guest(c,rp,&r); return rp; }
static uint64_t l_mktime(cpu_t*c,mcur*u){ uint32_t tp=W(c,u); struct tm r; tm_from_guest(c,tp,&r); return (uint32_t)mktime(&r); }
static uint64_t l_strftime(cpu_t*c,mcur*u){ uint32_t d=W(c,u),max=W(c,u),f=W(c,u),tp=W(c,u);
    char fmt[256],out[1024]; gstr(c,f,fmt,sizeof fmt); struct tm r; tm_from_guest(c,tp,&r);
    size_t cap = sizeof out < max ? sizeof out : max; size_t n = cap ? strftime(out,cap,fmt,&r) : 0;
    for(size_t i=0;i<n;i++){ uint8_t ch=(uint8_t)out[i]; c->mem.write(&c->mem,d+(uint32_t)i,&ch,1); }
    return (uint32_t)n; }

/* ---- qsort/bsearch: guest-comparator trampoline (nested cpu_run + ctx save/restore) ---- */
static void gswap(cpu_t*c, uint32_t a, uint32_t b, uint32_t size){
    uint8_t ta[256], tb[256];
    while(size){ uint32_t k=size<256?size:256; c->mem.read(&c->mem,ta,a,k); c->mem.read(&c->mem,tb,b,k);
        c->mem.write(&c->mem,a,tb,k); c->mem.write(&c->mem,b,ta,k); a+=k; b+=k; size-=k; }
}
static int cmp_guest(cpu_t*c, uint32_t cmp, uint32_t a, uint32_t b, uc_context*ctx){
    uc_context_save(c->uc, ctx);
    uc_reg_write(c->uc,UC_ARM_REG_R0,&a); uc_reg_write(c->uc,UC_ARM_REG_R1,&b);
    uint32_t lr=RG_RET; uc_reg_write(c->uc,UC_ARM_REG_LR,&lr);
    cpu_run(c, cmp, 0);
    uint32_t rr=0; uc_reg_read(c->uc,UC_ARM_REG_R0,&rr);
    uc_context_restore(c->uc, ctx);
    return (int)(int32_t)rr;
}
static uint64_t l_qsort(cpu_t*c,mcur*u){ uint32_t base=W(c,u),n=W(c,u),size=W(c,u),cmp=W(c,u);
    if(n<2 || size==0 || size>4096) return 0;
    uc_context*ctx=NULL; if(uc_context_alloc(c->uc,&ctx)) return 0;
    for(uint32_t i=1;i<n;i++)
        for(uint32_t j=i;j>0;j--){ uint32_t a=base+(j-1)*size, b=base+j*size;
            if(cmp_guest(c,cmp,a,b,ctx)>0) gswap(c,a,b,size); else break; }
    uc_context_free(ctx); return 0;
}
static uint64_t l_bsearch(cpu_t*c,mcur*u){ uint32_t key=W(c,u),base=W(c,u),n=W(c,u),size=W(c,u),cmp=W(c,u);
    if(size==0 || size>4096) return 0;
    uc_context*ctx=NULL; if(uc_context_alloc(c->uc,&ctx)) return 0;
    uint32_t lo=0,hi=n,res=0;
    while(lo<hi){ uint32_t mid=lo+(hi-lo)/2, e=base+mid*size; int r=cmp_guest(c,cmp,key,e,ctx);
        if(r==0){ res=e; break; } if(r<0) hi=mid; else lo=mid+1; }
    uc_context_free(ctx); return res;
}

typedef uint64_t (*lfn)(cpu_t*, mcur*);
typedef struct { const char*name; int ret; lfn fn; } lent;   /* ret: 1 word, 2 i64/f64 */
static const lent LT[] = {
    {"sin",2,l_sin},{"cos",2,l_cos},{"tan",2,l_tan},{"asin",2,l_asin},{"acos",2,l_acos},{"atan",2,l_atan},
    {"sinh",2,l_sinh},{"cosh",2,l_cosh},{"tanh",2,l_tanh},{"exp",2,l_exp},{"log",2,l_log},{"log10",2,l_log10},
    {"sqrt",2,l_sqrt},{"rint",2,l_rint},{"atan2",2,l_atan2},{"pow",2,l_pow},{"fmod",2,l_fmod},
    {"ldexp",2,l_ldexp},{"frexp",2,l_frexp},{"modf",2,l_modf},{"difftime",2,l_difftime},
    {"sinf",1,l_sinf},{"cosf",1,l_cosf},{"tanf",1,l_tanf},{"acosf",1,l_acosf},{"asinf",1,l_asinf},
    {"ceilf",1,l_ceilf},{"floorf",1,l_floorf},{"sqrtf",1,l_sqrtf},{"atan2f",1,l_atan2f},{"fmodf",1,l_fmodf},
    {"modff",1,l_modff},{"lrint",1,l_lrint},{"lrintf",1,l_lrintf},
    {"isdigit",1,l_isdigit},{"isspace",1,l_isspace},{"isupper",1,l_isupper},{"isxdigit",1,l_isxdigit},
    {"tolower",1,l_tolower},{"towlower",1,l_towlower},{"towupper",1,l_towupper},{"iswctype",1,l_iswctype},
    {"atoi",1,l_atoi},{"atol",1,l_atol},{"strtol",1,l_strtol},{"strtoul",1,l_strtoul},
    {"strtoll",2,l_strtoll},{"strtoull",2,l_strtoull},{"strtod",2,l_strtod},
    {"strcasecmp",1,l_strcasecmp},{"strncasecmp",1,l_strncasecmp},
    {"strcspn",1,l_strcspn},{"strspn",1,l_strspn},{"strpbrk",1,l_strpbrk},{"strncat",1,l_strncat},
    {"memmem",1,l_memmem},{"memrchr",1,l_memrchr},{"basename",1,l_basename},
    {"wcslen",1,l_wcslen},{"wmemcpy",1,l_wmemcpy},{"wmemmove",1,l_wmemmove},{"wmemset",1,l_wmemset},
    {"wmemchr",1,l_wmemchr},{"wmemcmp",1,l_wmemcmp},
    {"lrand48",1,l_lrand48},{"srand48",1,l_srand48},
    {"getpid",1,l_getpid},{"geteuid",1,l_geteuid},{"getenv",1,l_getenv},{"getpwuid",1,l_getpwuid},
    {"__system_property_get",1,l_sysprop},{"uname",1,l_uname},
    {"socket",1,l_socket},{"connect",1,l_connect},{"bind",1,l_bind},{"send",1,l_send},{"sendto",1,l_sendto},
    {"recv",1,l_recv},{"recvfrom",1,l_recvfrom},{"getpeername",1,l_getpeername},{"getsockname",1,l_getsockname},
    {"getsockopt",1,l_getsockopt},{"setsockopt",1,l_setsockopt},{"gethostname",1,l_gethostname},{"poll",1,l_poll},
    {"getaddrinfo",1,l_getaddrinfo},{"freeaddrinfo",1,l_freeaddrinfo},{"gai_strerror",1,l_gai_strerror},
    {"inet_addr",1,l_inet_addr},{"inet_pton",1,l_inet_pton},{"inet_ntop",1,l_inet_ntop},
    {"if_indextoname",1,l_if_indextoname},{"if_nametoindex",1,l_if_nametoindex},{"getservbyport",1,l_getservbyport},
    {"dlopen",1,l_dlopen},{"dlsym",1,l_dlsym},{"dlclose",1,l_dlclose},{"dlerror",1,l_dlerror},
    {"clock",1,l_clock},{"time",1,l_time},{"nanosleep",1,l_nanosleep},
    {"sigaction",1,l_sigaction},{"sigprocmask",1,l_sigprocmask},
    {"strcoll",1,l_strcoll},{"strxfrm",1,l_strxfrm},{"fnmatch",1,l_fnmatch},{"mbrtowc",1,l_mbrtowc},{"strtok_r",1,l_strtok_r},
    {"getcwd",1,l_getcwd},{"access",1,l_access},
    {"gmtime",1,l_gmtime},{"localtime",1,l_localtime},{"gmtime_r",1,l_gmtime_r},{"localtime_r",1,l_localtime_r},
    {"mktime",1,l_mktime},{"strftime",1,l_strftime},{"qsort",1,l_qsort},{"bsearch",1,l_bsearch},
};

int libc_try(cpu_t *c, const char *name, mcur *cur){
    for(size_t i=0;i<sizeof LT/sizeof LT[0];i++){
        if(!strcmp(LT[i].name,name)){
            uint64_t rv=LT[i].fn(c,cur);
            if(LT[i].ret>=1){ uint32_t lo=(uint32_t)rv; uc_reg_write(c->uc,UC_ARM_REG_R0,&lo); }
            if(LT[i].ret==2){ uint32_t hi=(uint32_t)(rv>>32); uc_reg_write(c->uc,UC_ARM_REG_R1,&hi); }
            return 1;
        }
    }
    return 0;
}
