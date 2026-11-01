#!/usr/bin/env python3
"""Differential oracle harness: runs functions of the original gromada.exe inside unicorn.

The PE image is mapped at its image base (0x400000) exactly like the Windows loader would
(sections at their virtual addresses, .bss zero filled). Nothing is relocated, imports are
not resolved: every IAT slot points to a trap that raises `UnexpectedCall` unless a Python
handler was registered for that import. The same mechanism replaces internal functions
(`stub(addr, ...)`), which is how side effects such as ChangeAction or virtual Action calls
are recorded instead of executed.

Typical use (see the gen_*.py generators):

    o = Oracle()                                  # $GROMADA_EXE or $GROMADA_DATA/gromada.exe
    o.call(0x40b030, [x1, y1, x2, y2])            # cdecl, returns eax (signed 32)
    o.call(0x40aff0, [other], this=sprite, conv="thiscall")
    o.run(0x409cae, ends=[0x40a116], regs={...})  # run a code range with given registers

Calling conventions: "cdecl" (caller pops), "stdcall" (callee pops) and "thiscall" (MSVC:
`this` in ecx, callee pops). The harness checks that the callee popped what the convention
says, which detects a wrong prototype immediately.

Never commit the executable, dumps of its bytes or game data: the generators only write
numbers (inputs and outputs of the emulated functions) to tests/data/oracle/*.json.
"""
import os
import struct

import pefile
from unicorn import (UC_ARCH_X86, UC_HOOK_CODE, UC_HOOK_INTR, UC_HOOK_MEM_INVALID, UC_MODE_32,
                     UC_PROT_ALL, Uc, UcError)
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_EBP, UC_X86_REG_EBX, UC_X86_REG_ECX,
                               UC_X86_REG_EDI, UC_X86_REG_EDX, UC_X86_REG_EIP, UC_X86_REG_ESI,
                               UC_X86_REG_ESP)

REGS = {"eax": UC_X86_REG_EAX, "ebx": UC_X86_REG_EBX, "ecx": UC_X86_REG_ECX, "edx": UC_X86_REG_EDX,
        "esi": UC_X86_REG_ESI, "edi": UC_X86_REG_EDI, "ebp": UC_X86_REG_EBP, "esp": UC_X86_REG_ESP}

HEAP_BASE, HEAP_SIZE = 0x10000000, 0x01000000
STACK_BASE, STACK_SIZE = 0x20000000, 0x00100000
STUB_BASE, STUB_SIZE = 0x30000000, 0x00010000
RETURN_MAGIC = STUB_BASE + STUB_SIZE - 0x10   # return address of every top-level call
MAX_INSNS = 2_000_000                          # guard against runaway emulation


def exe_path():
    if os.environ.get("GROMADA_EXE"):
        return os.environ["GROMADA_EXE"]
    return os.path.join(os.environ.get("GROMADA_DATA", "/root/gromada-data/game"), "gromada.exe")


def s8(v):
    v &= 0xFF
    return v - 0x100 if v & 0x80 else v


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def s32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


class UnexpectedCall(Exception):
    pass


class CpuFault(Exception):
    """The emulated code raised a CPU exception (e.g. #DE divide error = vector 0)."""

    def __init__(self, vector, eip):
        super().__init__(f"cpu exception {vector} at {eip:#x}")
        self.vector = vector
        self.eip = eip


