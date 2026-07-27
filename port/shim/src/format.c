/* format.c — printf/scanf varargs engine. See format.h.
 *
 * One core (fmt_core) drives an emit sink (guest buffer or host buffer) and an
 * argument source (marshal cursor for source A, guest va_list for source B).
 * Each conversion is parsed, its argument pulled per the soft-float ABI, and
 * formatted via the host libc into a temp, then emitted. */
#include "format.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define MAXW  4096            /* clamp for width/precision (formats are internal) */
#define TMP   8192            /* per-conversion host scratch (fits %.4096f etc.) */

typedef struct {
    guest_mem *m;
    /* guest sink (s(n)printf): cap = size (0xFFFFFFFF for sprintf), 0 = unused */
    uint32_t out, cap, stored;
    /* host sink (printf/log): 0 = unused */
    char    *hbuf; uint32_t hcap, hlen;
    uint32_t written;               /* the C return value */
    int      is_va;                 /* 0 = source A (cursor), 1 = source B (va) */
    mcur     cur;
    uint32_t va;
} fmt_ctx;

/* ---- argument source (soft-float AAPCS32) ---- */
static uint32_t va_word(guest_mem *m, uint32_t *va){ uint32_t v=gm_rd32(m,*va); *va+=4; return v; }
static uint64_t va_dword(guest_mem *m, uint32_t *va){
    *va = (*va + 7u) & ~7u;
    uint32_t lo=gm_rd32(m,*va), hi=gm_rd32(m,*va+4); *va+=8;
    return (uint64_t)lo | ((uint64_t)hi<<32);
}
static uint32_t pull_word (fmt_ctx *f){ return f->is_va ? va_word (f->m,&f->va) : marshal_pull_word (f->m,&f->cur); }
static uint64_t pull_dword(fmt_ctx *f){ return f->is_va ? va_dword(f->m,&f->va) : marshal_pull_dword(f->m,&f->cur); }

/* ---- emit sink ---- */
static void emit(fmt_ctx *f, char ch){
    if (f->out && f->cap && f->stored < f->cap - 1){ gm_wr8(f->m, f->out + f->stored, (uint8_t)ch); f->stored++; }
    else if (f->hbuf && f->hlen < f->hcap - 1){ f->hbuf[f->hlen++] = ch; }
    f->written++;
}
static void emit_str(fmt_ctx *f, const char *s){ while (*s) emit(f, *s++); }

