/* bridge_file.c — stdio FILE* / raw fd / mmap / filesystem runtime bridges
 * (Audit 06 surface; see RUNTIME_BRIDGES.md). Routed from dispatch's stub_cb via
 * file_try() (after libc_try, before UNIMPL), mirroring gl_try/libc_try.
 *
 * FILE model (Audit 06): a guest FILE* is a galloc'd bionic __sFILE struct in guest
 * memory with the fd (`_file`, a short) at offset 0x0e; a host FILE* is mapped to it in
 * a side table. On-device the shim runs inside the real app process, so the real paths
 * the guest passes are accessible; on a host unit-test an fopen may just fail -> NULL.
 * Everything runs under the GEL (one guest thread at a time) so static tables are safe.
 * There is NO network here -- nothing in this file opens a socket. */
#include "bridge.h"
#include "format.h"
#include "regions.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <utime.h>

static uint32_t W (cpu_t*c, mcur*u){ return marshal_pull_word(&c->mem,u); }
static void gwr(cpu_t*c, uint32_t a, const void*p, uint32_t n){ if(n) c->mem.write(&c->mem,a,p,n); }
static void grd(cpu_t*c, uint32_t a, void*p, uint32_t n){ if(n) c->mem.read(&c->mem,p,a,n); }
static int  gstr(cpu_t*c, uint32_t p, char*o, int max){
    int i=0; if(!p){ if(max)o[0]=0; return 0; }
    for(;i<max-1;i++){ uint8_t ch; c->mem.read(&c->mem,&ch,p+(uint32_t)i,1); if(!ch)break; o[i]=(char)ch; }
    o[i]=0; return i;
}

/* ---- guest FILE* <-> host FILE* side table + bionic __sFILE struct ---- */
#define SFILE_SZ  88
#define OFF_FILE  0x0e
static struct { uint32_t gf; FILE* hf; } g_ft[64]; static int g_nft;
static FILE* hf_of(uint32_t gf){ for(int i=0;i<g_nft;i++) if(g_ft[i].gf==gf) return g_ft[i].hf; return NULL; }
static void  ft_del(uint32_t gf){ for(int i=0;i<g_nft;i++) if(g_ft[i].gf==gf){ g_ft[i]=g_ft[--g_nft]; return; } }
static uint32_t mk_gfile(cpu_t*c, FILE* hf){
    uint32_t g=galloc_malloc(c->heap, SFILE_SZ); if(!g){ fclose(hf); return 0; }
    uint8_t z[SFILE_SZ]; memset(z,0,sizeof z); gwr(c,g,z,SFILE_SZ);
    uint16_t fd=(uint16_t)fileno(hf); gwr(c,g+OFF_FILE,&fd,2);
    if(g_nft<64){ g_ft[g_nft].gf=g; g_ft[g_nft].hf=hf; g_nft++; } else { fclose(hf); galloc_free(c->heap,g); return 0; }
    return g;
}

/* ---- stdio FILE* ---- */
static int f_trace(void){ static int on=-1; if(on<0) on=getenv("ABSHIM_FILE_TRACE")?1:0; return on; }
#include <dlfcn.h>
#include <stdarg.h>
static void flog(const char*fmt,...){ static int(*rl)(int,const char*,const char*,...)=0; static int t=0; /* DIAG: file opens -> logcat */
    if(!t){ t=1; rl=(int(*)(int,const char*,const char*,...))dlsym(RTLD_DEFAULT,"__android_log_print"); }
    char b[700]; va_list ap; va_start(ap,fmt); vsnprintf(b,sizeof b,fmt,ap); va_end(ap); if(rl) rl(4,"abshim","%s",b); }
static uint64_t f_fopen(cpu_t*c,mcur*u){ uint32_t p=W(c,u),m=W(c,u); char path[1024],mode[16]; gstr(c,p,path,sizeof path); gstr(c,m,mode,sizeof mode);
    if(getenv("ABSHIM_DET_URANDOM") && (!strcmp(path,"/dev/urandom")||!strcmp(path,"/dev/random"))) strcpy(path,"/dev/zero"); /* DIAG: deterministic randomness */
    FILE*h=fopen(path,mode);
    flog("[fopen] '%s' (%s) -> %s",path,mode,h?"ok":"FAIL(->IOException)");
    return h?mk_gfile(c,h):0; }
