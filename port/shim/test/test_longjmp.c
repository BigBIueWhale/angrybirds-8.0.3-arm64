/* test_longjmp.c — device-layer: validate setjmp/longjmp + the cpu_run stop/restart
 * PC redirect with a hand-assembled ARM snippet. No engine needed. */
#include "dispatch.h"
#include "regions.h"
#include <stdio.h>
#include <string.h>

static int fails=0;
#define CK(c,msg) do{ if(!(c)){ printf("  FAIL: %s\n",msg); fails++; } }while(0)

/* ARM encoders for the PC-relative branches */
static uint32_t arm_bl (uint32_t from, uint32_t to){ int32_t off=((int32_t)(to-(from+8)))>>2; return 0xEB000000u|((uint32_t)off&0xFFFFFFu); }
static uint32_t arm_bne(uint32_t from, uint32_t to){ int32_t off=((int32_t)(to-(from+8)))>>2; return 0x1A000000u|((uint32_t)off&0xFFFFFFu); }

int main(void){
    setvbuf(stdout,NULL,_IONBF,0);
    printf("=== setjmp/longjmp round-trip (hand-assembled) ===\n");
    cpu_t cpu; if(cpu_create(&cpu)){ printf("cpu_create failed\n"); return 1; }

    /* minimal loader: stub slot 0 = setjmp, slot 1 = longjmp */
    loader_t L; memset(&L,0,sizeof L);
    L.stub_name[0]="setjmp"; L.stub_name[1]="longjmp"; L.stub_count=2;
    dispatch_t D; dispatch_install(&D,&cpu,&L);

    /* code page adjacent to the stub arena (within bl reach) */
    uint32_t CODE=0x10040000u;
    uc_mem_map(cpu.uc, CODE, 0x1000, UC_PROT_READ|UC_PROT_EXEC);
    uint32_t setjmp_stub=RG_STUB+0, longjmp_stub=RG_STUB+4, done=CODE+0x2c;
    uint32_t prog[]={
        0xe1a0a00e,                 /* +00 mov r10,lr   (save real return; longjmp clobbers lr) */
        0xe3a04011,                 /* +04 mov r4,#0x11 (callee-saved marker) */
        0xe1a00005,                 /* +08 mov r0,r5    (r0=jmpbuf) */
        arm_bl(CODE+0x0c,setjmp_stub),/*+0c bl setjmp */
        0xe3500000,                 /* +10 cmp r0,#0 */
        arm_bne(CODE+0x14,done),    /* +14 bne done     (if returned from longjmp) */
        0xe3a04022,                 /* +18 mov r4,#0x22 (clobber r4) */
        0xe1a00005,                 /* +1c mov r0,r5 */
        0xe3a01007,                 /* +20 mov r1,#7    (longjmp val) */
        arm_bl(CODE+0x24,longjmp_stub),/*+24 bl longjmp */
        0xe3a0902f,                 /* +28 mov r9,#0x2f (WRONG-PATH: must be skipped) */
        0xe12fff1a,                 /* +2c done: bx r10 (return via saved lr; r10 restored by longjmp) */
    };
    uc_mem_write(cpu.uc, CODE, prog, sizeof prog);

    uint32_t jmpbuf=RG_GUESTDATA+0x200;   /* RW, mapped */
    uint32_t r5=jmpbuf, sp=RG_STACK+RG_STACK_SZ-0x1000, lr=RG_RET, z=0;
    uc_reg_write(cpu.uc,UC_ARM_REG_R5,&r5);
    uc_reg_write(cpu.uc,UC_ARM_REG_SP,&sp);
    uc_reg_write(cpu.uc,UC_ARM_REG_LR,&lr);
    uc_reg_write(cpu.uc,UC_ARM_REG_R4,&z);
    uc_reg_write(cpu.uc,UC_ARM_REG_R9,&z);

    uc_err e=cpu_run(&cpu, CODE, 100000);
    uint32_t r0,r4,r9,pc;
    uc_reg_read(cpu.uc,UC_ARM_REG_R0,&r0); uc_reg_read(cpu.uc,UC_ARM_REG_R4,&r4);
    uc_reg_read(cpu.uc,UC_ARM_REG_R9,&r9); uc_reg_read(cpu.uc,UC_ARM_REG_PC,&pc);
    printf("  emu=%s pc=0x%x  r0=%u r4=0x%x r9=0x%x\n", uc_strerror(e), pc, r0, r4, r9);
    CK(e==UC_ERR_OK,"emu ok");
    CK(r0==7,"longjmp delivered val=7 to the setjmp site");
    CK(r4==0x11,"callee-saved r4 restored to its setjmp-time value (0x11, not 0x22)");
    CK(r9!=0x2f,"instruction after bl longjmp was NOT executed (control redirected)");
    CK(pc==RG_RET,"returned to RG_RET");

    cpu_destroy(&cpu);
    printf(fails? "\n=== %d FAILURE(S) ===\n":"\n=== ALL PASS ===\n", fails);
    return fails?1:0;
}
