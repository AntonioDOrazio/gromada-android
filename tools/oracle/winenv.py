"""Minimal Win32 environment for running the whole gromada.exe inside the oracle (issue #13).

The differential oracle (oracle.py) runs isolated functions. The game-level trace needs the
original code to load fw.res, a map, a script and a demo through its statically linked MSVC
CRT, so this module provides what that CRT and the game ask Windows for:

- a TEB (fs:[0] SEH chain, used by the CRT startup and by C++ functions with destructors);
- KERNEL32 heap (HeapCreate/HeapAlloc/HeapFree/HeapReAlloc/HeapSize) and VirtualAlloc on a
  deterministic allocator (same addresses at every run);
- KERNEL32 files (CreateFileA/ReadFile/WriteFile/SetFilePointer/CloseHandle/...): reads come
  from the game folder (case-insensitive paths, backslashes); writes never reach the game
  folder, they stay in memory;
- the CRT startup environment (command line, startup info, code pages, environment);
- a handful of USER32/WINMM/GDI-free answers (timeGetTime is driven by the caller).

Everything that is not listed here still traps with UnexpectedCall: a new dependency shows up
immediately instead of silently returning garbage.
"""
import os
import struct

from unicorn import UC_PROT_ALL
from unicorn.x86_const import (UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_FS, UC_X86_REG_GDTR,
                               UC_X86_REG_GS, UC_X86_REG_SS)

TEB_BASE = 0x7FFD0000
WINHEAP_BASE, WINHEAP_SIZE = 0x50000000, 0x10000000   # 256 MiB: HeapAlloc
VIRT_BASE, VIRT_SIZE = 0x60000000, 0x08000000         # 128 MiB: VirtualAlloc
ENV_BASE = 0x7FF00000                                 # strings handed to the CRT

INVALID_HANDLE = 0xFFFFFFFF
STD_HANDLES = {-10 & 0xFFFFFFFF: 0x11, -11 & 0xFFFFFFFF: 0x12, -12 & 0xFFFFFFFF: 0x13}
GAME_ROOT = "C:\\GROMADA\\"


def resolve_ci(root, rel):
    """Case-insensitive lookup of a relative path with \\ or / separators under root."""
    parts = [p for p in rel.replace("\\", "/").split("/") if p and p != "."]
    cur = root
    for p in parts:
        try:
            names = os.listdir(cur)
        except OSError:
            return None
        hit = next((n for n in names if n.lower() == p.lower()), None)
        if hit is None:
            return None
        cur = os.path.join(cur, hit)
    return cur


class Heap:
    """Deterministic first-fit-by-size allocator (exact size classes, LIFO reuse)."""

    def __init__(self, emu, base, size):
        self.emu, self.base, self.end, self.top = emu, base, base + size, base
        self.sizes = {}
        self.free = {}

    def alloc(self, size, zero=False):
        size = max(16, (size + 15) & ~15)
        lst = self.free.get(size)
        if lst:
            p = lst.pop()
            if zero:
                self.emu.write(p, b"\0" * size)
        else:
            p = self.top
            self.top += size
            if self.top > self.end:
                raise MemoryError("win32 heap exhausted")
        self.sizes[p] = size
        return p

    def release(self, p):
        size = self.sizes.pop(p, None)
        if size is None:
            return False
        self.free.setdefault(size, []).append(p)
        return True


