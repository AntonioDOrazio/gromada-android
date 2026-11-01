"""Helpers for reverse engineering gromada.exe with angr (Ghidra is not available here).

Usage (from another script or `python -i`):
    import grre; p, cfg = grre.load()          # GROMADA_EXE or $GROMADA_DATA/gromada.exe
    print(grre.decompile(p, cfg, 0x401000))
    grre.xrefs_to(p, cfg, 0x42a534)            # code addresses referencing a VA

Scratch output goes to $GRRE_CACHE (default /root/tools/re-runtime-project).
Never commit the executable or full decompiled listings.
"""
import os, pickle, logging, struct
logging.getLogger('angr').setLevel(logging.ERROR)
logging.getLogger('cle').setLevel(logging.ERROR)
logging.getLogger('pyvex').setLevel(logging.ERROR)
import angr

def exe_path():
    if os.environ.get('GROMADA_EXE'):
        return os.environ['GROMADA_EXE']
    return os.path.join(os.environ.get('GROMADA_DATA', '/root/gromada-data/game'), 'gromada.exe')

CACHE = os.environ.get('GRRE_CACHE', '/root/tools/re-runtime-project')

def load(normalize=True):
    """Load the exe and build a CFG (about 20 s). The CFG lives in p.kb, which the decompiler uses."""
    p = angr.Project(exe_path(), auto_load_libs=False)
    cfg = p.analyses.CFGFast(normalize=normalize, data_references=True, cross_references=True,
                             resolve_indirect_jumps=True)
    return p, cfg

def decompile(p, cfg, addr):
    f = cfg.kb.functions.get(addr) or cfg.kb.functions.floor_func(addr)
    d = p.analyses.Decompiler(f, cfg=cfg.model)
    return d.codegen.text if d.codegen else '<no output>'

def read(p, va, n):
    return p.loader.memory.load(va, n)

def u32(p, va):
    return struct.unpack('<I', read(p, va, 4))[0]

def cstr(p, va, n=256):
    b = read(p, va, n)
    return b.split(b'\0')[0].decode('cp1251', 'replace')

def xrefs_to(p, cfg, va):
    """Return sorted list of (ins_addr, func_addr) that reference va."""
    out = set()
    for x in cfg.kb.xrefs.get_xrefs_by_dst(va):
        ia = x.ins_addr
        f = cfg.kb.functions.floor_func(ia) if ia else None
        out.add((ia, f.addr if f else None))
    return sorted(out, key=lambda t: (t[0] or 0))

def find_imm_refs(p, cfg, value):
    """Scan all instructions for an operand equal to value (slow but simple)."""
    import capstone
    res = []
    for f in cfg.kb.functions.values():
        for b in f.blocks:
            try:
                for ins in b.capstone.insns:
                    for op in ins.insn.operands:
                        if (op.type == capstone.x86.X86_OP_IMM and op.imm & 0xffffffff == value) or \
                           (op.type == capstone.x86.X86_OP_MEM and op.mem.disp & 0xffffffff == value):
                            res.append((ins.address, f.addr))
            except Exception:
                pass
    return sorted(set(res))
