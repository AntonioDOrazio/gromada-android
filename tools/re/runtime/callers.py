"""Print call sites of given functions (and the function containing each). Usage: callers.py ADDR..."""
import sys, grre
p, cfg = grre.load()
for a in sys.argv[1:]:
    t = int(a, 16)
    print('== callers of %#x' % t)
    for f in cfg.kb.functions.values():
        for site in f.get_call_sites():
            if f.get_call_target(site) == t:
                blk = p.factory.block(site)
                print('  call at %#x in func %#x' % (blk.instruction_addrs[-1], f.addr))
