#!/usr/bin/env python3
"""Compares the C++ script compiler (src/script/compiler.hpp, through `script_run --layout`)
with the Python replica of the original compiler (tools/re/runtime/gscript.py) on every game
script: bytecode byte for byte, variable cells (name, flags, initial value, order), functions,
externs, defines and iff sites (including the ones reached by a jump).

Usage: script_layout_compare.py PATH/TO/script_run
Needs the game folder in $GROMADA_DATA; exits with 77 (skipped) without it.
Never prints script contents beyond names and offsets.
"""
import glob
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'tools', 're', 'runtime'))


def main():
    root = os.environ.get('GROMADA_DATA')
    if not root or not os.path.isdir(os.path.join(root, 'maps')):
        print('SKIP (set GROMADA_DATA to the game folder)')
        return 77
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    import gscript  # noqa: E402 (path set above)
    tool = sys.argv[1]
    # Every .ini in maps/, maps/levels/, maps/net/, whatever the letter case (levels/18.ini).
    files = sorted(f for d in ('maps', os.path.join('maps', 'levels'), os.path.join('maps', 'net'))
                   for f in glob.glob(os.path.join(root, d, '*')) if f.lower().endswith('.ini'))
    failures = 0
    compared = 0
    for f in files:
        rel = os.path.relpath(f, root)
        if os.path.basename(f).lower() in ('header.ini', 'locale.ini', 'music.ini'):
            continue
        ref = gscript.Compiler(root).compile(rel.replace('/', '\\'))
        out = subprocess.run([tool, '--root', root, '--layout', rel], capture_output=True, check=False)
        if out.returncode != 0:
            print('%s: script_run failed: %s' % (rel, out.stderr.decode('latin-1').strip()))
            failures += 1
            continue
        mine = {'cell': [], 'func': [], 'extern': [], 'define': [], 'iff': []}
        code = b''
        for line in out.stdout.decode('latin-1').splitlines():
            kind, _, rest = line.partition(' ')
            if kind == 'code':
                code = bytes.fromhex(rest)
            elif kind in mine:
                mine[kind].append(rest)
        problems = []
        if code != bytes(ref.code):
            first = next((i for i in range(min(len(code), len(ref.code))) if code[i] != ref.code[i]),
                         min(len(code), len(ref.code)))
            problems.append('bytecode differs (%d vs %d bytes, first difference at %d)' % (len(code), len(ref.code), first))
        ref_cells = []
        for i, (name, flags, value) in enumerate(ref.vars):
            init = '-' if not flags & gscript.F_INIT else ('?' if value is None else str(value))
            ref_cells.append('%d %d %s %s' % (i, flags, init, name.decode('latin-1')))
        if mine['cell'] != ref_cells:
            diff = [(a, b) for a, b in zip(mine['cell'], ref_cells) if a != b][:3]
            problems.append('cells differ (%d vs %d): %s' % (len(mine['cell']), len(ref_cells), diff))
        ref_funcs = ['%s %d %d %d' % (fn['name'].decode('latin-1'), fn['entry'], fn['parambase'], fn['nparams'])
                     for fn in ref.funcs if fn['op'] == 30]
        if mine['func'] != ref_funcs:
            problems.append('functions differ')
        ref_externs = ['%s %d' % (fn['name'].decode('latin-1'), fn['op']) for fn in ref.funcs if fn['op'] != 30]
        if mine['extern'] != ref_externs:
            problems.append('externs differ')
        ref_defines = ['%s %s' % (n.decode('latin-1'), '"%s"' % s.decode('latin-1') if s is not None else str(v - (1 << 32) if v >= 1 << 31 else v))
                       for (n, s, v) in ref.defines]
        if mine['define'] != ref_defines:
            problems.append('defines differ')
        jumped = {t for (k, t) in gscript.analyze(ref) if k == 'iff-reached-by-jmp'}
        ref_iffs = []
        for (start, f1, l1), (opnd, f2, l2) in zip(ref.iff_starts, ref.iff_sites):
            where = '%s:%d' % (f1, l1)
            ref_iffs.append('%d %d %s' % (start, opnd, 'jump' if where in jumped else ''))
        mine_iffs = ['%s %s %s' % (r.split()[0], r.split()[1], 'jump' if r.endswith(' jump') else '') for r in mine['iff']]
        if mine_iffs != ref_iffs:
            problems.append('iff sites differ: %s' % [(a, b) for a, b in zip(mine_iffs, ref_iffs) if a != b][:3])
        compared += 1
        if problems:
            failures += 1
            for p in problems:
                print('%s: %s' % (rel, p))
        else:
            print('%-24s OK  code %5d bytes, %3d cells, %2d functions, %2d iff' % (
                rel, len(code), len(ref_cells), len(ref_funcs), len(ref_iffs)))
    print('%d scripts compared, %d with differences' % (compared, failures))
    return 1 if failures or compared < 39 else 0


if __name__ == '__main__':
    sys.exit(main())
