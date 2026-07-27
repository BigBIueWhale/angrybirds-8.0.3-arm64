/* regions.h — the ONE combined guest address-space map (Audit 05 + Audit 09 S1).
 * Non-overlapping; every region every audit reserves has exactly one home. */
#ifndef ABSHIM_REGIONS_H
#define ABSHIM_REGIONS_H

#define RG_STUB       0x10000000u   /* import bx-lr trampoline arena  (R-X) */
#define RG_STUB_SZ    0x00020000u
#define RG_JNI        0x11000000u   /* JNI env/VM trampoline arena    (R-X) */
#define RG_JNI_SZ     0x00010000u
#define RG_GUESTDATA  0x12000000u   /* ctype/__sF/canary + JNI sentinels+glGetString cache (RW) */
#define RG_GUESTDATA_SZ 0x00010000u
#define RG_ENGINE     0x40000000u   /* relocated 32-bit engine        (per-seg) */
#define RG_ENGINE_MAX 0x08000000u   /* reserve window to 0x47FFFFFF */
#define RG_TCB        0x48000000u   /* per-GEC TCB pages (errno+tls+keys) (RW) */
#define RG_TCB_SZ     0x01000000u
#define RG_HEAP       0x50000000u   /* real allocator arena           (RW) */
#define RG_HEAP_SZ    0x20000000u   /* 512MB. Genuine exhaustion is real once the boot PROGRESSES all the way to
                                      * GAMEPLAY: 128MB died in the menu (audio PCM decode), 256MB died at the
                                      * level/physics live-set (LZMA script decode -> St9bad_alloc). The 32-bit
                                      * guest has ~1.4GB of unused VA above the stack, so grow the heap to 0x50..0x70
                                      * and relocate ASSET/STACK/LIB above 0x80000000. (Old "256MB red herring" note
                                      * was about the now-fixed corrupt-length append; real gameplay needs the room.) */
#define RG_HEAPGUARD  0x70000000u   /* unmapped: heap overrun -> fault (heap now ends at 0x70000000) */
#define RG_STACK      0x80000000u   /* per-GEC guard-framed stacks     (RW) — relocated above the bigger heap */
#define RG_STACK_SZ   0x10000000u
#define RG_ASSET      0x90000000u   /* AAsset_getBuffer guest copies  (RW) — relocated */
#define RG_ASSET_SZ   0x08000000u
#define RG_LIB        0xa0000000u   /* dormant guest-lib (single-lib), unused — relocated out of the heap window */
#define RG_RET        0xdead0000u   /* emu_call return trap (∉ exidx)  (R-X) */
#define RG_RET_SZ     0x00001000u
#define RG_KUSER      0xffff0000u   /* kuser helper page               (R-X) */
#define RG_KUSER_SZ   0x00001000u

/* kuser helper entry points */
#define KUSER_CMPXCHG  0xffff0fc0u
#define KUSER_GET_TLS  0xffff0fe0u
#define KUSER_MEMBAR   0xffff0fa0u

#define ARM_BX_LR      0xe12fff1eu   /* the trampoline fill instruction */

#endif /* ABSHIM_REGIONS_H */
