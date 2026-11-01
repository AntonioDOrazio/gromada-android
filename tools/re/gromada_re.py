#!/usr/bin/env python3
"""Reverse-engineering helper for gromada.exe (Buka, 1999) based on angr + capstone.

Ghidra could not be downloaded in the analysis environment, so this script provides the
subset of features used to write docs/re/engine.md: function list, disassembly, angr
decompilation, cross references (CFG xrefs + raw operand scan) and string lookup.

Usage (the exe path comes from $GROMADA_EXE or $GROMADA_DATA/gromada.exe):
    python3 -I tools/re/gromada_re.py <command> [args] [; <command> [args] ...]

Commands (several can be chained with ';' to reuse the CFG, which takes a few seconds):
    funcs                     list functions: address, size, name
    dis ADDR [N]              disassemble the function containing ADDR (or N insns from ADDR)
    dec ADDR                  decompile the function at ADDR with angr
    xrefs ADDR                instructions referencing ADDR (immediate, displacement, call/jmp)
    callers ADDR              functions calling the function at ADDR
    callees ADDR              functions called by the function at ADDR
    str TEXT                  addresses of strings containing TEXT and the code using them
    imm VALUE                 instructions whose immediate/displacement equals VALUE
    jt ADDR [N]               dump N dwords of a jump/pointer table at ADDR

Never commit the executable or full listings produced by this script.
"""
import logging
import os
import sys

logging.getLogger("angr").setLevel(logging.CRITICAL)
logging.getLogger("cle").setLevel(logging.CRITICAL)
logging.getLogger("pyvex").setLevel(logging.CRITICAL)

import angr  # noqa: E402
import capstone  # noqa: E402
import pefile  # noqa: E402


def exe_path():
    if os.environ.get("GROMADA_EXE"):
        return os.environ["GROMADA_EXE"]
    return os.path.join(os.environ.get("GROMADA_DATA", "/root/gromada-data/game"), "gromada.exe")