static void fmt_core(fmt_ctx *f, uint32_t fmt){
    uint32_t p = fmt;
    for (;;){
        uint8_t c = gm_rd8(f->m, p++);
        if (!c) break;
        if (c != '%'){ emit(f, (char)c); continue; }
        if (gm_rd8(f->m, p) == '%'){ emit(f, '%'); p++; continue; }

        char spec[96]; int s = 0; spec[s++] = '%';
        /* flags */
        for (;;){ uint8_t fc = gm_rd8(f->m, p);
            if (fc=='-'||fc=='+'||fc==' '||fc=='#'||fc=='0'){ if (s<80) spec[s++]=(char)fc; p++; } else break; }
        /* width (number or '*') */
        { uint8_t wc = gm_rd8(f->m, p); long w = -1;
          if (wc=='*'){ p++; int v=(int)pull_word(f); if (v<0){ if(s<80)spec[s++]='-'; v=-v; } w=v; }
          else { long acc=0; int any=0; while ((wc=gm_rd8(f->m,p))>='0'&&wc<='9'){ any=1; acc=acc*10+(wc-'0'); if(acc>MAXW)acc=MAXW; p++; } if(any) w=acc; }
          if (w>MAXW) w=MAXW;
          if (w>=0) s += snprintf(spec+s, 16, "%ld", w); }
        /* precision ('.' number or '.*') */
        { uint8_t pc = gm_rd8(f->m, p);
          if (pc=='.'){ p++; pc=gm_rd8(f->m,p); long pr=0;
            if (pc=='*'){ p++; int v=(int)pull_word(f); pr = v<0 ? -1 : v; }
            else { pr=0; while ((pc=gm_rd8(f->m,p))>='0'&&pc<='9'){ pr=pr*10+(pc-'0'); if(pr>MAXW)pr=MAXW; p++; } }
            if (pr>MAXW) pr=MAXW;
            if (pr>=0) s += snprintf(spec+s, 20, ".%ld", pr);
            else s += snprintf(spec+s, 4, ".0"); } }
        /* length modifier (consumed; host length reconstructed below) */
        int is64 = 0;
        { uint8_t lc = gm_rd8(f->m, p);
          if (lc=='h'){ p++; if (gm_rd8(f->m,p)=='h') p++; }
          else if (lc=='l'){ p++; if (gm_rd8(f->m,p)=='l'){ p++; is64=1; } }   /* guest 'l' = 32-bit */
          else if (lc=='j'){ p++; is64=1; }
          else if (lc=='z'||lc=='t'){ p++; }                                    /* size_t/ptrdiff_t = 32-bit */
          else if (lc=='L'){ p++; } }                                           /* long double == double */
        /* conversion */
        uint8_t cv = gm_rd8(f->m, p++);
        char tmp[TMP];
        switch (cv){
        case 'd': case 'i':
            if (is64){ long long v=(long long)pull_dword(f); spec[s++]='l';spec[s++]='l';spec[s++]=(char)cv;spec[s]=0; snprintf(tmp,TMP,spec,v); }
            else     { int v=(int)pull_word(f);              spec[s++]=(char)cv;spec[s]=0;                        snprintf(tmp,TMP,spec,v); }
            emit_str(f,tmp); break;
        case 'u': case 'o': case 'x': case 'X':
            if (is64){ unsigned long long v=pull_dword(f); spec[s++]='l';spec[s++]='l';spec[s++]=(char)cv;spec[s]=0; snprintf(tmp,TMP,spec,v); }
            else     { unsigned v=(unsigned)pull_word(f);  spec[s++]=(char)cv;spec[s]=0;                          snprintf(tmp,TMP,spec,v); }
            emit_str(f,tmp); break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            uint64_t bits = pull_dword(f); double d; memcpy(&d,&bits,8);
            spec[s++]=(char)cv; spec[s]=0; snprintf(tmp,TMP,spec,d); emit_str(f,tmp); break; }
        case 'c': { int v=(int)pull_word(f); spec[s++]='c'; spec[s]=0; snprintf(tmp,TMP,spec,v); emit_str(f,tmp); break; }
        case 'p': { uint32_t v=pull_word(f); spec[s++]='p'; spec[s]=0; snprintf(tmp,TMP,spec,(void*)(uintptr_t)v); emit_str(f,tmp); break; }
        case 's': {
            uint32_t sp = pull_word(f);
            static char sb[MAXW+1];
            uint32_t i=0;
            if (sp){ for (; i<MAXW; i++){ uint8_t ch=gm_rd8(f->m, sp+i); if(!ch) break; sb[i]=(char)ch; } }
            sb[i]=0;
            spec[s++]='s'; spec[s]=0; snprintf(tmp,TMP,spec, sp? sb : "(null)"); emit_str(f,tmp); break; }
        case 'n': { uint32_t ptr=pull_word(f); if (ptr) gm_wr32(f->m, ptr, f->written); break; }
        default:  { emit(f,'%'); if (cv) emit(f,(char)cv); break; }
        }
    }
    /* NUL-terminate the active sink */
    if (f->out && f->cap) gm_wr8(f->m, f->out + f->stored, 0);
    if (f->hbuf && f->hcap) f->hbuf[f->hlen] = 0;
}

uint32_t fmt_to_guest(guest_mem *m, uint32_t out, uint32_t cap, uint32_t fmt, mcur cur){
    fmt_ctx f; memset(&f,0,sizeof f);
    f.m=m; f.out=out; f.cap=cap; f.is_va=0; f.cur=cur;
    fmt_core(&f, fmt);
    return f.written;
}
uint32_t fmt_to_guest_va(guest_mem *m, uint32_t out, uint32_t cap, uint32_t fmt, uint32_t va){
    fmt_ctx f; memset(&f,0,sizeof f);
    f.m=m; f.out=out; f.cap=cap; f.is_va=1; f.va=va;
    fmt_core(&f, fmt);
    return f.written;
}
uint32_t fmt_to_host(guest_mem *m, char *hbuf, uint32_t hcap, uint32_t fmt, mcur cur, uint32_t *outlen){
    fmt_ctx f; memset(&f,0,sizeof f);
    f.m=m; f.hbuf=hbuf; f.hcap=hcap; f.is_va=0; f.cur=cur;
    fmt_core(&f, fmt);
    if (outlen) *outlen = f.hlen;
    return f.written;
}
