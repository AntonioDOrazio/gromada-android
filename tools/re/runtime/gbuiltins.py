"""Split the builtin dispatcher of gromada.exe (0x412b30, switch on opcode 64..139) into
per-ID handler listings, annotating known helpers. Usage: builtins.py [ID ...]
Reads maps/HEADER.INI from $GROMADA_DATA to name IDs."""
import sys, os, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gdis

DISPATCH = 0x412b30
TABLE = 0x413f5c
FIRST, COUNT = 0x40, 0x4c
NAMES = {
    0x41b200: 'pop_str', 0x41b330: 'pop_int', 0x41b2d0: 'push_int', 0x41b270: 'push_str',
    0x407fb0: 'log_warn', 0x408030: 'fatal', 0x407ed0: 'fatal2',
}

def header_externs(data_dir):
    path = os.path.join(data_dir, 'maps', 'HEADER.INI')
    out = {}
    for line in open(path, 'rb').read().decode('cp866').splitlines():
        m = re.match(r'\s*extern\s+(\w+)\s*\(([^)]*)\)\s*(\d+)\s*;', line)
        if m:
            out.setdefault(int(m.group(3)), []).append((m.group(1), m.group(2)))
    return out

def handlers(img):
    h = {}
    for i in range(COUNT):
        h[FIRST + i] = img.u32(TABLE + 4 * i)
    return h

def main():
    img = gdis.Img()
    data = os.environ.get('GROMADA_DATA', '/root/gromada-data/game')
    ext = header_externs(data)
    h = handlers(img)
    starts = sorted(set(h.values()) | {TABLE})
    want = [int(a, 0) for a in sys.argv[1:]] or sorted(h)
    for i in want:
        s = h[i]
        e = min(x for x in starts if x > s)
        names = ext.get(i, [('?', '')])
        shared = [k for k, v in h.items() if v == s and k != i]
        print('=== ID %d %s  handler %#x..%#x%s' % (i, ' / '.join('%s(%s)' % n for n in names), s, e,
              ('  (same as %s)' % shared) if shared else ''))
        import io
        buf = io.StringIO()
        gdis.disasm(img, s, min(e, s + 0x400), buf)
        for line in buf.getvalue().splitlines():
            m = re.search(r'call +0x([0-9a-f]+)', line)
            if m and int(m.group(1), 16) in NAMES:
                line += '  <' + NAMES[int(m.group(1), 16)] + '>'
            print(line)

if __name__ == '__main__':
    main()
