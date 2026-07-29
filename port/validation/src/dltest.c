/* dltest.c — ask the ANDROID LOADER whether a shared object can be loaded on this kernel.
 *
 * WHY THIS EXISTS
 * ---------------
 * The 16 KB page-size claim used to rest on two weaker things: the ELF program headers saying
 * p_align=0x4000, and the APK installing on a 16 KB device. Neither is the property that matters.
 * Android rejects a misaligned library at **dlopen**, not at install, so "it installed" says nothing
 * about alignment — an overclaim that sat in OPEN_FINDINGS until this program was written.
 *
 * Being tiny is the point: it needs no Activity, no ART, and no working app, so it can test the
 * loader on an image where the game itself refuses to launch (see R18).
 *
 * Build it 16 KB-aligned, or it cannot run on the very kernel it is meant to test — the first
 * version was built without the flag, segfaulted on exec, and looked exactly like "the shim fails
 * to load on 16 KB". Only the negative control (dlopening system libc, which must work) revealed
 * that the tester itself had never started.
 */
#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: dltest <path.so>\n"); return 2; }
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { printf("DLOPEN-FAIL: %s\n", dlerror()); return 1; }
    printf("DLOPEN-OK\n");
    return 0;
}