class Oracle:
    def __init__(self, path=None):
        self.path = path or exe_path()
        if not os.path.exists(self.path):
            raise SystemExit(f"gromada.exe not found ({self.path}): set GROMADA_DATA or GROMADA_EXE")
        self.pe = pefile.PE(self.path)
        if self.pe.FILE_HEADER.Machine != 0x14C:
            raise SystemExit("not a PE32 i386 image")
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.image_size = (self.pe.OPTIONAL_HEADER.SizeOfImage + 0xFFF) & ~0xFFF
        self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        self.uc.mem_map(self.base, self.image_size, UC_PROT_ALL)
        self.uc.mem_map(HEAP_BASE, HEAP_SIZE, UC_PROT_ALL)
        self.uc.mem_map(STACK_BASE, STACK_SIZE, UC_PROT_ALL)
        self.uc.mem_map(STUB_BASE, STUB_SIZE, UC_PROT_ALL)
        self.uc.mem_write(STUB_BASE, b"\xc3" * STUB_SIZE)  # never executed: hooks redirect first
        self.pristine = {}
        for s in self.pe.sections:
            va = self.base + s.VirtualAddress
            data = s.get_data()[: max(s.Misc_VirtualSize, 0)] if s.Misc_VirtualSize else s.get_data()
            size = max(s.Misc_VirtualSize, s.SizeOfRawData)
            blob = (data + b"\0" * size)[:size]
            self.pristine[s.Name.rstrip(b"\0").decode()] = (va, blob)
            self.uc.mem_write(va, blob)
        # Value of the registers a call or range does not set. Regenerating with another value
        # (ORACLE_GARBAGE=0) must give identical vectors: the ranges do not depend on them.
        self.garbage = int(os.environ.get("ORACLE_GARBAGE", "0xDEADBEEF"), 0)
        self.handlers = {}       # address -> (callback, popped bytes)
        self.next_stub = STUB_BASE
        self.fault = None
        self.heap_top = HEAP_BASE
        self.calls = []          # log of recorded stub calls (cleared by the generators)
        self._install_import_traps()
        self.uc.hook_add(UC_HOOK_INTR, self._on_intr)
        self.uc.hook_add(UC_HOOK_MEM_INVALID, self._on_bad_mem)
        # A CPU exception leaves QEMU's exception state set: the next one would become a
        # double fault. Restoring a clean CPU context after every fault avoids that.
        self.clean_cpu = self.uc.context_save()

    # ---- memory -------------------------------------------------------------------------
    def reset_data(self):
        """Restore .data/.bss to the values of the file (call between families)."""
        va, blob = self.pristine[".data"]
        self.uc.mem_write(va, blob)

    def heap_reset(self):
        self.heap_top = HEAP_BASE

    def alloc(self, size, align=16):
        self.heap_top = (self.heap_top + align - 1) & ~(align - 1)
        p = self.heap_top
        self.heap_top += size
        if self.heap_top > HEAP_BASE + HEAP_SIZE:
            raise MemoryError("oracle heap exhausted")
        self.uc.mem_write(p, b"\0" * size)
        return p

    def read(self, addr, size):
        return bytes(self.uc.mem_read(addr, size))

    def write(self, addr, data):
        self.uc.mem_write(addr, bytes(data))

    def u8(self, a):
        return self.read(a, 1)[0]

    def s8(self, a):
        return s8(self.u8(a))

    def u16(self, a):
        return struct.unpack("<H", self.read(a, 2))[0]

    def s16(self, a):
        return struct.unpack("<h", self.read(a, 2))[0]

    def u32(self, a):
        return struct.unpack("<I", self.read(a, 4))[0]

    def s32(self, a):
        return struct.unpack("<i", self.read(a, 4))[0]

    def w8(self, a, v):
        self.write(a, struct.pack("<B", v & 0xFF))

    def w16(self, a, v):
        self.write(a, struct.pack("<H", v & 0xFFFF))

    def w32(self, a, v):
        self.write(a, struct.pack("<I", v & 0xFFFFFFFF))

    # ---- stubs and traps ----------------------------------------------------------------
    def _install_import_traps(self):
        self.import_slots = {}
        for entry in self.pe.DIRECTORY_ENTRY_IMPORT:
            for imp in entry.imports:
                name = f"{entry.dll.decode()}!{imp.name.decode() if imp.name else imp.ordinal}"
                trap = self._new_stub_address()
                self.w32(imp.address, trap)
                self.import_slots[name] = trap
                self.handlers[trap] = (self._unexpected(name), 0)
                self.uc.hook_add(UC_HOOK_CODE, self._on_stub, begin=trap, end=trap)

    def _unexpected(self, name):
        def cb(o, args):
            raise UnexpectedCall(f"call to {name} (not stubbed)")
        return cb

    def _new_stub_address(self):
        a = self.next_stub
        self.next_stub += 4
        if self.next_stub >= RETURN_MAGIC:
            raise MemoryError("out of stub addresses")
        return a

    def stub(self, addr, callback, popped=0):
        """Replace the function at `addr` (internal code or a new stub address) by `callback`.

        `callback(oracle, args)` receives a lazy argument reader (`args(i)` = i-th dword on the
        stack after the return address; ecx is available as `oracle.reg('ecx')`) and returns
        the value for eax (None = 0). `popped` = bytes removed by the callee (`ret n`).
        """
        new = addr not in self.handlers
        self.handlers[addr] = (callback, popped)
        if new:
            self.uc.hook_add(UC_HOOK_CODE, self._on_stub, begin=addr, end=addr)
        return addr

    def new_stub(self, callback, popped=0):
        """Allocate a fresh code address handled by `callback` (vtable slots, function pointers)."""
        return self.stub(self._new_stub_address(), callback, popped)

    def stub_import(self, name, callback, popped):
        self.handlers[self.import_slots[name]] = (callback, popped)

    def unstub(self, addr):
        if addr in self.handlers:
            self.handlers[addr] = (None, 0)

    def clear_internal_stubs(self):
        """Let every replaced function of the image run its original code again."""
        for a in list(self.handlers):
            if self.base <= a < self.base + self.image_size:
                self.handlers[a] = (None, 0)

    def _on_stub(self, uc, address, size, user):
        cb, popped = self.handlers.get(address, (None, 0))
        if cb is None:
            return  # unstubbed internal function: run the original code
        esp = uc.reg_read(UC_X86_REG_ESP)

        def args(i):
            return s32(self.u32(esp + 4 + 4 * i))

        try:
            ret = cb(self, args)
        except Exception as e:  # propagate after stopping cleanly
            self.fault = e
            uc.emu_stop()
            return
        ret_addr = self.u32(esp)
        if ret is not None:
            uc.reg_write(UC_X86_REG_EAX, ret & 0xFFFFFFFF)
        uc.reg_write(UC_X86_REG_ESP, esp + 4 + popped)
        uc.reg_write(UC_X86_REG_EIP, ret_addr)

    def _on_intr(self, uc, intno, user):
        self.fault = CpuFault(intno, uc.reg_read(UC_X86_REG_EIP))
        uc.emu_stop()

    def _on_bad_mem(self, uc, access, address, size, value, user):
        self.fault = RuntimeError(f"invalid memory access at {address:#x} (eip {uc.reg_read(UC_X86_REG_EIP):#x})")
        return False

    # ---- execution ----------------------------------------------------------------------
    def reg(self, name):
        return self.uc.reg_read(REGS[name])

    def set_reg(self, name, v):
        self.uc.reg_write(REGS[name], v & 0xFFFFFFFF)

    def _start(self, begin, until):
        self.fault = None
        try:
            self.uc.emu_start(begin, until, count=MAX_INSNS)
        except UcError as e:
            if self.fault is None:
                self.fault = RuntimeError(f"unicorn: {e} at eip {self.uc.reg_read(UC_X86_REG_EIP):#x}")
        if self.fault is not None:
            f, self.fault = self.fault, None
            self.uc.context_restore(self.clean_cpu)
            raise f
        eip = self.uc.reg_read(UC_X86_REG_EIP)
        return eip

    def call(self, addr, args=(), conv="cdecl", this=None, regs=None):
        """Call the function at `addr`; returns eax as a signed 32-bit integer."""
        sp = STACK_BASE + STACK_SIZE - 0x1000
        for a in reversed(list(args)):
            sp -= 4
            self.w32(sp, a)
        sp -= 4
        self.w32(sp, RETURN_MAGIC)
        for r in ("eax", "ebx", "ecx", "edx", "esi", "edi", "ebp"):
            self.set_reg(r, self.garbage)  # make reads of uninitialised registers obvious
        for k, v in (regs or {}).items():
            self.set_reg(k, v)
        if conv == "thiscall":
            self.set_reg("ecx", this)
        self.set_reg("esp", sp)
        eip = self._start(addr, RETURN_MAGIC)
        if eip != RETURN_MAGIC:
            raise RuntimeError(f"emulation stopped at {eip:#x} (instruction limit?)")
        popped = self.reg("esp") - (sp + 4)
        expected = 0 if conv == "cdecl" else 4 * len(args)
        if popped != expected:
            raise RuntimeError(f"{addr:#x}: callee popped {popped} bytes, {conv} expects {expected}")
        return s32(self.reg("eax"))

    def run(self, begin, ends, regs=None, stack_words=64):
        """Run code from `begin` until one of `ends` is reached; returns the end address hit.

        A scratch stack frame of `stack_words` dwords is provided at esp (locals of the
        enclosing function); `self.frame` holds its address for reading locals afterwards.
        """
        sp = STACK_BASE + STACK_SIZE - 0x2000
        self.frame = sp
        self.write(sp, b"\xcc" * (4 * stack_words))  # sentinel pattern for uninitialised locals
        for r in ("eax", "ebx", "ecx", "edx", "esi", "edi", "ebp"):
            self.set_reg(r, self.garbage)
        for k, v in (regs or {}).items():
            self.set_reg(k, v)
        self.set_reg("esp", sp)
        hit = []

        def stop(uc, address, size, user):
            hit.append(address)
            uc.emu_stop()

        hooks = [self.uc.hook_add(UC_HOOK_CODE, stop, begin=e, end=e) for e in ends]
        try:
            self._start(begin, RETURN_MAGIC)
        finally:
            for h in hooks:
                self.uc.hook_del(h)
        if not hit:
            raise RuntimeError(f"run {begin:#x}: no end address reached")
        return hit[0]

    # ---- recording helpers --------------------------------------------------------------
    def recorder(self, tag, nargs, popped=None, ret=0, this=True):
        """A stub callback that appends (tag, this, args...) to self.calls and returns `ret`."""
        def cb(o, args):
            entry = [tag] + ([o.reg("ecx")] if this else []) + [args(i) for i in range(nargs)]
            o.calls.append(entry)
            return ret(o, entry) if callable(ret) else ret
        return cb
