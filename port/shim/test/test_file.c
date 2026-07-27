/* test_file.c — validate bridge_file.c's stdio FILE model end-to-end: a real
 * fopen/fwrite/fclose then fopen/fread must round-trip the bytes through the guest.
 * Exercises the bionic __sFILE guest struct + the guest<->host FILE* side table. */
#include "bridge.h"
#include "regions.h"
#include <stdio.h>
#include <string.h>

static int fails=0;
#define CK(c,msg) do{ if(!(c)){ printf("  FAIL: %s\n",msg); fails++; } }while(0)

static cpu_t CPU;
static void setr(int id,uint32_t v){ uc_reg_write(CPU.uc,id,&v); }
static uint32_t getr(int id){ uint32_t v=0; uc_reg_read(CPU.uc,id,&v); return v; }
static void gwrite(uint32_t a,const void*p,uint32_t n){ uc_mem_write(CPU.uc,a,p,n); }
static void gread (uint32_t a,void*p,uint32_t n){ uc_mem_read(CPU.uc,a,p,n); }
static void gstrw(uint32_t a,const char*s){ gwrite(a,s,(uint32_t)strlen(s)+1); }

/* drive file_try(name) with up to 4 word args in r0..r3; return r0 */
static uint32_t call(const char*nm,uint32_t a0,uint32_t a1,uint32_t a2,uint32_t a3){
    setr(UC_ARM_REG_R0,a0); setr(UC_ARM_REG_R1,a1); setr(UC_ARM_REG_R2,a2); setr(UC_ARM_REG_R3,a3);
    mcur cur; marshal_cur_init(&CPU.mem,&cur);
    int ok=file_try(&CPU,nm,&cur);
    CK(ok==1,nm);
    return getr(UC_ARM_REG_R0);
}

int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    printf("=== bridge_file stdio round-trip ===\n");
    if(cpu_create(&CPU)){ printf("cpu_create failed\n"); return 1; }
    setr(UC_ARM_REG_SP, RG_STACK+RG_STACK_SZ-0x1000u);

    uint32_t P=RG_GUESTDATA+0x400, M=RG_GUESTDATA+0x500, BUF=RG_GUESTDATA+0x600, RBUF=RG_GUESTDATA+0x700;
    const char *path="/tmp/abshim_filetest.bin";
    const char data[8]={'H','E','L','L','O','1','2','3'};
    gstrw(P,path);
    gstrw(M,"wb");
    gwrite(BUF,data,8);

    uint32_t fp=call("fopen",P,M,0,0);
    printf("   fopen(w) -> guest FILE* 0x%x\n", fp);
    CK(fp!=0,"fopen for write succeeded");
    if(fp){
        uint32_t nw=call("fwrite",BUF,1,8,fp);
        CK(nw==8,"fwrite wrote 8 bytes");
        uint32_t rc=call("fclose",fp,0,0,0);
        CK(rc==0,"fclose(w) == 0");
    }

    gstrw(M,"rb");
    uint32_t fp2=call("fopen",P,M,0,0);
    printf("   fopen(r) -> guest FILE* 0x%x\n", fp2);
    CK(fp2!=0,"fopen for read succeeded");
    if(fp2){
        uint32_t nr=call("fread",RBUF,1,8,fp2);
        CK(nr==8,"fread read 8 bytes");
        call("fclose",fp2,0,0,0);
        char got[8]; gread(RBUF,got,8);
        CK(memcmp(got,data,8)==0,"bytes round-tripped through the guest");
    }

    /* stat: real host stat emitted into the bionic arm32 struct stat layout */
    uint32_t ST=RG_GUESTDATA+0x800;
    uint32_t sr=call("stat",P,ST,0,0);
    uint32_t st_mode=0,st_size=0; gread(ST+16,&st_mode,4); gread(ST+48,&st_size,4);
    printf("   stat -> ret=%d mode@16=0%o size@48=%u\n", sr, st_mode, st_size);
    CK(sr==0,"stat succeeded");
    CK((st_mode&0xF000u)==0x8000u,"stat st_mode@16 = S_IFREG");
    CK(st_size==8,"stat st_size@48 = 8 (bytes written)");

    /* fclose on a FOREIGN FILE* (e.g. a stdio stream via __sF, not from fopen) must be
     * safe: it must NOT galloc_free a non-heap address (that would corrupt the allocator) */
    uint32_t fr=call("fclose", RG_GUESTDATA+0x900, 0,0,0);
    CK(fr==0xffffffffu,"fclose(foreign FILE*) -> -1 (no galloc_free of a foreign addr)");
    uint32_t fp3=call("fopen",P,M,0,0);           /* allocator still works? */
    CK(fp3!=0,"allocator intact after a foreign fclose");
    if(fp3) call("fclose",fp3,0,0,0);

    /* mmap must return PAGE-ALIGNED memory (guests mask to the page base) */
    setr(UC_ARM_REG_R0,0); setr(UC_ARM_REG_R1,8192); setr(UC_ARM_REG_R2,3); setr(UC_ARM_REG_R3,0x20);
    uint32_t msp=getr(UC_ARM_REG_SP); gm_wr32(&CPU.mem,msp,0xffffffffu); gm_wr32(&CPU.mem,msp+4,0);
    { mcur cur; marshal_cur_init(&CPU.mem,&cur); file_try(&CPU,"mmap",&cur); }
    uint32_t mm=getr(UC_ARM_REG_R0);
    printf("   mmap(anon,8192) -> 0x%x\n", mm);
    CK(mm!=0xffffffffu,"mmap succeeded");
    CK((mm&0xfffu)==0,"mmap result is page-aligned");

    remove(path);

    cpu_destroy(&CPU);
    printf(fails? "\n=== %d FAILURE(S) ===\n":"\n=== ALL PASS ===\n", fails);
    return fails?1:0;
}