class WinEnv:
    def __init__(self, o, game_dir, out_dir=None, log=None):
        self.o = o
        self.uc = o.uc
        self.game_dir = game_dir
        self.out_dir = out_dir
        self.log = log or (lambda s: None)
        uc = self.uc
        uc.mem_map(TEB_BASE, 0x2000, UC_PROT_ALL)
        uc.mem_map(WINHEAP_BASE, WINHEAP_SIZE, UC_PROT_ALL)
        uc.mem_map(VIRT_BASE, VIRT_SIZE, UC_PROT_ALL)
        uc.mem_map(ENV_BASE, 0x10000, UC_PROT_ALL)
        self._setup_teb()
        self.heap = Heap(o, WINHEAP_BASE, WINHEAP_SIZE)
        self.virt_top = VIRT_BASE
        self.files = {}           # handle -> dict(path, data(bytearray), pos, write)
        self.next_handle = 0x100
        self.written = {}         # name -> bytes of files the game wrote (never on disk)
        self.time_ms = 0x10000    # timeGetTime
        self.time_step = 0        # added after every timeGetTime call
        self.last_error = 0
        env = ENV_BASE
        self.cmdline = env
        o.write(env, b"C:\\GROMADA\\gromada.exe\0")
        self.envstr_a = env + 0x100
        o.write(self.envstr_a, b"PATH=C:\\\0\0")
        self.envstr_w = env + 0x200
        o.write(self.envstr_w, "PATH=C:\\\0\0".encode("utf-16-le"))
        self._install()

    # ---------------------------------------------------------------- TEB / fs
    def _setup_teb(self):
        o = self.o
        o.w32(TEB_BASE + 0x00, 0xFFFFFFFF)         # SEH chain end
        o.w32(TEB_BASE + 0x04, 0x20100000)         # stack base
        o.w32(TEB_BASE + 0x08, 0x20000000)         # stack limit
        o.w32(TEB_BASE + 0x18, TEB_BASE)           # self
        self._gdt_fs()   # FS_BASE is not honoured by unicorn in 32-bit mode: use a GDT entry

    def _gdt_fs(self):
        gdt = TEB_BASE + 0x1000

        def desc(base, limit, access, flags):
            d = limit & 0xFFFF
            d |= (base & 0xFFFFFF) << 16
            d |= access << 40
            d |= ((limit >> 16) & 0xF) << 48
            d |= flags << 52
            d |= ((base >> 24) & 0xFF) << 56
            return struct.pack("<Q", d)
        # Flat ring-0 code/data (loading GDTR makes unicorn reload every segment from it) and
        # an fs segment on the TEB.
        self.o.write(gdt, b"\0" * 8 * 8)
        self.o.write(gdt + 8 * 1, desc(0, 0xFFFFF, 0x9B, 0xC))
        self.o.write(gdt + 8 * 2, desc(0, 0xFFFFF, 0x93, 0xC))
        self.o.write(gdt + 8 * 3, desc(TEB_BASE, 0xFFF, 0x93, 0x4))
        self.uc.reg_write(UC_X86_REG_GDTR, (0, gdt, 8 * 8 - 1, 0))
        self.uc.reg_write(UC_X86_REG_CS, 1 * 8)
        for r in (UC_X86_REG_SS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_GS):
            self.uc.reg_write(r, 2 * 8)
        self.uc.reg_write(UC_X86_REG_FS, 3 * 8)

    # ---------------------------------------------------------------- helpers
    def cstr(self, p, n=512):
        if not p:
            return ""
        b = self.o.read(p, n)
        return b.split(b"\0")[0].decode("latin-1")

    def stub(self, name, nargs, fn):
        self.o.stub_import(name, lambda o, a: fn(a), 4 * nargs)

    def const(self, name, nargs, value):
        self.stub(name, nargs, lambda a: value)

    def _resolve(self, name):
        n = name
        if n.upper().startswith(GAME_ROOT):
            n = n[len(GAME_ROOT):]
        elif len(n) > 2 and n[1] == ":":
            return None
        return resolve_ci(self.game_dir, n)

    # ---------------------------------------------------------------- imports
    def _install(self):
        k = "KERNEL32.dll!"
        st = self.stub
        # process / CRT startup
        self.const(k + "GetVersion", 0, 0x0A280105)          # Windows XP-ish, build 2600
        st(k + "GetCommandLineA", 0, lambda a: self.cmdline)
        st(k + "GetStartupInfoA", 1, self._startup_info)
        st(k + "GetStdHandle", 1, lambda a: STD_HANDLES.get(a(0) & 0xFFFFFFFF, INVALID_HANDLE))
        st(k + "GetFileType", 1, lambda a: 2 if (a(0) & 0xFFFFFFFF) in STD_HANDLES.values() else 1)
        st(k + "SetHandleCount", 1, lambda a: a(0))
        st(k + "GetEnvironmentStringsW", 0, lambda a: self.envstr_w)
        st(k + "GetEnvironmentStrings", 0, lambda a: self.envstr_a)
        self.const(k + "FreeEnvironmentStringsW", 1, 1)
        self.const(k + "FreeEnvironmentStringsA", 1, 1)
        st(k + "GetModuleFileNameA", 3, self._module_file_name)
        self.const(k + "GetModuleHandleA", 1, 0x400000)
        self.const(k + "GetACP", 0, 1252)
        self.const(k + "GetOEMCP", 0, 866)
        st(k + "GetCPInfo", 2, self._cpinfo)
        st(k + "MultiByteToWideChar", 6, self._mb2wc)
        st(k + "WideCharToMultiByte", 8, self._wc2mb)
        st(k + "GetStringTypeW", 4, self._string_type_w)
        st(k + "GetStringTypeA", 5, self._string_type_a)
        st(k + "LCMapStringW", 6, self._lcmap_w)
        st(k + "LCMapStringA", 6, self._lcmap_a)
        st(k + "GetLastError", 0, lambda a: self.last_error)
        st(k + "GetSystemTimeAsFileTime", 1, lambda a: self.o.write(a(0), b"\0" * 8))
        self.const(k + "GetCurrentProcess", 0, 0xFFFFFFFF)
        st(k + "UnhandledExceptionFilter", 1, self._fatal("UnhandledExceptionFilter"))
        st(k + "TerminateProcess", 2, self._fatal("TerminateProcess"))
        st(k + "ExitProcess", 1, self._fatal("ExitProcess"))
        st(k + "RaiseException", 4, self._fatal("RaiseException"))
        # heap
        st(k + "HeapCreate", 3, lambda a: 0x00DE0000)
        self.const(k + "HeapDestroy", 1, 1)
        st(k + "HeapAlloc", 3, lambda a: self.heap.alloc(a(2) & 0xFFFFFFFF, zero=bool(a(1) & 8)))
        st(k + "HeapFree", 3, lambda a: 1 if self.heap.release(a(2) & 0xFFFFFFFF) or True else 0)
        st(k + "HeapReAlloc", 4, self._heap_realloc)
        st(k + "HeapSize", 3, lambda a: self.heap.sizes.get(a(2) & 0xFFFFFFFF, 0))
        st(k + "VirtualAlloc", 4, self._virtual_alloc)
        self.const(k + "VirtualFree", 3, 1)
        # files
        st(k + "CreateFileA", 7, self._create_file)
        st(k + "ReadFile", 5, self._read_file)
        st(k + "WriteFile", 5, self._write_file)
        st(k + "SetFilePointer", 4, self._set_file_pointer)
        st(k + "CloseHandle", 1, self._close)
        self.const(k + "FlushFileBuffers", 1, 1)
        st(k + "SetEndOfFile", 1, self._set_eof)
        self.const(k + "SetStdHandle", 2, 1)
        st(k + "GetProcAddress", 2, self._get_proc_address)
        st(k + "LoadLibraryA", 1, lambda a: self.log(f"LoadLibraryA({self.cstr(a(0) & 0xFFFFFFFF)!r}) -> 0") or 0)
        # time
        st("WINMM.dll!timeGetTime", 0, self._time)

    def _fatal(self, name):
        def cb(a):
            raise RuntimeError(f"{name} called by the emulated program")
        return cb

    def _get_proc_address(self, a):
        name = a(1) & 0xFFFFFFFF
        self.log(f"GetProcAddress({a(0) & 0xFFFFFFFF:#x}, {self.cstr(name) if name > 0xFFFF else name!r}) -> 0")
        return 0

    def _startup_info(self, a):
        p = a(0) & 0xFFFFFFFF
        self.o.write(p, struct.pack("<I", 68) + b"\0" * 64)
        return 0

    def _module_file_name(self, a):
        buf, size = a(1) & 0xFFFFFFFF, a(2)
        s = b"C:\\GROMADA\\gromada.exe"
        n = min(len(s), size - 1)
        self.o.write(buf, s[:n] + b"\0")
        return n

    def _cpinfo(self, a):
        p = a(1) & 0xFFFFFFFF
        self.o.write(p, struct.pack("<I", 1) + b"?\0" + b"\0" * 12)
        return 1

    def _mb2wc(self, a):
        src, n, dst, cap = a(2) & 0xFFFFFFFF, a(3), a(4) & 0xFFFFFFFF, a(5)
        if n == -1:
            data = self.o.read(src, 4096).split(b"\0")[0] + b"\0"
        else:
            data = self.o.read(src, n)
        if cap == 0:
            return len(data)
        out = data.decode("latin-1").encode("utf-16-le")[: 2 * cap]
        self.o.write(dst, out)
        return len(out) // 2

    def _wc2mb(self, a):
        src, n, dst, cap = a(2) & 0xFFFFFFFF, a(3), a(4) & 0xFFFFFFFF, a(5)
        if n == -1:
            raw = self.o.read(src, 8192)
            k = 0
            while raw[k:k + 2] != b"\0\0":
                k += 2
            data = raw[:k + 2]
        else:
            data = self.o.read(src, 2 * n)
        txt = data.decode("utf-16-le").encode("latin-1", "replace")
        if cap == 0:
            return len(txt)
        self.o.write(dst, txt[:cap])
        return min(len(txt), cap)

    def _string_type_w(self, a):
        src, n, out = a(1) & 0xFFFFFFFF, a(2), a(3) & 0xFFFFFFFF
        return self._ctype(self.o.read(src, 2 * max(n, 0)).decode("utf-16-le", "replace"), out)

    def _string_type_a(self, a):
        src, n, out = a(2) & 0xFFFFFFFF, a(3), a(4) & 0xFFFFFFFF
        return self._ctype(self.o.read(src, max(n, 0)).decode("latin-1"), out)

    def _ctype(self, s, out):
        r = b""
        for ch in s:
            c = ord(ch)
            t = 0
            if ch.isupper():
                t |= 0x1
            if ch.islower():
                t |= 0x2
            if ch.isdigit():
                t |= 0x4
            if ch in " \t\r\n\v\f":
                t |= 0x8
            if 0x21 <= c < 0x7F and not ch.isalnum():
                t |= 0x10
            if c < 0x20 or c == 0x7F:
                t |= 0x20
            if ch == " ":
                t |= 0x40
            if ch.isalpha():
                t |= 0x100
            r += struct.pack("<H", t)
        self.o.write(out, r)
        return 1

    def _lcmap_w(self, a):
        flags, src, n, dst, cap = a(1), a(2) & 0xFFFFFFFF, a(3), a(4) & 0xFFFFFFFF, a(5)
        s = self.o.read(src, 2 * n).decode("utf-16-le", "replace")
        s = s.upper() if flags & 0x200 else s.lower() if flags & 0x100 else s
        if cap == 0:
            return len(s)
        self.o.write(dst, s.encode("utf-16-le")[: 2 * cap])
        return len(s)

    def _lcmap_a(self, a):
        flags, src, n, dst, cap = a(1), a(2) & 0xFFFFFFFF, a(3), a(4) & 0xFFFFFFFF, a(5)
        s = self.o.read(src, n).decode("latin-1")
        s = s.upper() if flags & 0x200 else s.lower() if flags & 0x100 else s
        if cap == 0:
            return len(s)
        self.o.write(dst, s.encode("latin-1", "replace")[:cap])
        return len(s)

    def _heap_realloc(self, a):
        old, size = a(2) & 0xFFFFFFFF, a(3) & 0xFFFFFFFF
        osize = self.heap.sizes.get(old, 0)
        p = self.heap.alloc(size, zero=bool(a(1) & 8))
        if old:
            self.o.write(p, self.o.read(old, min(osize, size)))
            self.heap.release(old)
        return p

    def _virtual_alloc(self, a):
        addr, size, typ = a(0) & 0xFFFFFFFF, a(1) & 0xFFFFFFFF, a(2) & 0xFFFFFFFF
        if addr:
            return addr       # commit inside a reserved region: already mapped
        size = (size + 0xFFFF) & ~0xFFFF
        p = self.virt_top
        self.virt_top += size
        if self.virt_top > VIRT_BASE + VIRT_SIZE:
            raise MemoryError("VirtualAlloc region exhausted")
        return p

    # ---------------------------------------------------------------- files
    def _create_file(self, a):
        name = self.cstr(a(0) & 0xFFFFFFFF)
        access, disp = a(1) & 0xFFFFFFFF, a(4)
        write = bool(access & 0x40000000)
        path = self._resolve(name)
        key = name.replace("/", "\\").lower()
        if write:
            if disp in (2, 1) or path is None or key in self.written:     # CREATE_ALWAYS/CREATE_NEW
                data = bytearray(self.written.get(key, b"")) if disp not in (2, 1) else bytearray()
            else:
                with open(path, "rb") as f:
                    data = bytearray(f.read())
            self.log(f"CreateFileA({name!r}, write) -> memory")
        else:
            if key in self.written:
                data = bytearray(self.written[key])
            elif path is None or not os.path.isfile(path):
                self.last_error = 2
                self.log(f"CreateFileA({name!r}) -> not found")
                return INVALID_HANDLE
            else:
                with open(path, "rb") as f:
                    data = bytearray(f.read())
        h = self.next_handle
        self.next_handle += 4
        self.files[h] = {"name": name, "key": key, "data": data, "pos": 0, "write": write}
        self.log(f"CreateFileA({name!r}) -> {h:#x}")
        return h

    def _read_file(self, a):
        h, buf, n, nread = a(0) & 0xFFFFFFFF, a(1) & 0xFFFFFFFF, a(2) & 0xFFFFFFFF, a(3) & 0xFFFFFFFF
        f = self.files.get(h)
        if f is None:
            if nread:
                self.o.w32(nread, 0)
            return 0
        chunk = bytes(f["data"][f["pos"]:f["pos"] + n])
        f["pos"] += len(chunk)
        self.o.write(buf, chunk)
        if nread:
            self.o.w32(nread, len(chunk))
        return 1

    def _write_file(self, a):
        h, buf, n, nw = a(0) & 0xFFFFFFFF, a(1) & 0xFFFFFFFF, a(2) & 0xFFFFFFFF, a(3) & 0xFFFFFFFF
        data = self.o.read(buf, n)
        f = self.files.get(h)
        if f is not None:
            p = f["pos"]
            d = f["data"]
            if len(d) < p:
                d.extend(b"\0" * (p - len(d)))
            d[p:p + n] = data
            f["pos"] = p + n
            self.written[f["key"]] = bytes(d)
        elif h in STD_HANDLES.values():
            self.log("stdout: " + data.decode("latin-1").rstrip())
        if nw:
            self.o.w32(nw, n)
        return 1

    def _set_file_pointer(self, a):
        h, dist, method = a(0) & 0xFFFFFFFF, a(1), a(3)
        f = self.files.get(h)
        if f is None:
            return 0xFFFFFFFF
        base = {0: 0, 1: f["pos"], 2: len(f["data"])}.get(method)
        if base is None:
            self.last_error = 87
            return 0xFFFFFFFF
        f["pos"] = max(0, base + dist)
        return f["pos"]

    def _set_eof(self, a):
        f = self.files.get(a(0) & 0xFFFFFFFF)
        if f is not None:
            del f["data"][f["pos"]:]
            self.written[f["key"]] = bytes(f["data"])
        return 1

    def _close(self, a):
        self.files.pop(a(0) & 0xFFFFFFFF, None)
        return 1

    def _time(self, a):
        t = self.time_ms
        self.time_ms = (self.time_ms + self.time_step) & 0xFFFFFFFF
        return t