class Binary:
    def __init__(self, path):
        self.path = path
        self.pe = pefile.PE(path)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.data = open(path, "rb").read()
        self.md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        self.md.detail = True
        text = next(s for s in self.pe.sections if s.Name.startswith(b".text"))
        self.text_va = self.base + text.VirtualAddress
        self.text_bytes = text.get_data()[: text.Misc_VirtualSize]
        self.imports = {}
        for entry in self.pe.DIRECTORY_ENTRY_IMPORT:
            for imp in entry.imports:
                name = imp.name.decode() if imp.name else f"ord{imp.ordinal}"
                self.imports[imp.address] = f"{entry.dll.decode()}!{name}"
        self._insns = None
        self.proj = None
        self.cfg = None

    # ---- raw helpers -------------------------------------------------------------------
    def read(self, va, size):
        off = self.pe.get_offset_from_rva(va - self.base)
        return self.data[off : off + size]

    def dword(self, va):
        return int.from_bytes(self.read(va, 4), "little")

    def cstring(self, va, maxlen=256):
        try:
            raw = self.read(va, maxlen)
        except Exception:
            return None
        return raw.split(b"\0", 1)[0].decode("cp866", "replace")

    def insns(self):
        """Linear sweep of .text driven by the angr function list (avoids data in code)."""
        if self._insns is None:
            self.load_cfg()
            out = []
            for f in self.cfg.kb.functions.values():
                for block in f.blocks:
                    try:
                        code = self.read(block.addr, block.size)
                    except Exception:
                        continue
                    out.extend(self.md.disasm(code, block.addr))
            out.sort(key=lambda i: i.address)
            self._insns = out
        return self._insns

    def load_cfg(self):
        if self.cfg is None:
            self.proj = angr.Project(self.path, auto_load_libs=False)
            self.cfg = self.proj.analyses.CFGFast(normalize=True, data_references=True)
        return self.cfg

    def func_of(self, addr):
        self.load_cfg()
        f = self.cfg.kb.functions.floor_func(addr)
        return f

    def name(self, addr):
        if addr in self.imports:
            return self.imports[addr]
        f = self.cfg.kb.functions.function(addr=addr) if self.cfg else None
        return f.name if f else hex(addr)

    def fmt(self, i):
        s = f"{i.address:08x}  {i.mnemonic:6} {i.op_str}"
        notes = []
        for op in i.operands:
            v = None
            if op.type == capstone.x86.X86_OP_IMM:
                v = op.imm & 0xFFFFFFFF
            elif op.type == capstone.x86.X86_OP_MEM and op.mem.base == 0:
                v = op.mem.disp & 0xFFFFFFFF
            if v is None:
                continue
            if v in self.imports:
                notes.append(self.imports[v])
            elif 0x426000 <= v < 0x42d000:
                st = self.cstring(v, 64)
                if st and len(st) >= 3 and all(32 <= ord(c) < 127 or ord(c) > 127 for c in st):
                    notes.append(repr(st))
        if notes:
            s += "    ; " + ", ".join(notes)
        return s

    # ---- commands ----------------------------------------------------------------------
    def cmd_funcs(self):
        self.load_cfg()
        for f in sorted(self.cfg.kb.functions.values(), key=lambda f: f.addr):
            print(f"{f.addr:08x} {f.size:6d} {f.name}")

    def cmd_dis(self, addr, n=None):
        addr = int(addr, 0)
        if n is not None:
            for i, ins in enumerate(self.md.disasm(self.read(addr, int(n, 0) * 15), addr)):
                if i >= int(n, 0):
                    break
                print(self.fmt(ins))
            return
        f = self.func_of(addr)
        print(f"; function {f.name} at {f.addr:08x} size {f.size}")
        for block in sorted(f.blocks, key=lambda b: b.addr):
            print(f"loc_{block.addr:08x}:")
            for ins in self.md.disasm(self.read(block.addr, block.size), block.addr):
                print("  " + self.fmt(ins))

    def cmd_dec(self, addr):
        addr = int(addr, 0)
        self.load_cfg()
        f = self.cfg.kb.functions.function(addr=addr) or self.func_of(addr)
        try:
            dec = self.proj.analyses.Decompiler(f, cfg=self.cfg.model)
            print(dec.codegen.text if dec.codegen else "<no output>")
        except Exception as e:  # angr's decompiler is not bullet proof
            print(f"<decompiler failed: {e!r}>")

    def operand_values(self, ins):
        for op in ins.operands:
            if op.type == capstone.x86.X86_OP_IMM:
                yield op.imm & 0xFFFFFFFF
            elif op.type == capstone.x86.X86_OP_MEM:
                yield op.mem.disp & 0xFFFFFFFF

    def cmd_imm(self, value, lo=None):
        value = int(value, 0) & 0xFFFFFFFF
        for ins in self.insns():
            if value in self.operand_values(ins):
                f = self.func_of(ins.address)
                print(f"{f.addr:08x}+{ins.address - f.addr:<5x} {self.fmt(ins)}")

    cmd_xrefs = cmd_imm

    def cmd_callers(self, addr):
        addr = int(addr, 0)
        self.load_cfg()
        seen = set()
        for ins in self.insns():
            if ins.mnemonic in ("call", "jmp") and addr in self.operand_values(ins):
                f = self.func_of(ins.address)
                key = (f.addr, ins.address)
                if key not in seen:
                    seen.add(key)
                    print(f"{f.addr:08x} {f.name:24} call at {ins.address:08x}")

    def cmd_callees(self, addr):
        addr = int(addr, 0)
        f = self.func_of(addr)
        for block in sorted(f.blocks, key=lambda b: b.addr):
            for ins in self.md.disasm(self.read(block.addr, block.size), block.addr):
                if ins.mnemonic == "call":
                    print(self.fmt(ins))

    def cmd_str(self, *words):
        needle = " ".join(words).encode("cp866")
        start = 0
        while True:
            off = self.data.find(needle, start)
            if off < 0:
                break
            start = off + 1
            # back up to the string start
            s = off
            while s > 0 and self.data[s - 1] != 0:
                s -= 1
            va = self.base + self.pe.get_rva_from_offset(s)
            print(f"string {va:08x} {self.cstring(va)!r}")
            self.cmd_imm(hex(va))

    def cmd_lin(self, start, end):
        """Linear disassembly of [start, end) regardless of function boundaries."""
        start, end = int(start, 0), int(end, 0)
        for ins in self.md.disasm(self.read(start, end - start), start):
            print(self.fmt(ins))

    def cmd_switch(self, jmp_addr):
        """Decode an MSVC switch ending with `jmp [reg*4 + table]` at jmp_addr.

        Recognises the optional byte index table (`mov cl, byte ptr [reg + T1]`), the case
        bias (`add reg, -k` / `sub reg, k`) and the bound (`cmp reg, n`). Prints the case
        values grouped by target address.
        """
        jmp_addr = int(jmp_addr, 0)
        window = list(self.md.disasm(self.read(jmp_addr - 48, 48 + 7), jmp_addr - 48))
        # re-synchronise: keep the longest tail that ends exactly at jmp_addr
        for skip in range(0, 48):
            window = list(self.md.disasm(self.read(jmp_addr - 48 + skip, 48 - skip + 7), jmp_addr - 48 + skip))
            if any(i.address == jmp_addr and i.mnemonic == "jmp" for i in window):
                break
        window = [i for i in window if i.address <= jmp_addr]
        jmp = window[-1]
        table = jmp.operands[0].mem.disp & 0xFFFFFFFF
        bias, bound, index_table, bound_reg = 0, None, None, None
        for ins in reversed(window[:-1]):
            if ins.mnemonic == "mov" and len(ins.operands) == 2 and ins.operands[1].type == capstone.x86.X86_OP_MEM \
                    and ins.operands[1].size == 1 and index_table is None:
                index_table = ins.operands[1].mem.disp & 0xFFFFFFFF
            elif ins.mnemonic == "cmp" and ins.operands[1].type == capstone.x86.X86_OP_IMM and bound is None:
                bound = ins.operands[1].imm
                bound_reg = ins.operands[0].reg if ins.operands[0].type == capstone.x86.X86_OP_REG else None
            elif bound is not None and bound_reg is not None and ins.operands \
                    and ins.operands[0].type == capstone.x86.X86_OP_REG and ins.operands[0].reg == bound_reg:
                # the instruction that produced the switch register: bias or plain load
                if ins.mnemonic in ("add", "sub") and ins.operands[1].type == capstone.x86.X86_OP_IMM:
                    bias = -ins.operands[1].imm if ins.mnemonic == "add" else ins.operands[1].imm
                elif ins.mnemonic == "lea":
                    bias = -ins.operands[1].mem.disp
                break
        if bound is None:
            print("bound not found")
            return
        groups = {}
        for k in range(bound + 1):
            idx = self.read(index_table + k, 1)[0] if index_table else k
            target = self.dword(table + 4 * idx)
            groups.setdefault(target, []).append(k + bias)
        print(f"; switch at {jmp_addr:08x}: table {table:08x} index {index_table and hex(index_table)} bias {bias} bound {bound}")
        for target, cases in sorted(groups.items(), key=lambda t: t[1][0]):
            print(f"  {target:08x}: {cases}")

    def cmd_jt(self, addr, n="16"):
        addr = int(addr, 0)
        for k in range(int(n, 0)):
            v = self.dword(addr + 4 * k)
            print(f"[{k:3d}] {addr + 4 * k:08x}: {v:08x}")


def main(argv):
    b = Binary(exe_path())
    cmds = " ".join(argv).split(";")
    for c in cmds:
        parts = c.split()
        if not parts:
            continue
        print(f"===== {' '.join(parts)}")
        getattr(b, "cmd_" + parts[0])(*parts[1:])


if __name__ == "__main__":
    main(sys.argv[1:])