static uint64_t f_freopen(cpu_t*c,mcur*u){ uint32_t p=W(c,u),m=W(c,u),gf=W(c,u); char path[1024],mode[16]; gstr(c,p,path,sizeof path); gstr(c,m,mode,sizeof mode);
    FILE*old=hf_of(gf); FILE*h=freopen(path,mode,old?old:stdin); if(!h) return 0;
    for(int i=0;i<g_nft;i++) if(g_ft[i].gf==gf){ g_ft[i].hf=h; uint16_t fd=(uint16_t)fileno(h); gwr(c,gf+OFF_FILE,&fd,2); return gf; }
    return mk_gfile(c,h); }
static uint64_t f_fdopen(cpu_t*c,mcur*u){ int fd=(int)W(c,u); uint32_t m=W(c,u); char mode[16]; gstr(c,m,mode,sizeof mode); FILE*h=fdopen(fd,mode); return h?mk_gfile(c,h):0; }
static uint64_t f_fclose(cpu_t*c,mcur*u){ uint32_t gf=W(c,u); FILE*h=hf_of(gf);
    if(!h) return (uint32_t)-1;   /* unknown FILE* (e.g. a stdio stream via __sF): never galloc_free a foreign address */
    int r=fclose(h); ft_del(gf); galloc_free(c->heap,gf); return (uint32_t)r; }
static uint64_t f_fread(cpu_t*c,mcur*u){ uint32_t p=W(c,u),size=W(c,u),n=W(c,u),gf=W(c,u); FILE*h=hf_of(gf); if(!h||!size) return 0;
    size_t total=(size_t)size*n, done=0; uint8_t buf[8192];
    while(total){ size_t k=total<sizeof buf?total:sizeof buf; size_t got=fread(buf,1,k,h); if(got){ gwr(c,p+(uint32_t)done,buf,(uint32_t)got); done+=got; } if(got<k) break; total-=k; }
    return (uint32_t)(done/size); }
static uint64_t f_fwrite(cpu_t*c,mcur*u){ uint32_t p=W(c,u),size=W(c,u),n=W(c,u),gf=W(c,u); FILE*h=hf_of(gf); if(!size) return 0;
    if(!h) return n;   /* unknown FILE* (stdio stream via __sF): pretend success + discard, so the engine sees no write error */
    size_t total=(size_t)size*n, done=0; uint8_t buf[8192];
    while(total){ size_t k=total<sizeof buf?total:sizeof buf; grd(c,p+(uint32_t)done,buf,(uint32_t)k); size_t put=fwrite(buf,1,k,h); done+=put; if(put<k) break; total-=k; }
    return (uint32_t)(done/size); }
