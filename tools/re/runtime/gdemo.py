#!/usr/bin/env python3
"""Parser for Gromada demo files (maps/demo*.dem), format reverse engineered from gromada.exe
(map load 0x4149b0, tick 0x4141f0, FRead 0x413651, map unload 0x415c00).  See docs/re/runtime.md.

A .dem is a resource container (same as .map / fw.res) with ONE section of type 7:
  u32 namelen, char name[namelen]   map to load (e.g. maps\\levels\\10.map)
  u32 seed                          srand() seed used for the map (MSVC LCG)
  u32 clock                         game clock (GetTime) at map start, overrides the .map timer
  u32 globals[N]                    value of every int cell of the script (declaration order,
                                    array elements included), written right after compiling it
  then, one per game tick, in order:
    u32 clock ; u8 input[12]        input snapshot of the tick (see below)
  interleaved, at the point where the script calls FRead() during a tick:
    u32 len ; char data[len]        line returned by FRead (FOpen/FCreate return 0 in demos)
  u32 0xFFFFFFFF                    written when the recorded map is unloaded
The section ends there; the original files carry an extra trailer after it (the name of the map
to load when the demo ends, e.g. maps\\mainmenu.map), read back by the map loader.

input[12]: i16 world_mouse_x, i16 world_mouse_y, i16 screen_mouse_x, i16 screen_mouse_y,
           u16 input_bits (GetInputState), u8 key (GetKey), u8 unused.

N (number of int cells) depends on the script: it is computed here with gscript.py.
Usage: gdemo.py maps/demo1.dem [--ticks]
"""
import os, sys, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gscript


def parse(path, root):
    d = open(path, 'rb').read()
    nsec = struct.unpack_from('<I', d, 0)[0]
    stype, nxt, cnt, doff = struct.unpack_from('<BIIH', d, 4)
    assert nsec == 1 and stype == 7, 'not a demo container'
    o = 15 + doff
    end = 4 + 5 + nxt
    n = struct.unpack_from('<I', d, o)[0]; o += 4
    name = d[o:o + n].decode('latin-1'); o += n
    seed, clock0 = struct.unpack_from('<II', d, o); o += 8
    ini = os.path.splitext(name)[0] + '.ini'
    c = gscript.Compiler(root).compile(ini)
    nint = sum(1 for v in c.vars if v[1] & gscript.F_INT)
    globals_ = list(struct.unpack_from('<%dI' % nint, d, o)); o += 4 * nint
    ticks, freads = [], []
    while True:
        v = struct.unpack_from('<I', d, o)[0]
        if v == 0xFFFFFFFF:
            o += 4
            break
        if v < 0x10000:            # FRead record (lines are < 128 bytes; clocks are large)
            freads.append((len(ticks), d[o + 4:o + 4 + v].decode('latin-1')))
            o += 4 + v
            continue
        rec = struct.unpack_from('<hhhhHBB', d, o + 4)
        ticks.append((v,) + rec)
        o += 16
    trailer = d[o:].decode('latin-1')
    return dict(name=name, seed=seed, clock0=clock0, nint=nint, globals=globals_, ticks=ticks,
                freads=freads, section_end=end, parsed_end=o, trailer=trailer, vars=c.vars)


if __name__ == '__main__':
    root = os.environ.get('GROMADA_DATA', '/root/gromada-data/game')
    r = parse(sys.argv[1] if os.path.isabs(sys.argv[1]) else os.path.join(root, sys.argv[1]), root)
    t = r['ticks']
    dts = [b[0] - a[0] for a, b in zip(t, t[1:])]
    print('map %s seed %#x clock0 %d  int-globals %d  ticks %d  freads %d' % (
        r['name'], r['seed'], r['clock0'], r['nint'], len(t), len(r['freads'])))
    print('section end %d, parsed end %d (%s)  trailer %r' % (
        r['section_end'], r['parsed_end'], 'OK' if r['section_end'] == r['parsed_end'] else 'MISMATCH', r['trailer']))
    if dts:
        print('tick dt ms: min %d max %d mean %.1f; duration %.1f s' % (min(dts), max(dts), sum(dts) / len(dts), (t[-1][0] - t[0][0]) / 1000))
    keys = sorted({x[6] for x in t if x[6]})
    print('keys pressed:', keys, ' input bits seen: %#x' % (sum({x[5] for x in t}) and max(x[5] for x in t)))
    print('freads:', r['freads'][:20])
    if '--ticks' in sys.argv:
        for x in t:
            print(x)
