"""Decompile functions to $GRRE_CACHE/dec/<addr>.c (outside the repo). Usage: dec.py 0x41a790 ..."""
import sys, os, grre
p, cfg = grre.load()
for a in sys.argv[1:]:
    addr = int(a, 16)
    out = os.path.join(grre.CACHE, 'dec', '%x.c' % addr)
    try:
        txt = grre.decompile(p, cfg, addr)
    except Exception as e:
        txt = 'ERROR %r' % e
    open(out, 'w').write(txt)
    print(out, len(txt.splitlines()), 'lines')