static uint64_t f_fseek(cpu_t*c,mcur*u){ uint32_t gf=W(c,u); int32_t off=(int32_t)W(c,u); int wh=(int)W(c,u); FILE*h=hf_of(gf); return (uint32_t)(h?fseek(h,off,wh):-1); }
static uint64_t f_ftell(cpu_t*c,mcur*u){ uint32_t gf=W(c,u); FILE*h=hf_of(gf); return (uint32_t)(h?ftell(h):-1); }
static uint64_t f_feof(cpu_t*c,mcur*u){ uint32_t gf=W(c,u); FILE*h=hf_of(gf); return (uint32_t)(h?feof(h):1); }
static uint64_t f_ferror(cpu_t*c,mcur*u){ uint32_t gf=W(c,u); FILE*h=hf_of(gf); return (uint32_t)(h?ferror(h):0); }  /* unknown stream: no error */
static uint64_t f_fflush(cpu_t*c,mcur*u){ uint32_t gf=W(c,u); FILE*h=hf_of(gf); if(!gf) return (uint32_t)fflush(NULL); return (uint32_t)(h?fflush(h):0); }
static uint64_t f_fgets(cpu_t*c,mcur*u){ uint32_t p=W(c,u); int n=(int)W(c,u); uint32_t gf=W(c,u); FILE*h=hf_of(gf); if(!h||n<=0) return 0; char buf[4096]; if(n>(int)sizeof buf)n=sizeof buf; if(!fgets(buf,n,h)) return 0; gwr(c,p,buf,(uint32_t)strlen(buf)+1); return p; }
static uint64_t f_fputc(cpu_t*c,mcur*u){ int ch=(int)W(c,u); uint32_t gf=W(c,u); FILE*h=hf_of(gf); return (uint32_t)(h?fputc(ch,h):ch); }
static uint64_t f_fputs(cpu_t*c,mcur*u){ uint32_t s=W(c,u),gf=W(c,u); FILE*h=hf_of(gf); if(!h) return 0; char b[8192]; gstr(c,s,b,sizeof b); return (uint32_t)fputs(b,h); }
static uint64_t f_fgetc(cpu_t*c,mcur*u){ uint32_t gf=W(c,u); FILE*h=hf_of(gf); return (uint32_t)(h?fgetc(h):-1); }
static uint64_t f_putc(cpu_t*c,mcur*u){ int ch=(int)W(c,u); uint32_t gf=W(c,u); FILE*h=hf_of(gf); return (uint32_t)(h?putc(ch,h):ch); }
static uint64_t f_puts(cpu_t*c,mcur*u){ uint32_t s=W(c,u); char b[8192]; gstr(c,s,b,sizeof b); return (uint32_t)fputs(b,stdout); }
static uint64_t f_ungetc(cpu_t*c,mcur*u){ int ch=(int)W(c,u); uint32_t gf=W(c,u); FILE*h=hf_of(gf); return (uint32_t)(h?ungetc(ch,h):-1); }
static uint64_t f_setvbuf(cpu_t*c,mcur*u){ (void)c;(void)W(c,u);(void)W(c,u);(void)W(c,u);(void)W(c,u); return 0; }
static uint64_t f_remove(cpu_t*c,mcur*u){ uint32_t p=W(c,u); char path[1024]; gstr(c,p,path,sizeof path); return (uint32_t)remove(path); }
static uint64_t f_rename(cpu_t*c,mcur*u){ uint32_t a=W(c,u),b=W(c,u); char pa[1024],pb[1024]; gstr(c,a,pa,sizeof pa); gstr(c,b,pb,sizeof pb); return (uint32_t)rename(pa,pb); }
static uint64_t f_tmpfile(cpu_t*c,mcur*u){ (void)u; FILE*h=tmpfile(); return h?mk_gfile(c,h):0; }
static uint64_t f_mkstemp(cpu_t*c,mcur*u){ uint32_t p=W(c,u); char t[1024]; gstr(c,p,t,sizeof t); int fd=mkstemp(t); if(fd>=0) gwr(c,p,t,(uint32_t)strlen(t)+1); return (uint32_t)fd; }
static uint64_t f_perror(cpu_t*c,mcur*u){ (void)c;(void)W(c,u); return 0; }
static uint64_t f_fprintf(cpu_t*c,mcur*u){ uint32_t gf=W(c,u),fmt=W(c,u); FILE*h=hf_of(gf); char hb[4096]; uint32_t hl=0; uint32_t n=fmt_to_host(&c->mem,hb,sizeof hb,fmt,*u,&hl); if(h&&hl) fwrite(hb,1,hl,h); return n; }
/* v-variants walk a guest va_list */
static uint32_t g_vscratch=0;
static uint32_t vscratch(cpu_t*c){ if(!g_vscratch) g_vscratch=galloc_malloc(c->heap,8192); return g_vscratch; }
static uint64_t f_vfprintf(cpu_t*c,mcur*u){ uint32_t gf=W(c,u),fmt=W(c,u),va=W(c,u); FILE*h=hf_of(gf); uint32_t sc=vscratch(c); if(!sc) return 0; uint32_t n=fmt_to_guest_va(&c->mem,sc,8192,fmt,va); if(h&&n){ char hb[8192]; uint32_t k=n<sizeof hb?n:sizeof hb; grd(c,sc,hb,k); fwrite(hb,1,k,h); } return n; }
static uint64_t f_vprintf(cpu_t*c,mcur*u){ uint32_t fmt=W(c,u),va=W(c,u); uint32_t sc=vscratch(c); if(!sc) return 0; return fmt_to_guest_va(&c->mem,sc,8192,fmt,va); }
static uint64_t f_vsprintf(cpu_t*c,mcur*u){ uint32_t out=W(c,u),fmt=W(c,u),va=W(c,u); return fmt_to_guest_va(&c->mem,out,0x7fffffffu,fmt,va); }

