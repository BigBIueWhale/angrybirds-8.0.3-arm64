#!/usr/bin/env python3
"""Stage-3 PoC (host, any arch): load ARM32 libAngryBirdsClassic.so into Unicorn,
apply relocations, run the C++ init_array constructors under emulation with a
minimal libc bridge, and report how far it gets + which imports fire.
Proves the emulate-and-bridge approach on THIS binary before any Android build."""
import struct, sys
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection
from unicorn import *
from unicorn.arm_const import *

SO   = "/work/work803/libv7/libAngryBirdsClassic.so"
BASE = 0x40000000
STACK= 0x70000000; STACK_SZ=0x200000
STUB = 0x10000000; STUB_SZ =0x20000
HEAP = 0x50000000; HEAP_SZ =0x08000000
RET  = 0xdead0000
R_ARM_ABS32, R_ARM_GLOB_DAT, R_ARM_JUMP_SLOT, R_ARM_RELATIVE = 2, 21, 22, 23

elf = ELFFile(open(SO,"rb"))
pgup = lambda x,p=0x1000:(x+p-1)&~(p-1)

loads=[s for s in elf.iter_segments() if s['p_type']=='PT_LOAD']
image_sz=pgup(max(s['p_vaddr']+s['p_memsz'] for s in loads))
mu=Uc(UC_ARCH_ARM, UC_MODE_ARM)
mu.mem_map(BASE, image_sz)
for s in loads: mu.mem_write(BASE+s['p_vaddr'], s.data())
for a,sz in ((STACK,STACK_SZ),(STUB,STUB_SZ),(HEAP,HEAP_SZ),(RET&~0xfff,0x1000)): mu.mem_map(a,sz)
# enable VFP/NEON coprocessors (else the first FP/SIMD instruction faults as UC_ERR_INSN_INVALID)
mu.reg_write(UC_ARM_REG_C1_C0_2, mu.reg_read(UC_ARM_REG_C1_C0_2) | (0xf<<20))
mu.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
mu.mem_write(STUB, b'\x1e\xff\x2f\xe1'*(STUB_SZ//4))   # every import stub = ARM `bx lr`

dynsym=elf.get_section_by_name('.dynsym')
syms=[(s.name,s['st_value'],s['st_shndx']) for s in dynsym.iter_symbols()]
stub_of={}; name_at={}; nxt=STUB
for i,(nm,val,sh) in enumerate(syms):
    if sh=='SHN_UNDEF' and nm: stub_of[i]=nxt; name_at[nxt]=nm; nxt+=4

def reloc(sec):
    for r in sec.iter_relocations():
        off=BASE+r['r_offset']; t=r['r_info_type']; si=r['r_info_sym']
        cur=struct.unpack("<I",mu.mem_read(off,4))[0]
        if t==R_ARM_RELATIVE: mu.mem_write(off,struct.pack("<I",(cur+BASE)&0xffffffff))
        elif t in (R_ARM_JUMP_SLOT,R_ARM_GLOB_DAT):
            nm,val,sh=syms[si]; a=stub_of.get(si,STUB) if sh=='SHN_UNDEF' else BASE+val
            mu.mem_write(off,struct.pack("<I",a&0xffffffff))
        elif t==R_ARM_ABS32:
            nm,val,sh=syms[si]; a=stub_of.get(si,0) if sh=='SHN_UNDEF' else BASE+val
            mu.mem_write(off,struct.pack("<I",(a+cur)&0xffffffff))
for n in ('.rel.dyn','.rel.plt'):
    s=elf.get_section_by_name(n)
    if isinstance(s,RelocationSection): reloc(s)

heap=[HEAP]; allocsz={}
def balloc(n): n=(n+15)&~15; p=heap[0]; heap[0]+=n; allocsz[p]=n; return p
def cstr(uc,p):
    o=0
    while uc.mem_read(p+o,1)!=b'\0': o+=1
    return bytes(uc.mem_read(p,o))
seen=set(); unimpl={}
import zipfile as _zip
_apk=_zip.ZipFile("/work/apks/com.rovio.angrybirds@8.0.3.apk")
_assets={n[7:]:n for n in _apk.namelist() if n.startswith("assets/")}
_oa={}; _ahc=[0x42000000]; glid=[0x1000]
GLSTR=balloc(32); mu.mem_write(GLSTR, b"OpenGL ES 2.0\x00")
def bridge(nm,uc):
    r=lambda i:uc.reg_read(UC_ARM_REG_R0+i)
    a0,a1,a2=r(0),r(1),r(2)
    if nm in('malloc','valloc'): return balloc(max(a0,1))
    if nm=='calloc': p=balloc(max(a0*a1,1)); uc.mem_write(p,b'\0'*(a0*a1)); return p
    if nm=='realloc':
        np=balloc(max(a1,1))
        if a0 and a1: uc.mem_write(np,bytes(uc.mem_read(a0,min(a1,allocsz.get(a0,a1)))))
        return np
    if nm in('free','freea'): return 0
    if nm in('memcpy','memmove','__aeabi_memcpy','__aeabi_memcpy4','__aeabi_memcpy8','__aeabi_memmove','__aeabi_memmove4','__aeabi_memmove8'):
        uc.mem_write(a0,bytes(uc.mem_read(a1,a2))); return a0
    if nm in('memset','__aeabi_memset','__aeabi_memset4','__aeabi_memset8'):
        uc.mem_write(a0,bytes([a1&0xff])*a2); return a0
    if nm in('__aeabi_memclr','__aeabi_memclr4','__aeabi_memclr8'):
        uc.mem_write(a0,b'\0'*a1); return a0
    if nm=='memcmp':
        for x,y in zip(bytes(uc.mem_read(a0,a2)),bytes(uc.mem_read(a1,a2))):
            if x!=y: return (x-y)&0xffffffff
        return 0
    if nm=='memchr':
        i=bytes(uc.mem_read(a0,a2)).find(a1&0xff); return (a0+i) if i>=0 else 0
    if nm=='strlen': return len(cstr(uc,a0))
    if nm in('strcmp','strcoll'):
        b0,b1=cstr(uc,a0),cstr(uc,a1)
        for x,y in zip(b0,b1):
            if x!=y: return (x-y)&0xffffffff
        return (len(b0)-len(b1))&0xffffffff
    if nm=='strncmp':
        b0=bytes(uc.mem_read(a0,a2)).split(b'\0')[0]; b1=bytes(uc.mem_read(a1,a2)).split(b'\0')[0]
        for x,y in zip(b0,b1):
            if x!=y: return (x-y)&0xffffffff
        return 0
    if nm in('strcpy','stpcpy'):
        s=cstr(uc,a1); uc.mem_write(a0,s+b'\0'); return a0
    if nm=='strncpy':
        s=cstr(uc,a1)[:a2]; uc.mem_write(a0,s+b'\0'*(a2-len(s))); return a0
    if nm=='strchr':
        s=cstr(uc,a0); i=s.find(a1&0xff); return (a0+i) if i>=0 else 0
    if nm=='strdup':
        s=cstr(uc,a0); p=balloc(len(s)+1); uc.mem_write(p,s+b'\0'); return p
    if nm in('__android_log_print','__android_log_write','__android_log_vprint','__android_log_assert'):
        try: print("      [engine]", cstr(uc,(a2 if nm=='__android_log_print' else a1))[:120].decode('latin1'))
        except: pass
        return 0
    if nm in('printf','puts','fprintf','vprintf','fputs','fwrite'):
        try: print("      [engine]", cstr(uc,(a0 if nm in('printf','puts') else a1))[:120].decode('latin1'))
        except: pass
        return 0
    if nm in('__cxa_atexit','__cxa_finalize','atexit','__cxa_guard_release','__cxa_guard_abort','__register_atfork'): return 0
    if nm=='__cxa_guard_acquire': return 1
    if nm=='__errno': return HEAP+HEAP_SZ-0x100
    if nm in('pthread_once','pthread_mutex_lock','pthread_mutex_unlock','pthread_mutex_init','pthread_mutex_destroy','pthread_self','pthread_getspecific','pthread_setspecific','pthread_key_create'): return 0
    if nm in('sysconf','getpagesize'): return 4096
    if nm in('gettid','getpid','pthread_gettid_np'): return 1234
    if nm in('open','open64','openat'): return 3
    if nm in('read','pread','pread64','write'): return 0
    if nm in('close','lseek','lseek64','fsync','fcntl','flock','ioctl','fdatasync'): return 0
    if nm in('fstat','fstat64','stat','stat64','lstat','lstat64'):
        try: uc.mem_write(a1, b'\0'*128)
        except: pass
        return 0
    if nm=='getauxval': return (0x1000 if a0==16 else 0)
    if nm in('access','unlink','remove'): return 0xffffffff
    if nm in('getenv','getcwd','setlocale'): return 0
    if nm in('mmap','mmap64'): return balloc(max(a1 if a1 else 0x1000,0x1000))
    if nm in('munmap','madvise','mprotect','msync'): return 0
    if nm in('ceil','floor','fabs','sqrt','sin','cos','tan','exp','log','log10','round','trunc'):
        import math as _m
        d=struct.unpack('<d',struct.pack('<Q',(a0|(a1<<32))&0xffffffffffffffff))[0]
        fn={'ceil':_m.ceil,'floor':_m.floor,'fabs':abs,'sqrt':(lambda x:_m.sqrt(x) if x>=0 else float('nan')),
            'sin':_m.sin,'cos':_m.cos,'tan':_m.tan,'exp':_m.exp,
            'log':(lambda x:_m.log(x) if x>0 else float('-inf')),'log10':(lambda x:_m.log10(x) if x>0 else float('-inf')),
            'round':round,'trunc':_m.trunc}[nm]
        try: res=float(fn(d))
        except: res=0.0
        rb=struct.unpack('<Q',struct.pack('<d',res))[0]
        uc.reg_write(UC_ARM_REG_R0,rb&0xffffffff); uc.reg_write(UC_ARM_REG_R1,(rb>>32)&0xffffffff); return None
    if nm=='btowc': return a0
    if nm=='wctob': return a0 if a0<0x80 else 0xffffffff
    if nm in('wctype','iswctype'): return 1
    if nm in('toupper','tolower'):
        c=a0
        if nm=='toupper' and 97<=c<=122: c-=32
        if nm=='tolower' and 65<=c<=90: c+=32
        return c
    if nm in('sprintf','snprintf','vsprintf','vsnprintf'):
        if nm in('snprintf','vsnprintf'): buf=a0; fmt=cstr(uc,a2).decode('latin1'); start=3
        else: buf=a0; fmt=cstr(uc,a1).decode('latin1'); start=2
        sp=uc.reg_read(UC_ARM_REG_SP); box=[start]
        def na():
            k=box[0]; box[0]+=1
            return uc.reg_read(UC_ARM_REG_R0+k) if k<4 else struct.unpack("<I",uc.mem_read(sp+(k-4)*4,4))[0]
        out=b''; i=0
        while i<len(fmt):
            c=fmt[i]
            if c!='%': out+=c.encode('latin1'); i+=1; continue
            j=i+1
            while j<len(fmt) and fmt[j] not in 'diouxXeEfFgGcspn%': j+=1
            conv=fmt[j] if j<len(fmt) else '%'
            try:
                if conv=='%': out+=b'%'
                elif conv=='s': p=na(); out+=(cstr(uc,p) if p else b'(null)')
                elif conv in 'di': v=na(); v=(v+2**31)%2**32-2**31; out+=str(v).encode()
                elif conv in 'uxXo': out+=(('%'+conv)%na()).encode()
                elif conv=='c': out+=bytes([na()&0xff])
                elif conv=='p': out+=('0x%x'%na()).encode()
                elif conv in 'fFeEgG': lo=na(); hi=na(); out+=(('%'+conv)%struct.unpack('<d',struct.pack('<II',lo,hi))[0]).encode()
                else: out+=fmt[i:j+1].encode('latin1')
            except Exception: out+=b'?'
            i=j+1
        uc.mem_write(buf,out+b'\0'); return len(out)
    if nm=='strrchr':
        s=cstr(uc,a0); i=s.rfind(a1&0xff); return (a0+i) if i>=0 else 0
    if nm=='strstr':
        h=cstr(uc,a0); n2=cstr(uc,a1); i=h.find(n2); return (a0+i) if i>=0 else 0
    if nm in('strcat','strncat'):
        dst=cstr(uc,a0); s=cstr(uc,a1)
        if nm=='strncat': s=s[:a2]
        uc.mem_write(a0+len(dst), s+b'\0'); return a0
    if nm in('strtol','strtoul','atoi','atol','strtoll'):
        import re as _re; m=_re.match(rb'\s*[+-]?\d+', cstr(uc,a0))
        return (int(m.group())&0xffffffff) if m else 0
    if nm in('strcasecmp','strncasecmp'):
        b0=cstr(uc,a0).lower(); b1=cstr(uc,a1).lower()
        if nm=='strncasecmp': b0=b0[:a2]; b1=b1[:a2]
        return 0 if b0==b1 else (1 if b0>b1 else 0xffffffff)
    if nm in('__gnu_Unwind_Find_exidx','dl_unwind_find_exidx','__cxa_begin_catch','__cxa_end_catch'): return 0
    if nm in('abort','__assert2','__assert','__stack_chk_fail'):
        print(f"      [engine] {nm}() from lr={uc.reg_read(UC_ARM_REG_LR):#x} r0={uc.reg_read(UC_ARM_REG_R0):#x}")
        uc.emu_stop(); return 0
    if nm=='AAssetManager_fromJava': return 0x41000000
    if nm=='AAssetManager_open':
        n=_assets.get(cstr(uc,a1).decode('latin1'))
        if n is None: return 0
        _ahc[0]+=8; _oa[_ahc[0]]=[_apk.read(n),0,None]; return _ahc[0]
    if nm=='AAsset_read':
        st=_oa.get(a0)
        if not st: return 0xffffffff
        ch=st[0][st[1]:st[1]+a2]; uc.mem_write(a1,ch); st[1]+=len(ch); return len(ch)
    if nm in('AAsset_getLength','AAsset_getLength64'):
        st=_oa.get(a0); return len(st[0]) if st else 0
    if nm in('AAsset_getRemainingLength','AAsset_getRemainingLength64'):
        st=_oa.get(a0); return (len(st[0])-st[1]) if st else 0
    if nm in('AAsset_seek','AAsset_seek64'):
        st=_oa.get(a0)
        if not st: return 0xffffffff
        st[1]=(a1 if a2==0 else (st[1]+a1 if a2==1 else len(st[0])+a1)); return st[1]
    if nm=='AAsset_getBuffer':
        st=_oa.get(a0)
        if not st: return 0
        if st[2] is None: st[2]=balloc(len(st[0])+16); uc.mem_write(st[2],st[0])
        return st[2]
    if nm=='AAsset_close': _oa.pop(a0,None); return 0
    if nm.startswith('gl'):
        if nm in('glCreateProgram','glCreateShader'): glid[0]+=1; return glid[0]
        if nm in('glGenTextures','glGenBuffers','glGenFramebuffers','glGenRenderbuffers','glGenVertexArraysOES'):
            for k in range(a0): glid[0]+=1; uc.mem_write(a1+k*4,struct.pack('<I',glid[0]))
            return None
        if nm=='glGetError': return 0
        if nm=='glGetString': return GLSTR
        if nm in('glGetShaderiv','glGetProgramiv'): uc.mem_write(a2,struct.pack('<I',1)); return None
        if nm in('glGetIntegerv','glGetFloatv','glGetBooleanv'):
            try: uc.mem_write(a1,b'\0'*16)
            except: pass
            return None
        if nm in('glGetUniformLocation','glGetAttribLocation'): glid[0]+=1; return glid[0]
        if nm in('glGetShaderInfoLog','glGetProgramInfoLog','glGetActiveUniform','glGetActiveAttrib'): return None
        return 0
    unimpl[nm]=unimpl.get(nm,0)+1; return 0
def stub_cb(uc,addr,size,user):
    nm=name_at.get(addr,"?%x"%addr); seen.add(nm)
    ret=bridge(nm,uc)
    if ret is not None: uc.reg_write(UC_ARM_REG_R0, ret&0xffffffff)
    # stub slot holds `bx lr` (filled below); unicorn executes it for native ARM/Thumb interworking
mu.hook_add(UC_HOOK_CODE, stub_cb, None, STUB, STUB+STUB_SZ)

# ARM kuser helper page: kernel-provided helpers at the fixed high address 0xffff0000
KUSER=0xffff0000; mu.mem_map(KUSER,0x1000)
mu.mem_write(KUSER, b'\x1e\xff\x2f\xe1'*(0x1000//4))   # `bx lr` filler for native kuser returns
TLS=balloc(0x1000)
def kuser_cb(uc,addr,size,user):
    lr=uc.reg_read(UC_ARM_REG_LR)
    if addr==0xffff0fe0:      # __kuser_get_tls
        uc.reg_write(UC_ARM_REG_R0, TLS)
    elif addr==0xffff0fc0:    # __kuser_cmpxchg(oldval, newval, ptr) -> 0 if swapped
        old=uc.reg_read(UC_ARM_REG_R0); new=uc.reg_read(UC_ARM_REG_R1); ptr=uc.reg_read(UC_ARM_REG_R2)
        cur=struct.unpack("<I",uc.mem_read(ptr,4))[0]
        if cur==old: uc.mem_write(ptr,struct.pack("<I",new&0xffffffff)); uc.reg_write(UC_ARM_REG_R0,0)
        else: uc.reg_write(UC_ARM_REG_R0,1)
    elif addr==0xffff0f60:    # __kuser_cmpxchg64(oldp, newp, ptr)
        oldp=uc.reg_read(UC_ARM_REG_R0); newp=uc.reg_read(UC_ARM_REG_R1); ptr=uc.reg_read(UC_ARM_REG_R2)
        if bytes(uc.mem_read(ptr,8))==bytes(uc.mem_read(oldp,8)):
            uc.mem_write(ptr,bytes(uc.mem_read(newp,8))); uc.reg_write(UC_ARM_REG_R0,0)
        else: uc.reg_write(UC_ARM_REG_R0,1)
    # 0xffff0fa0 __kuser_memory_barrier: no-op; page holds `bx lr` for native return
mu.hook_add(UC_HOOK_CODE, kuser_cb, None, KUSER, KUSER+0x1000)

# SVC syscall handler (bionic makes some inline syscalls; EABI syscall nr in r7)
syscalls={}
def intr_cb(uc, intno, user):
    nr=uc.reg_read(UC_ARM_REG_R7); syscalls[nr]=syscalls.get(nr,0)+1
    ret=0
    if nr in (20,224,178): ret=1234          # getpid/gettid
    elif nr==263 or nr==78:                   # clock_gettime / gettimeofday: zero the timespec at r1
        p=uc.reg_read(UC_ARM_REG_R1)
        try: uc.mem_write(p, b'\0'*16)
        except: pass
    uc.reg_write(UC_ARM_REG_R0, ret&0xffffffff)
mu.hook_add(UC_HOOK_INTR, intr_cb)
faults=[]
mu.hook_add(UC_HOOK_MEM_UNMAPPED, lambda uc,acc,addr,sz,val,u:(faults.append((acc,addr,uc.reg_read(UC_ARM_REG_PC))),False)[1])

def call(addr,args=(),tag=""):
    mu.reg_write(UC_ARM_REG_SP, STACK+STACK_SZ-0x1000)
    for i,x in enumerate(args[:4]): mu.reg_write(UC_ARM_REG_R0+i,x)
    mu.reg_write(UC_ARM_REG_LR, RET)
    try:
        mu.emu_start(addr, RET, count=8_000_000); return mu.reg_read(UC_ARM_REG_R0)  # LSB of addr selects ARM/Thumb
    except UcError as e:
        pc=mu.reg_read(UC_ARM_REG_PC)
        try:
            import capstone as _cs
            thumb=(mu.reg_read(UC_ARM_REG_CPSR)&0x20)!=0
            md=_cs.Cs(_cs.CS_ARCH_ARM, _cs.CS_MODE_THUMB if thumb else _cs.CS_MODE_ARM)
            ins=next(md.disasm(bytes(mu.mem_read(pc&~1,4)), pc&~1), None)
            dis=(ins.mnemonic+' '+ins.op_str) if ins else bytes(mu.mem_read(pc&~1,4)).hex()
        except Exception: dis='?'
        print(f"      [{tag}] {e} pc={pc:#x} insn=[{dis}]"); return None

print(f"[+] image {image_sz:#x} @ {BASE:#x}; {len(stub_of)} import stubs; relocs applied")
ia=elf.get_section_by_name('.init_array')
raw=bytes(mu.mem_read(BASE+ia['sh_addr'], ia['sh_size']))
ctors=[struct.unpack_from("<I",raw,o)[0] for o in range(0,len(raw),4)]
print(f"[+] {len(ctors)} constructors")
ok=0; failed=[]
for i,c in enumerate(ctors):
    if c in (0,0xffffffff): continue
    if call(c,tag=f"ctor{i}@{c:#x}") is None: failed.append(i)
    else: ok+=1
print(f"[=] {ok}/{len(ctors)} constructors ran CLEAN; {len(failed)} failed: {failed}")
print("[=] imports fired:", len(seen), sorted(seen))
print("[=] UNIMPLEMENTED bridges hit:", sorted(unimpl.items(), key=lambda x:-x[1]))
if faults: print("[!] mem faults:", [(a,hex(b),hex(c)) for a,b,c in faults[:10]])
if failed:
    fi=failed[0]; c=ctors[fi]; hist=[]
    print(f"[trace] re-running ctor{fi} @ {c:#x} for a mode trace")
    def tr(uc,address,size,user):
        hist.append((address,(uc.reg_read(UC_ARM_REG_CPSR)>>5)&1,size))
        if len(hist)>80: hist.pop(0)
    h=mu.hook_add(UC_HOOK_CODE, tr)
    call(c, tag=f"trace{fi}"); mu.hook_del(h)
    print("[trace] last 24 executed (pc / thumb / size):")
    for pc,t,sz in hist[-24:]: print(f"    {pc:#010x}  T={t}  sz={sz}")

# ---------- JNIEnv / JavaVM bridge + JNI entry attempt ----------
print("\n[JNI] building fake JavaVM + JNIEnv function tables")
JNISTUB=0x11000000; mu.mem_map(JNISTUB,0x10000)
mu.mem_write(JNISTUB, b'\x1e\xff\x2f\xe1'*(0x10000//4))
ENVTAB=balloc(300*4)
for i in range(300): mu.mem_write(ENVTAB+i*4, struct.pack("<I", JNISTUB+i*4))
ENV=balloc(4); mu.mem_write(ENV, struct.pack("<I", ENVTAB))
VMTAB=balloc(16*4)
for i in range(16): mu.mem_write(VMTAB+i*4, struct.pack("<I", JNISTUB+0x4000+i*4))
VM=balloc(4); mu.mem_write(VM, struct.pack("<I", VMTAB))
STRBUF=balloc(0x1000)
hc=[0x30000000]
def newh(): hc[0]+=8; return hc[0]
jni_called={}
ENV_NAMES={4:'GetVersion',6:'FindClass',15:'ExceptionOccurred',17:'ExceptionClear',18:'FatalError',
 21:'NewGlobalRef',22:'DeleteGlobalRef',23:'DeleteLocalRef',24:'IsSameObject',25:'NewLocalRef',
 31:'GetObjectClass',32:'IsInstanceOf',33:'GetMethodID',113:'GetStaticMethodID',
 94:'GetFieldID',144:'GetStaticFieldID',215:'RegisterNatives',219:'GetJavaVM',228:'ExceptionCheck',
 167:'NewStringUTF',169:'GetStringUTFChars',170:'ReleaseStringUTFChars',171:'GetStringUTFLength'}
def jni_cb(uc,addr,size,user):
    off=addr-JNISTUB
    r1=uc.reg_read(UC_ARM_REG_R1)
    if off<0x4000:
        idx=off//4; nm=ENV_NAMES.get(idx,f'env#{idx}'); jni_called[nm]=jni_called.get(nm,0)+1
        if idx==4: v=0x10006
        elif idx==219: uc.mem_write(r1,struct.pack('<I',VM)); v=0
        elif idx in(215,): v=0
        elif idx in(228,15,17,22,23): v=0
        elif idx in(21,25,31,6,33,113,94,144,167): v=newh()
        elif idx==169: uc.mem_write(STRBUF,b'\0'); v=STRBUF
        elif idx==171: v=0
        else: v=0
    else:
        idx=(off-0x4000)//4; jni_called['VM#%d'%idx]=jni_called.get('VM#%d'%idx,0)+1
        if idx in(4,6,7): uc.mem_write(r1,struct.pack('<I',ENV)); v=0
        else: v=0
    uc.reg_write(UC_ARM_REG_R0, v&0xffffffff)
mu.hook_add(UC_HOOK_CODE, jni_cb, None, JNISTUB, JNISTUB+0x10000)

def sym_addr(name):
    for nm,val,sh in syms:
        if nm==name and sh!='SHN_UNDEF': return BASE+val
    return None
ol=sym_addr('JNI_OnLoad')
if ol:
    print(f"[JNI] JNI_OnLoad @ {ol:#x} ->", hex(call(ol, args=(VM,0), tag='JNI_OnLoad') or 0))
ni=sym_addr('Java_com_rovio_fusion_NativeApplication_nativeInit')
if ni:
    print(f"[JNI] calling nativeInit @ {ni:#x} (env, thiz, 0, 0)")
    r=call(ni, args=(ENV, newh(), 0, 0), tag='nativeInit')
    print(f"[JNI] nativeInit returned: {r}")
print("[JNI] engine called back into Java:", sorted(jni_called.items(), key=lambda x:-x[1])[:30])
print("[JNI] new UNIMPL libc during init:", sorted(unimpl.items(), key=lambda x:-x[1])[:20])
