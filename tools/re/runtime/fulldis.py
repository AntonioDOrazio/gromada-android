"""Disassemble every basic block found by angr's CFG (sorted, with function headers) to stdout.
Use it to grep for field offsets; keep the output outside the repo."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import grre, gdis
p, cfg = grre.load()
img = gdis.Img()
seen = set()
for f in sorted(cfg.kb.functions.values(), key=lambda f: f.addr):
    print('\n;---- func %#x' % f.addr)
    for b in sorted(f.blocks, key=lambda b: b.addr):
        if b.addr in seen: continue
        seen.add(b.addr)
        gdis.disasm(img, b.addr, b.addr + b.size)