/* ---- wide stdio (C-locale: 1 byte == 1 wchar for ASCII) ---- */
static uint64_t f_getwc(cpu_t*c,mcur*u){ uint32_t gf=W(c,u); FILE*h=hf_of(gf); if(!h) return 0xffffffffu; int ch=fgetc(h); return ch<0?0xffffffffu:(uint32_t)ch; }
static uint64_t f_putwc(cpu_t*c,mcur*u){ uint32_t wc=W(c,u),gf=W(c,u); FILE*h=hf_of(gf); if(!h) return 0xffffffffu; return fputc((int)(wc&0xff),h)<0?0xffffffffu:wc; }
static uint64_t f_ungetwc(cpu_t*c,mcur*u){ uint32_t wc=W(c,u),gf=W(c,u); FILE*h=hf_of(gf); if(!h) return 0xffffffffu; return ungetc((int)(wc&0xff),h)<0?0xffffffffu:wc; }
static uint64_t f_wcrtomb(cpu_t*c,mcur*u){ uint32_t s=W(c,u),wc=W(c,u),ps=W(c,u); (void)ps; if(s){ uint8_t b=(uint8_t)(wc&0xff); gwr(c,s,&b,1); } return 1; }

/* ---- raw fd (real, path/fd-backed) ---- */
static uint64_t f_open(cpu_t*c,mcur*u){ uint32_t p=W(c,u); int fl=(int)W(c,u),mode=(int)W(c,u); char path[1024]; gstr(c,p,path,sizeof path); int fd=open(path,fl,mode);
    flog("[open] '%s' (0x%x) -> %s",path,fl,fd>=0?"ok":"FAIL(->IOException)");
    return (uint32_t)fd; }
