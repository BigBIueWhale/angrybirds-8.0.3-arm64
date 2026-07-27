# Runtime libc/GL bridge coverage — the gap between "boots" and "plays"

**Finding (2026-07-25):** the engine has **343 UND FUNC imports**; the dispatch `BR[]`
table + `gl_try` + `asset_try` currently bridge **156**. **187 are UNBRIDGED** and
today hit the `UNIMPL→0` path (silent wrong value). The 125 init_array ctors pass with
0 unimpl only because these are **runtime** (physics/render/save/parse) calls, not
init-time. This is the top correctness item after threading — several are gameplay-
critical (all trig/sqrt → 0 means Box2D + rendering produce garbage) and several are
security-critical (network `UNIMPL→0` makes `socket()`==fd 0 = "success").

Reproduce: `python3` coverage check vs `readelf --dyn-syms` (see conversation cont.25).

## Priority P0 — gameplay/correctness/security critical

### Math (libm host passthrough; soft-float double in r0:r1/r2:r3, ret r0:r1; float in/out r0-bits)
`h_ceil`/`h_floor` already show the pattern (`DW` pulls a double, `d_out` returns).
- double: `acos asin atan atan2 cos cosh sin sinh tan tanh exp log log10 pow sqrt fmod ldexp frexp modf rint difftime`
- float:  `acosf asinf atan2f ceilf cosf floorf fmodf modff sinf sqrtf tanf`
- long:   `lrint lrintf`
→ trivial `#include <math.h>` passthroughs. **P0 — without these all physics/trig = 0.**

### Network — HARD-FAIL facade (NEVER a real socket; Audit 06 L16 + public-IPv4 box rule)
`socket connect bind send sendto recv recvfrom getaddrinfo freeaddrinfo gai_strerror
getpeername getsockname getsockopt setsockopt gethostname inet_addr inet_ntop inet_pton
if_indextoname if_nametoindex getservbyport poll`
→ each returns the FAILURE value (socket/connect/bind/send/recv → -1 with errno=EACCES;
getaddrinfo → EAI_FAIL; inet_* → 0/-1). **`UNIMPL→0` is a security bug here** (0 = a valid
fd / success). Reinforces the de-phone-home guarantee at the libc layer too.

### dl — single-lib hard-fail (Audit 03)
`dlopen`→0, `dlsym`→0, `dlclose`→0, `dlerror`→a static "unsupported" string.

### process / fatal
`exit _exit raise` → route to the fatal channel (g_fatal → FatalError), NOT →0.

## Priority P1 — needed for real play (saves/config/parse/time)

### stdio FILE* (Audit 06: force bionic `__sFILE`, real fd-backed, ONE sandbox dir)
`fopen fclose fread fwrite fprintf vfprintf fputc fputs fgets fseek fseeko ftell ftello
feof ferror fflush freopen fdopen setvbuf ungetc getc putc puts remove rename tmpfile
mkstemp perror vprintf vsprintf` — wire to the existing `fdtable` + `format` modules.

### filesystem (real fd-backed under the sandbox, else safe fail)
`open* read write close lseek stat fstat access mkdir rmdir unlink chmod ftruncate fsync
getcwd opendir closedir readdir_r statfs pathconf dup pipe fcntl ioctl` (open/read/close/
lseek/fstat currently STUBBED to -1/0 in BR[] — make real).

### string / conversion (host passthrough)
`strtol strtoll strtoul strtoull strtod sscanf strtok_r strncat strcasecmp strncasecmp
strcspn strspn strpbrk strcoll strxfrm strerror strerror_r memmem memrchr atoi atol
basename fnmatch`

### ctype (host or the ctype_tables module)
`isdigit isspace isupper isxdigit tolower`

### time (real monotonic/wall model; Audit 06 L9)
`time clock localtime localtime_r gmtime gmtime_r mktime strftime nanosleep utime`

### wide char (C-locale host passthrough)
`wcslen wmemchr wmemcmp wmemcpy wmemmove wmemset wcscoll wcsxfrm wcsftime wcrtomb mbrtowc
getwc putwc ungetwc towlower towupper iswctype`

### qsort / bsearch — guest-comparator trampolines (Audit 06 L17)
comparator is a guest fn-ptr → call it via nested `cpu_run` (S3 non-blocking leaf), like
`sched_once` runs the once-routine.

### rand (48-bit LCG; Audit 06 L10)  `lrand48 srand48`
### mmap/munmap (real, Audit 05)  `mmap munmap`
### system info (sane defaults)  `getpid geteuid getpwuid getenv __system_property_get uname sigaction sigprocmask writev`

## Priority P2 — GL additions to bridge_gl.c (dlsym passthrough, same as the ~70 present)
`glFinish glFlush glReadPixels glGetActiveUniform glGetUniformfv glCopyTexImage2D glValidateProgram`
(`glFinish`/`glFlush` matter for frame pacing; the OUT-pointer ones need guest copy-back.)

## Implementation plan
1. New `src/bridge_libc.c` with a `libc_try(cpu, name, mcur)` table (math/string/time/
   wchar/ctype/rand/sysinfo passthroughs + network/dl hard-fail), routed from `stub_cb`
   like `gl_try`/`asset_try` (one added line). Keeps dispatch.c's BR[] small.
2. FILE/fd + qsort/bsearch + mmap into dispatch.c proper (they touch cpu/heap/fdtable).
3. Add the 7 GL fns to bridge_gl.c.
4. A host coverage test: assert EVERY engine UND FUNC resolves to a bridge (not UNIMPL),
   so this can never silently regress. Then re-run suite + rebuild APK.

**Order:** math + network-hardfail + exit/dl first (P0), then FILE/string/time/qsort (P1),
then GL (P2). Best done as a sequential Opus subagent for the mechanical bulk (math/
string/wchar/time passthroughs), with the tricky ones (network semantics, qsort
trampoline, FILE model, mmap) done by hand + reviewed.
