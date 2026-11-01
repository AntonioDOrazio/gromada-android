"""List functions found by angr CFGFast in an address range, with size and string refs.
Usage: funcs.py START END"""
import sys, grre
p, cfg = grre.load()
s, e = int(sys.argv[1], 16), int(sys.argv[2], 16)
for f in sorted(cfg.kb.functions.values(), key=lambda f: f.addr):
    if s <= f.addr < e:
        strs = []
        for b in f.blocks:
            for ins in b.capstone.insns:
                for tok in ins.op_str.replace(',', ' ').split():
                    if tok.startswith('0x') and len(tok) >= 8:
                        try:
                            v = int(tok.rstrip(']'), 16); st = grre.cstr(p, v, 60)
                            if len(st) >= 3 and st.isprintable(): strs.append(st)
                        except Exception: pass
        callers = sorted({hex(c.addr) for c in cfg.kb.functions.callgraph.predecessors(f.addr) and [cfg.kb.functions[a] for a in cfg.kb.functions.callgraph.predecessors(f.addr)]})
        print('%x size=%d callers=%s %s' % (f.addr, f.size, ','.join(callers[:6]), strs[:4]))