static uint64_t f_openat(cpu_t*c,mcur*u){ int d=(int)W(c,u); uint32_t p=W(c,u); int fl=(int)W(c,u),mode=(int)W(c,u); char path[1024]; gstr(c,p,path,sizeof path); return (uint32_t)openat(d,path,fl,mode); }
static uint64_t f_read(cpu_t*c,mcur*u){ int fd=(int)W(c,u); uint32_t p=W(c,u),n=W(c,u); uint8_t buf[8192]; uint32_t done=0; while(n){ uint32_t k=n<sizeof buf?n:sizeof buf; ssize_t got=read(fd,buf,k); if(got<=0)break; gwr(c,p+done,buf,(uint32_t)got); done+=(uint32_t)got; if((uint32_t)got<k)break; n-=(uint32_t)got; } return done; }
static uint64_t f_write(cpu_t*c,mcur*u){ int fd=(int)W(c,u); uint32_t p=W(c,u),n=W(c,u); uint8_t buf[8192]; uint32_t done=0; while(n){ uint32_t k=n<sizeof buf?n:sizeof buf; grd(c,p+done,buf,k); ssize_t put=write(fd,buf,k); if(put<=0)break; done+=(uint32_t)put; if((uint32_t)put<k)break; n-=(uint32_t)put; } return done; }
static uint64_t f_close(cpu_t*c,mcur*u){ (void)c; return (uint32_t)close((int)W(c,u)); }
static uint64_t f_lseek(cpu_t*c,mcur*u){ int fd=(int)W(c,u); int32_t off=(int32_t)W(c,u); int wh=(int)W(c,u); return (uint32_t)lseek(fd,off,wh); }
static uint64_t f_dup(cpu_t*c,mcur*u){ (void)c; return (uint32_t)dup((int)W(c,u)); }
static uint64_t f_ftruncate(cpu_t*c,mcur*u){ int fd=(int)W(c,u); int32_t len=(int32_t)W(c,u); return (uint32_t)ftruncate(fd,len); }
static uint64_t f_fsync(cpu_t*c,mcur*u){ (void)c; return (uint32_t)fsync((int)W(c,u)); }
static uint64_t f_fcntl(cpu_t*c,mcur*u){ int fd=(int)W(c,u),cmd=(int)W(c,u),arg=(int)W(c,u); return (uint32_t)fcntl(fd,cmd,arg); }
static uint64_t f_ioctl(cpu_t*c,mcur*u){ (void)c;(void)W(c,u);(void)W(c,u); return 0xffffffffu; }  /* refuse ioctls (device-specific) */
static uint64_t f_pipe(cpu_t*c,mcur*u){ uint32_t p=W(c,u); int fds[2]; int r=pipe(fds); if(r==0&&p){ gm_wr32(&c->mem,p,(uint32_t)fds[0]); gm_wr32(&c->mem,p+4,(uint32_t)fds[1]); } return (uint32_t)r; }
static uint64_t f_writev(cpu_t*c,mcur*u){ int fd=(int)W(c,u); uint32_t iov=W(c,u),n=W(c,u); uint32_t total=0; uint8_t buf[8192];
    for(uint32_t i=0;i<n;i++){ uint32_t base=gm_rd32(&c->mem,iov+i*8), len=gm_rd32(&c->mem,iov+i*8+4); uint32_t off=0;
        while(len){ uint32_t k=len<sizeof buf?len:sizeof buf; grd(c,base+off,buf,k); ssize_t put=write(fd,buf,k); if(put<=0) return total; total+=(uint32_t)put; off+=k; len-=k; } }
    return total; }

/* ---- filesystem (real where the layout is trivial) ---- */
static uint64_t f_mkdir(cpu_t*c,mcur*u){ uint32_t p=W(c,u); int mode=(int)W(c,u); char path[1024]; gstr(c,p,path,sizeof path); return (uint32_t)mkdir(path,mode); }
static uint64_t f_rmdir(cpu_t*c,mcur*u){ uint32_t p=W(c,u); char path[1024]; gstr(c,p,path,sizeof path); return (uint32_t)rmdir(path); }
static uint64_t f_unlink(cpu_t*c,mcur*u){ uint32_t p=W(c,u); char path[1024]; gstr(c,p,path,sizeof path); return (uint32_t)unlink(path); }
static uint64_t f_chmod(cpu_t*c,mcur*u){ uint32_t p=W(c,u); int mode=(int)W(c,u); char path[1024]; gstr(c,p,path,sizeof path); return (uint32_t)chmod(path,mode); }
static uint64_t f_utime(cpu_t*c,mcur*u){ (void)c;(void)W(c,u);(void)W(c,u); return 0; }
static uint64_t f_pathconf(cpu_t*c,mcur*u){ (void)c;(void)W(c,u);(void)W(c,u); return 0xffffffffu; }
static uint64_t f_statfs(cpu_t*c,mcur*u){ (void)c;(void)W(c,u);(void)W(c,u); return 0xffffffffu; }   /* layout-intricate: report unavailable */
/* stat/fstat: real host stat written into the bionic arm32 struct stat (sizeof 104;
 * offsets verified via an NDK offsetof probe: st_mode@16 st_nlink@20 st_uid@24 st_gid@28
 * st_size@48(i64) st_blksize@56 st_mtim.tv_sec@80 st_ino@96). S_IF* mode bits are POSIX
 * standard so the host st_mode copies directly. */
