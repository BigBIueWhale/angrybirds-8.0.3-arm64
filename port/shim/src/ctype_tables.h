/* ctype_tables.h — GUESTDATA content for the 5 UND_OBJECT data-symbol mirrors
 * (Audit 03 B1-1 / Audit 06). elf32 classifies these imports as SC_UND_OBJECT;
 * the device loader asks this module for the real bytes + the symbol offset and
 * resolves each GLOB_DAT to (mirror_base + sym_off).
 *
 * Layout (from the Audit-06 disasm-derived spec):
 *   _ctype_        : const char*  POINTER var -> char[257] table; T[c+1]=class(c),T[0]=0.
 *                    inline is* compute  _ctype_[c+1] & MASK  (ldr ptr; ldrb [ptr+c+1]).
 *   _tolower_tab_  : const short* POINTER var -> short[257] table (ptr=&T[1]); T[c+1]=tolower(c),T[0]=-1.
 *   _toupper_tab_  : const short* POINTER var -> short[257] table (ptr=&T[1]); T[c+1]=toupper(c),T[0]=-1.
 *                    inline _tolower/_toupper[c] (the +1 lives in the pointer target).
 *   NB: these three are POINTER variables — the symbol resolves to a 4-byte pointer
 *   word holding &T[sym_off], written just past the table (see ctype_write_mirror).
 *   __sF           : 3 x 84-byte bionic __sFILE; symbol = &array[0];
 *                    _flags@0xc, _file@0xe = 0/1/2 (stdin/stdout/stderr).
 *   __stack_chk_guard : 4-byte canary (fixed, with a NUL byte); symbol = &value.
 *
 * The classification bit masks match BSD/bionic so the inline predicates behave
 * exactly as the compiled-in macros expect. */
#ifndef ABSHIM_CTYPE_TABLES_H
#define ABSHIM_CTYPE_TABLES_H
#include "memops.h"

/* BSD/bionic _ctype_ classification bits (used by the inline is* macros). */
#define _CT_U 0x01u   /* upper   */
#define _CT_L 0x02u   /* lower   */
#define _CT_N 0x04u   /* digit   */
#define _CT_S 0x08u   /* space   */
#define _CT_P 0x10u   /* punct   */
#define _CT_C 0x20u   /* control */
#define _CT_X 0x40u   /* hex     */
#define _CT_B 0x80u   /* blank/printable-space */

/* Return the host-side mirror bytes for a data-object symbol, or NULL if `name`
 * is not one of the five. *size = byte length of the mirror region; *sym_off =
 * the offset to add to the mirror base when resolving the symbol's GLOB_DAT. */
const void *ctype_mirror(const char *name, uint32_t *size, uint32_t *sym_off);

/* Write the mirror for `name` into guest memory at gaddr and return the guest
 * address the symbol should resolve to, or 0 if unknown. For the three pointer-
 * variable symbols this writes a 4-byte pointer word past the table and returns
 * its address; for the direct symbols it returns gaddr + sym_off. *consumed gets
 * the total guest bytes written (table [+ pointer word]) so the caller can advance
 * its GUESTDATA cursor correctly. */
uint32_t ctype_write_mirror(guest_mem *m, uint32_t gaddr, const char *name, uint32_t *consumed);

#endif /* ABSHIM_CTYPE_TABLES_H */
