"""Linear disassembly of a VA range of gromada.exe with string/xref annotations.
Usage: dis.py START [END|+LEN]   (no angr needed; capstone + pefile)
Output goes to stdout; do not commit full listings."""
import sys, os, re, struct
import pefile, capstone

def exe_path():
    return os.environ.get('GROMADA_EXE') or os.path.join(os.environ.get('GROMADA_DATA', '/root/gromada-data/game'), 'gromada.exe')

class Img:
    def __init__(self, path=None):
        self.pe = pefile.PE(path or exe_path())
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.mem = bytearray(self.pe.get_memory_mapped_image())
    def rd(self, va, n): return bytes(self.mem[va - self.base: va - self.base + n])
    def u32(self, va): return struct.unpack('<I', self.rd(va, 4))[0]
    def u16(self, va): return struct.unpack('<H', self.rd(va, 2))[0]
    def cstr(self, va, n=200):
        if not (self.base <= va < self.base + len(self.mem)): return None
        b = self.rd(va, n).split(b'\0')[0]
        if len(b) >= 3 and all(32 <= c < 127 or c >= 0x80 or c in (9, 10, 13) for c in b):
            return b.decode('cp1251', 'replace')
        return None
    def valid(self, va): return self.base + 0x1000 <= va < self.base + len(self.mem)

def disasm(img, start, end, out=sys.stdout):
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md.detail = False
    code = img.rd(start, end - start)
    for ins in md.disasm(code, start):
        note = ''
        for m in re.finditer(r'0x([0-9a-f]{6,8})', ins.op_str):
            v = int(m.group(1), 16)
            s = img.cstr(v)
            if s: note += '  ; "%s"' % s[:60]
        out.write('%08x  %-8s %s%s\n' % (ins.address, ins.mnemonic, ins.op_str, note))

if __name__ == '__main__':
    img = Img()
    s = int(sys.argv[1], 16)
    e = sys.argv[2] if len(sys.argv) > 2 else '+0x100'
    e = s + int(e[1:], 16) if e.startswith('+') else int(e, 16)
    disasm(img, s, e)