#define BST_SZ 104
static void wr_stat(cpu_t*c, uint32_t g, const struct stat*h){
    uint8_t z[BST_SZ]; memset(z,0,sizeof z); gwr(c,g,z,BST_SZ);
    gm_wr32(&c->mem,g+16,(uint32_t)h->st_mode);  gm_wr32(&c->mem,g+20,(uint32_t)h->st_nlink);
    gm_wr32(&c->mem,g+24,(uint32_t)h->st_uid);   gm_wr32(&c->mem,g+28,(uint32_t)h->st_gid);
    gm_wr32(&c->mem,g+48,(uint32_t)(uint64_t)h->st_size); gm_wr32(&c->mem,g+52,(uint32_t)((uint64_t)h->st_size>>32));
    gm_wr32(&c->mem,g+56,512);                    gm_wr32(&c->mem,g+80,(uint32_t)h->st_mtime);
    gm_wr32(&c->mem,g+96,(uint32_t)h->st_ino);
}
static uint64_t f_stat (cpu_t*c,mcur*u){ uint32_t p=W(c,u),g=W(c,u); char path[1024]; gstr(c,p,path,sizeof path); struct stat h; if(stat(path,&h)) return 0xffffffffu; wr_stat(c,g,&h); return 0; }
static uint64_t f_fstat(cpu_t*c,mcur*u){ int fd=(int)W(c,u); uint32_t g=W(c,u); struct stat h; if(fstat(fd,&h)) return 0xffffffffu; wr_stat(c,g,&h); return 0; }
/* directory enumeration: assets come via AAsset, not readdir -> present an empty dir */
static uint64_t f_opendir(cpu_t*c,mcur*u){ uint32_t p=W(c,u); char path[1024]; gstr(c,p,path,sizeof path);
    if(f_trace()||getenv("ABSHIM_LOG")) fprintf(stderr,"[opendir] '%s'\n",path); return 0; }
static uint64_t f_closedir(cpu_t*c,mcur*u){ (void)c;(void)W(c,u); return 0; }
static uint64_t f_readdir_r(cpu_t*c,mcur*u){ uint32_t d=W(c,u),ent=W(c,u),res=W(c,u); (void)d;(void)ent; if(res) gm_wr32(&c->mem,res,0); return 0; }  /* end of dir */

/* ---- mmap/munmap: guest-heap-backed (anonymous zero-fill or file-backed pread) ---- */
#define MAP_ANON_BIT 0x20
static struct { uint32_t base; uint32_t addr; uint32_t len; } g_maps[64]; static int g_nmaps;
static uint64_t f_mmap(cpu_t*c,mcur*u){ uint32_t hint=W(c,u),len=W(c,u),prot=W(c,u),flags=W(c,u); int fd=(int)W(c,u); uint32_t off=W(c,u); (void)hint;(void)prot;
    if(!len || len>64u*1024*1024u) return 0xffffffffu;
    uint32_t base=galloc_malloc(c->heap,len+4096u); if(!base) return 0xffffffffu;
    uint32_t g=(base+4095u)&~4095u;                 /* mmap MUST return page-aligned memory */
    uint8_t buf[8192];
    if((flags&MAP_ANON_BIT) || fd<0){ memset(buf,0,sizeof buf); uint32_t o=0,rem=len; while(rem){ uint32_t k=rem<sizeof buf?rem:sizeof buf; gwr(c,g+o,buf,k); o+=k; rem-=k; } }
    else { uint32_t o=0,rem=len; while(rem){ uint32_t k=rem<sizeof buf?rem:sizeof buf; ssize_t got=pread(fd,buf,k,(off_t)off+o); if(got<=0){ memset(buf,0,k); got=(ssize_t)k; } gwr(c,g+o,buf,(uint32_t)got); o+=k; rem-=k; } }
    if(g_nmaps<64){ g_maps[g_nmaps].base=base; g_maps[g_nmaps].addr=g; g_maps[g_nmaps].len=len; g_nmaps++; }
    return g; }
static uint64_t f_munmap(cpu_t*c,mcur*u){ uint32_t a=W(c,u); (void)W(c,u); for(int i=0;i<g_nmaps;i++) if(g_maps[i].addr==a){ galloc_free(c->heap,g_maps[i].base); g_maps[i]=g_maps[--g_nmaps]; return 0; } return 0; }

/* ---- strerror / sscanf / wide-collate ---- */
static uint32_t g_errbuf=0;
static uint64_t f_strerror(cpu_t*c,mcur*u){ int e=(int)W(c,u); if(!g_errbuf) g_errbuf=galloc_malloc(c->heap,128); if(!g_errbuf) return 0; char b[128]; snprintf(b,sizeof b,"%s",strerror(e)); gwr(c,g_errbuf,b,(uint32_t)strlen(b)+1); return g_errbuf; }
static uint64_t f_strerror_r(cpu_t*c,mcur*u){ int e=(int)W(c,u); uint32_t buf=W(c,u),len=W(c,u); char b[256]; snprintf(b,sizeof b,"%s",strerror(e)); uint32_t k=(uint32_t)strlen(b)+1; if(k>len)k=len; gwr(c,buf,b,k); return 0; }
static uint64_t f_sscanf(cpu_t*c,mcur*u){ uint32_t s=W(c,u),fmt=W(c,u); char str[4096],fs[512]; gstr(c,s,str,sizeof str); gstr(c,fmt,fs,sizeof fs);
    int matched=0; const char*ip=str,*fp=fs;
    while(*fp){
        if(isspace((unsigned char)*fp)){ while(isspace((unsigned char)*ip))ip++; fp++; continue; }
        if(*fp!='%'){ if(*ip==*fp){ip++;fp++;} else break; continue; }
        fp++; int sup=0; if(*fp=='*'){sup=1;fp++;} int width=0; while(isdigit((unsigned char)*fp)){width=width*10+(*fp-'0');fp++;}
        int lng=0; while(*fp=='l'||*fp=='h'||*fp=='L'||*fp=='j'||*fp=='z'){ if(*fp=='l')lng++; fp++; }
        char conv=*fp?*fp++:0;
        if(conv!='c'&&conv!='['&&conv!='%'){ while(isspace((unsigned char)*ip))ip++; }
        if(conv=='d'||conv=='i'||conv=='u'||conv=='x'||conv=='o'){ char*end; long v=strtol(ip,&end,conv=='x'?16:conv=='o'?8:10); if(end==ip)break;
            if(!sup){ uint32_t p=W(c,u); gm_wr32(&c->mem,p,(uint32_t)v); if(lng>=2) gm_wr32(&c->mem,p+4,(uint32_t)((int64_t)v>>32)); matched++; } ip=end; }
        else if(conv=='f'||conv=='e'||conv=='g'){ char*end; double v=strtod(ip,&end); if(end==ip)break;
            if(!sup){ uint32_t p=W(c,u); if(lng){ uint64_t b; memcpy(&b,&v,8); gm_wr32(&c->mem,p,(uint32_t)b); gm_wr32(&c->mem,p+4,(uint32_t)(b>>32)); } else { float f=(float)v; uint32_t b; memcpy(&b,&f,4); gm_wr32(&c->mem,p,b); } matched++; } ip=end; }
        else if(conv=='s'){ if(!sup){ uint32_t p=W(c,u); int k=0; while(*ip && !isspace((unsigned char)*ip) && (width==0||k<width)){ uint8_t ch=(uint8_t)*ip++; gwr(c,p+(uint32_t)k,&ch,1); k++; } uint8_t z=0; gwr(c,p+(uint32_t)k,&z,1); if(k)matched++; } else while(*ip&&!isspace((unsigned char)*ip))ip++; }
        else if(conv=='c'){ int cnt=width?width:1; if(!sup){ uint32_t p=W(c,u); int k=0; for(;k<cnt&&*ip;k++){ uint8_t ch=(uint8_t)*ip++; gwr(c,p+(uint32_t)k,&ch,1); } if(k)matched++; } else for(int k=0;k<cnt&&*ip;k++)ip++; }
        else if(conv=='%'){ if(*ip=='%')ip++; else break; }
        else break;
    }
    return (uint32_t)matched; }
static uint64_t f_wcscoll(cpu_t*c,mcur*u){ uint32_t a=W(c,u),b=W(c,u); for(uint32_t i=0;;i++){ uint32_t x=gm_rd32(&c->mem,a+i*4),y=gm_rd32(&c->mem,b+i*4); if(x!=y) return (uint32_t)(x<y?-1:1); if(!x) return 0; } }
static uint64_t f_wcsxfrm(cpu_t*c,mcur*u){ uint32_t d=W(c,u),s=W(c,u),n=W(c,u); uint32_t i=0; for(;;i++){ uint32_t ch=gm_rd32(&c->mem,s+i*4); if(i<n) gm_wr32(&c->mem,d+i*4,ch); if(!ch) break; } return i; }
static uint64_t f_wcsftime(cpu_t*c,mcur*u){ uint32_t d=W(c,u),max=W(c,u); (void)W(c,u);(void)W(c,u); if(max&&d) gm_wr32(&c->mem,d,0); return 0; }

typedef uint64_t (*ffn)(cpu_t*, mcur*);
typedef struct { const char*name; int ret; ffn fn; } fent;
static const fent FT[] = {
    {"fopen",1,f_fopen},{"freopen",1,f_freopen},{"fdopen",1,f_fdopen},{"fclose",1,f_fclose},
    {"fread",1,f_fread},{"fwrite",1,f_fwrite},{"fseek",1,f_fseek},{"ftell",1,f_ftell},
    {"fseeko",1,f_fseek},{"ftello",1,f_ftell},{"feof",1,f_feof},{"ferror",1,f_ferror},{"fflush",1,f_fflush},
    {"fgets",1,f_fgets},{"fputc",1,f_fputc},{"fputs",1,f_fputs},{"fgetc",1,f_fgetc},{"getc",1,f_fgetc},
    {"putc",1,f_putc},{"puts",1,f_puts},{"ungetc",1,f_ungetc},{"setvbuf",1,f_setvbuf},
    {"remove",1,f_remove},{"rename",1,f_rename},{"tmpfile",1,f_tmpfile},{"mkstemp",1,f_mkstemp},{"perror",1,f_perror},
    {"fprintf",1,f_fprintf},{"vfprintf",1,f_vfprintf},{"vprintf",1,f_vprintf},{"vsprintf",1,f_vsprintf},
    {"getwc",1,f_getwc},{"putwc",1,f_putwc},{"ungetwc",1,f_ungetwc},{"wcrtomb",1,f_wcrtomb},
    {"open",1,f_open},{"openat",1,f_openat},{"read",1,f_read},{"write",1,f_write},{"close",1,f_close},{"lseek",1,f_lseek},
    {"dup",1,f_dup},{"ftruncate",1,f_ftruncate},{"fsync",1,f_fsync},{"fcntl",1,f_fcntl},{"ioctl",1,f_ioctl},{"pipe",1,f_pipe},{"writev",1,f_writev},
    {"mkdir",1,f_mkdir},{"rmdir",1,f_rmdir},{"unlink",1,f_unlink},{"chmod",1,f_chmod},{"utime",1,f_utime},
    {"pathconf",1,f_pathconf},{"statfs",1,f_statfs},{"stat",1,f_stat},{"fstat",1,f_fstat},
    {"opendir",1,f_opendir},{"closedir",1,f_closedir},{"readdir_r",1,f_readdir_r},
    {"mmap",1,f_mmap},{"munmap",1,f_munmap},
    {"strerror",1,f_strerror},{"strerror_r",1,f_strerror_r},{"sscanf",1,f_sscanf},
    {"wcscoll",1,f_wcscoll},{"wcsxfrm",1,f_wcsxfrm},{"wcsftime",1,f_wcsftime},
};

int file_try(cpu_t *c, const char *name, mcur *cur){
    for(size_t i=0;i<sizeof FT/sizeof FT[0];i++){
        if(!strcmp(FT[i].name,name)){
            uint64_t rv=FT[i].fn(c,cur);
            if(FT[i].ret>=1){ uint32_t lo=(uint32_t)rv; uc_reg_write(c->uc,UC_ARM_REG_R0,&lo); }
            if(FT[i].ret==2){ uint32_t hi=(uint32_t)(rv>>32); uc_reg_write(c->uc,UC_ARM_REG_R1,&hi); }
            return 1;
        }
    }
    return 0;
}
