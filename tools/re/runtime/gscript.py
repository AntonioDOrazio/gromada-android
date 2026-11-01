#!/usr/bin/env python3
"""Byte-exact replica of the Gromada script compiler (gromada.exe 0x417cb0..0x41a790) plus a
bytecode disassembler and a static checker.  Reverse-engineering aid for issue #3; see
docs/re/runtime.md.  The compiler follows the original recursive-descent code including
its quirks (globals-only parameters, compile-time initialisers, '}' -> ';' patching, ...).

Usage:
  gscript.py compile FILE.INI [--dis]     compile one script (paths inside are relative to the
                                          game directory, i.e. $GROMADA_DATA)
  gscript.py check                        compile every maps/*.ini and maps/levels/*.ini and
                                          report statistics and risky constructs
Never commit the game scripts or their bytecode.
"""
import os, sys, struct, re, glob

OPNAMES = {1: 'PUSHI', 2: 'PUSHS', 3: 'ADD', 4: 'SUB', 5: 'MUL', 6: 'DIV', 7: 'MOD', 8: 'NEG',
           9: 'NOT', 10: 'XOR', 11: 'OR', 12: 'AND', 13: 'EQ', 14: 'LOR', 15: 'GT', 16: 'LT',
           17: 'GE', 18: 'LE', 19: 'LNOT', 20: 'NE', 21: 'LAND', 22: 'JZ', 23: 'PUSHV',
           24: 'STORE', 25: 'EOS', 28: 'JMP', 29: 'IFF', 30: 'CALL', 31: 'RET', 32: 'POSTINC',
           33: 'POSTDEC', 34: 'PREINC', 35: 'PREDEC', 36: 'INDEX', 37: 'SHR', 38: 'SHL'}

F_STR, F_INT, F_ARRAY, F_INIT = 1, 2, 4, 8

# Builtins whose handler in 0x412b30 pushes a return value (all others push nothing).
# GetNetInfo (129) pushes nothing for types 7, 8, 9 and out of range.
# C precedence of the operators that the original groups into one level (higher binds tighter)
CPREC = {37: 11, 38: 11, 15: 10, 16: 10, 17: 10, 18: 10, 13: 9, 20: 9, 12: 8, 10: 7, 11: 6, 21: 5, 14: 4}

PUSHING_BUILTINS = {65, 67, 68, 69, 74, 75, 85, 86, 88, 90, 93, 94, 95, 96, 102, 103, 105, 109, 110,
                    112, 113, 115, 116, 117, 121, 123, 124, 129, 131, 133, 137, 138, 139}


class CompileError(Exception):
    pass


def _isspace(c): return c in b' \t\n\r\v\f'
def _isdigit(c): return 48 <= c <= 57
def _isxdigit(c): return _isdigit(c) or 65 <= c <= 70 or 97 <= c <= 102
def _isalpha(c): return 65 <= c <= 90 or 97 <= c <= 122
def _isalnum(c): return _isalpha(c) or _isdigit(c)


def scanf_i(buf, pos):
    """C sscanf("%i"): optional sign, 0x hex, leading-0 octal, decimal. Returns value or None."""
    m = re.match(rb'\s*([+-]?)(0[xX][0-9a-fA-F]+|0[0-7]*|[1-9][0-9]*)', bytes(buf[pos:pos + 40]))
    if not m:
        return None
    s = m.group(2)
    v = int(s, 16) if s[:2].lower() == b'0x' else (int(s, 8) if s.startswith(b'0') and len(s) > 1 else int(s))
    if m.group(1) == b'-':
        v = -v
    return v & 0xffffffff


class Compiler:
    def __init__(self, root, warn=None):
        self.root = root
        self.code = bytearray(b'\x1e\0\0\0\0\x1c\x30\x75')  # CALL main ; JMP +30000
        self.vars = []      # [name, flags, value] ; arrays = several cells with the same name
        self.defines = []   # (name, str|None, int)
        self.funcs = []     # dict(name, op, entry, parambase)
        self.warn = warn or (lambda m: None)
        self.notes = []     # (kind, text) static findings
        self.buf = b''
        self.pos = 0
        self.file = ''

    # ---- low level
    def cur(self, k=0):
        p = self.pos + k
        return self.buf[p] if 0 <= p < len(self.buf) else 0

    def fatal(self, msg):
        raise CompileError('%s @%s:%d' % (msg, self.file, self.buf[:self.pos].count(b'\n') + 1))

    def skip_ws(self):  # 0x418000; returns True at end of buffer
        b = self.buf
        if self.pos >= len(b):
            return True
        while True:
            c = b[self.pos]
            if c == 0x2f and self.cur(1) == 0x2f:
                while True:
                    if b[self.pos] == 13:
                        break
                    self.pos += 1
                    if self.pos >= len(b):
                        return True
                    if b[self.pos] == 10:
                        break
                continue
            if c == 0x2f and self.cur(1) == 0x2a:
                while True:
                    if self.cur(-1) == 0x2f and self.cur(-2) == 0x2a:
                        break
                    self.pos += 1
                    if self.pos >= len(b):
                        return True
                continue
            if not _isspace(c) and c != 0:
                return False
            self.pos += 1
            if self.pos >= len(b):
                return True

    def ident(self):  # 0x4180b0
        if self.skip_ws():
            self.fatal('LOGIC::unened .ini file')
        s = bytearray()
        while _isalnum(self.cur()) or self.cur() == 0x5f:
            if len(s) >= 31:
                self.fatal('LOGIC::Long variable name')
            s.append(self.cur())
            self.pos += 1
        return bytes(s)

    def emit(self, *bs):
        self.code.extend(bs)

    def emit32(self, v):
        self.code.extend(struct.pack('<I', v & 0xffffffff))

    # ---- expressions
    def primary(self):  # 0x418140
        unary = 0
        c = self.cur()
        if c == 0x2d and self.cur(1) != 0x2d:
            self.pos += 1; unary = 8
        elif c == 0x7e:
            self.pos += 1; unary = 9
        elif c == 0x21:
            self.pos += 1; unary = 19
        if self.skip_ws():
            self.fatal('LOGIC::~!- not ended')
        varop = 23
        if self.cur() == 0x2d and self.cur(1) == 0x2d:
            varop = 35; self.pos += 2
        elif self.cur() == 0x2b and self.cur(1) == 0x2b:
            varop = 34; self.pos += 2
        if self.skip_ws():
            self.fatal('LOGIC::Unended mnog')
        c = self.cur()
        if _isdigit(c):
            v = scanf_i(self.buf, self.pos)
            txt = re.match(rb'[0-9a-fA-FxX]*', self.buf[self.pos:]).group()
            if len(txt) > 1 and txt[0] == 0x30 and txt[1] not in b'xX':
                self.notes.append(('octal-literal', '%s:%s' % (self.file, txt.decode())))
            self.emit(1); self.emit32(v)
            while _isxdigit(self.cur()) or self.cur() == 0x78:
                self.pos += 1
        elif c == 0x22:
            self.emit(2)
            self.pos += 1
            esc = False
            if self.cur() != 0x22:
                while True:
                    ch = self.cur()
                    if esc:
                        esc = False
                        self.emit(10 if ch == 0x6e else 13 if ch == 0x72 else ch)
                    elif ch == 0x5c:
                        esc = True
                    else:
                        self.emit(ch)
                    nxt = self.cur(1)
                    self.pos += 1
                    if nxt == 0x22:
                        break
            self.pos += 1
            self.emit(0)
        elif c == 0x27:
            v = self.cur(1)
            if v >= 0x80:
                v -= 0x100
            self.emit(1); self.emit32(v)
            self.pos += 2
            if self.cur() != 0x27:
                self.fatal("LOGIC::Enogth second '")
            self.pos += 1
        elif _isalpha(c):
            self.identifier(varop)
        elif c == 0x28:
            self.pos += 1
            if self.skip_ws():
                self.fatal('LOGIC::Not enough ) in end file')
            self.expr()
            if self.cur() != 0x29:
                self.fatal('LOGIC::Not enough )')
            self.pos += 1
        elif c == 0x23:
            self.pos += 1
            self.define()
        else:
            if unary:
                self.fatal('LOGIC::error:')
        if unary:
            self.emit(unary)
        self.skip_ws()

    def define(self):
        w = self.ident()
        if w != b'define':
            self.fatal('LOGIC::unknown preprocessor command:' + w.decode())
        name = self.ident()
        if len(self.defines) >= 256:
            self.fatal('LOGIC::Too many defines')
        if self.skip_ws():
            self.fatal('LOGIC::Not enough define constant')
        if self.cur() == 0x22:
            self.pos += 1
            s = bytearray()
            while self.cur() != 0x22:
                s.append(self.cur()); self.pos += 1
            self.pos += 1
            self.defines.append((name, bytes(s), 0))
        else:
            v = scanf_i(self.buf, self.pos)
            if v is None:
                self.notes.append(('define-not-number', '%s:%s' % (self.file, name.decode())))
                v = 0
            while _isxdigit(self.cur()) or self.cur() == 0x78:
                self.pos += 1
            self.defines.append((name, None, v))

    def identifier(self, varop=23):
        name = self.ident()
        if self.skip_ws():
            self.fatal('LOGIC::Unreferenced code')
        for i, f in enumerate(self.funcs):
            if f['name'] == name:
                return self.call(f, name)
        for (dn, ds, dv) in self.defines:
            if dn == name:
                if ds is not None:
                    self.emit(2); self.code.extend(ds); self.emit(0)
                else:
                    self.emit(1); self.emit32(dv)
                return
        for vi, v in enumerate(self.vars):
            if v[0] == name:
                return self.variable(vi, v, varop)
        if name == b'int' or name == b'string':
            return self.declare(name == b'string')
        if name == b'extern':
            return self.extern()
        if name == b'return':
            if self.cur() == 0x28:
                self.pos += 1
                if self.skip_ws():
                    self.fatal('LOGIC::Not enough ) after return(')
                self.expr()
                if self.cur() != 0x29:
                    self.fatal('LOGIC::Not enough ) after return(')
                self.pos += 1
            if self.skip_ws():
                self.fatal('LOGIC::Not enough ; after return')
            self.emit(31)
            return
        if self.cur() == 0x28:
            return self.funcdef(name)
        self.fatal('LOGIC::Unknown name %s' % name.decode())

    def call(self, f, name):
        if self.cur() != 0x28:
            self.fatal('LOGIC:: function without () ' + name.decode())
        self.pos += 1
        argi = 0
        start = len(self.code)
        while True:
            if self.skip_ws():
                self.fatal('LOGIC::Not enough ) in ' + name.decode())
            if self.cur() != 0x29:
                self.expr()
                if f['op'] == 30:
                    self.emit(24, (f['parambase'] + argi) & 0xff, 25)
            c = self.cur(); self.pos += 1; argi += 1
            if c == 0x29:
                break
        self.emit(f['op'])
        if f['op'] == 30:
            self.emit32(f['entry'])
        self.calls.append((name, f['op'], argi, start))

    def variable(self, vi, v, varop):
        idx = None
        if self.cur() == 0x5b:
            save = len(self.code)
            self.pos += 1
            self.expr()
            self.emit(36)
            idx = bytes(self.code[save:]); del self.code[save:]
            c = self.cur(); self.pos += 1
            if c != 0x5d:
                self.fatal("LOGIC::None ']'")
        op = varop
        c = self.cur()
        if c == 0x3d and self.cur(1) != 0x3d:
            self.pos += 1
            if self.skip_ws():
                self.fatal('LOGIC::Not enough vyrag after =')
            self.expr(); op = 24
        elif c == 0x2b and self.cur(1) == 0x2b:
            op = 32; self.pos += 2
        elif c == 0x2d and self.cur(1) == 0x2d:
            op = 33; self.pos += 2
        if idx is not None:
            self.code.extend(idx)
            if op != 24 and (v[1] & F_STR) and not (v[1] & F_ARRAY):
                self.notes.append(('string-index-read', '%s:%s' % (self.file, v[0].decode())))
        self.emit(op, vi & 0xff)
        if vi > 255:
            self.notes.append(('var-index-overflow', '%s:%s=%d' % (self.file, v[0].decode(), vi)))

    def declare(self, is_str):
        while True:
            name = self.ident()
            if len(self.vars) >= 256:
                self.fatal('LOGIC::Too many variables')
            if len(name) > 32:
                self.fatal('LOGIC::variable name too long')
            for v in self.vars:
                if v[0] == name:
                    self.fatal('LOGIC::%s redefinition' % name.decode())
            n = 1
            if self.cur() == 0x5b:
                self.pos += 1; self.skip_ws()
                n = scanf_i(self.buf, self.pos) or 0
                while _isxdigit(self.cur()) or self.cur() == 0x78:
                    self.pos += 1
                self.skip_ws()
                c = self.cur(); self.pos += 1
                if c != 0x5d:
                    self.fatal("LOGIC::None ']'")
            base = len(self.vars)
            for _ in range(n):
                fl = (F_STR if is_str else F_INT) | (F_ARRAY if n > 1 else 0)
                self.vars.append([name, fl, None])
            if self.in_function:
                self.notes.append(('decl-in-function', '%s:%s' % (self.file, name.decode())))
            self.skip_ws()
            if not is_str and self.cur() == 0x3d:
                self.pos += 1; self.skip_ws()
                if n == 1:
                    self.vars[base][2] = scanf_i(self.buf, self.pos)
                    self.vars[base][1] |= F_INIT
                    while _isxdigit(self.cur()) or self.cur() in (0x78, 0x2d):
                        self.pos += 1
                    self.skip_ws()
                else:
                    if self.cur() != 0x7b:
                        self.fatal('LOGIC::Not enough { in initialized')
                    i = 0
                    while True:
                        if i >= n:
                            self.fatal('LOGIC::too many initialized value')
                        self.pos += 1; self.skip_ws()
                        self.vars[base + i][2] = scanf_i(self.buf, self.pos)
                        self.vars[base + i][1] |= F_INIT
                        while _isxdigit(self.cur()) or self.cur() in (0x78, 0x2d):
                            self.pos += 1
                        i += 1
                        self.skip_ws()
                        if self.cur() != 0x2c:
                            break
                    if i < n:
                        self.notes.append(('partial-array-init', '%s:%s %d/%d' % (self.file, name.decode(), i, n)))
                    c = self.cur(); self.pos += 1
                    if c != 0x7d:
                        self.fatal('LOGIC::Not enough } in initialized')
            self.skip_ws()
            c = self.cur(); self.pos += 1
            if c != 0x2c:
                break
        self.pos -= 1

    def extern(self):
        if len(self.funcs) >= 128:
            self.fatal('LOGIC::Too many extern function')
        name = self.ident()
        pstart = self.pos
        c = self.cur(); self.pos += 1
        if c != 0x29:
            while True:
                c = self.cur(); self.pos += 1
                if c == 0x29:
                    break
        if self.skip_ws() or not _isdigit(self.cur()):
            self.fatal('LOGIC::Not enough extern function code')
        op = scanf_i(self.buf, self.pos) & 0xff
        while _isxdigit(self.cur()) or self.cur() == 0x78:
            self.pos += 1
        if any(f['name'] == name for f in self.funcs):
            self.notes.append(('duplicate-extern', '%s %d' % (name.decode(), op)))
        params = self.buf[pstart + 1:self.buf.index(b')', pstart)].strip()
        npar = 0 if not params else params.count(b',') + 1
        self.funcs.append(dict(name=name, op=op, entry=0, parambase=0, nparams=npar))
        self.builtin_nparams.setdefault(op, npar)

    def funcdef(self, name):
        if len(self.funcs) >= 128:
            self.fatal('LOGIC::Too many extern function')
        for f in self.funcs:
            if f['name'] == name:
                self.fatal('LOGIC::%s() redefinition' % name.decode())
        f = dict(name=name, op=0, entry=0, parambase=len(self.vars))
        self.funcs.append(f)
        c = self.cur(); self.pos += 1
        if c != 0x29:  # c is '(' here: the original tests the '(' itself, so the loop always runs
            pass
        if True:
            # the original reads '(' and then always enters the loop unless that char was ')'
            while True:
                if self.skip_ws():
                    self.fatal('LOGIC::Not enough ) in ' + name.decode())
                self.expr()
                if f['parambase'] == len(self.vars) and self.cur() != 0x29:
                    self.fatal('LOGIC::Incorrect declare ' + name.decode())
                c = self.cur(); self.pos += 1
                if c == 0x29:
                    break
        if self.skip_ws():
            self.fatal('LOGIC::Not enough function body ' + name.decode())
        f['op'] = 30; f['entry'] = len(self.code); f['nparams'] = len(self.vars) - f['parambase']
        if name == b'main':
            self.code[1:5] = struct.pack('<I', f['entry'])
        if self.cur() != 0x7b:
            self.fatal('LOGIC::Not enough function body ' + name.decode())
        self.pos += 1
        self.in_function = True
        if self.cur() != 0x7d:
            while True:
                if self.skip_ws():
                    self.fatal('LOGIC::Not enough } in ' + name.decode())
                if self.stmt(None):
                    self.fatal('LOGIC::Not enough } in ' + name.decode())
                if self.cur() == 0x7d:
                    break
        self.in_function = False
        # the original overwrites the closing '}' with ';' so the caller emits an EOS after RET
        self.buf = self.buf[:self.pos] + b';' + self.buf[self.pos + 1:]
        self.emit(31)
        f['end'] = len(self.code)

    def binlevel(self, sub, ops):
        chain = []
        sub()
        while True:
            for tok, op in ops:
                if self.buf[self.pos:self.pos + len(tok)] == tok and not (
                        len(tok) == 1 and tok in (b'&', b'|', b'>', b'<') and self.cur(1) in (tok[0], 0x3d)):
                    self.pos += len(tok)
                    if self.skip_ws():
                        self.fatal('LOGIC::Not enough second operand')
                    sub()
                    self.emit(op)
                    self.ops_seen.append(op)
                    prec = CPREC.get(op)
                    if prec is not None:
                        if any(CPREC.get(o, 99) < prec for o in chain):
                            self.notes.append(('c-precedence-differs', '%s:%d' % (
                                self.file, self.buf[:self.pos].count(b'\n') + 1)))
                        chain.append(op)
                    break
            else:
                break
        self.skip_ws()

    def mnog(self):
        self.binlevel(self.primary, [(b'*', 5), (b'/', 6), (b'%', 7)])

    def slag(self):
        self.binlevel(self.mnog, [(b'+', 3), (b'-', 4)])

    def cmpslag(self):
        self.binlevel(self.slag, [(b'>=', 17), (b'>>', 37), (b'>', 15), (b'<=', 18), (b'<<', 38),
                                  (b'<', 16), (b'==', 13), (b'!=', 20)])

    def expr(self):  # 0x419b40: ^ & && | || all at one level, left to right
        mark = len(self.ops_seen)
        self.binlevel(self.cmpslag, [(b'^', 10), (b'&&', 21), (b'&', 12), (b'||', 14), (b'|', 11)])
        lvl = set(self.ops_seen[mark:]) & {10, 11, 12, 14, 21}
        if 14 in lvl and 21 in lvl:
            self.notes.append(('mixed-and-or', '%s:%d' % (self.file, self.buf[:self.pos].count(b'\n') + 1)))
        del self.ops_seen[mark:]

    # ---- statements (0x419cf0); returns True at end of file
    def stmt(self, breaks):
        if self.skip_ws():
            self.fatal('LOGIC::unened .ini file')
        start = self.pos
        self.linemap.append((len(self.code), self.file, self.buf[:self.pos].count(b'\n') + 1))
        w = self.ident()
        if w in (b'if', b'iff'):
            if w == b'iff':
                self.iff_starts.append((len(self.code), self.file, self.buf[:self.pos].count(b'\n') + 1))
            if self.skip_ws():
                self.fatal("LOGIC::Unended 'if'")
            if self.cur() != 0x28:
                self.fatal("LOGIC::'if' without (")
            self.pos += 1
            if self.skip_ws():
                self.fatal("LOGIC::Not enough ) in 'if'")
            self.expr()
            if self.cur() != 0x29:
                self.fatal("LOGIC::'if' without )")
            self.pos += 1
            self.emit(29 if w == b'iff' else 22)
            opnd = len(self.code); self.emit(0, 0)
            if w == b'iff':
                self.iff_sites.append((opnd - 1, self.file, self.buf[:self.pos].count(b'\n') + 1))
            if self.skip_ws():
                self.fatal("LOGIC::Not enough ; in 'if'")
            self.stmt(breaks)
            after = self.pos
            struct.pack_into('<h', self.code, opnd, len(self.code) - opnd)
            w2 = self.ident()
            if w2 == b'else':
                rel = struct.unpack_from('<h', self.code, opnd)[0] + 3
                struct.pack_into('<h', self.code, opnd, rel)
                self.emit(28); op2 = len(self.code); self.emit(0, 0)
                self.else_jumps.append(op2 - 1)
                if w == b'iff':
                    self.notes.append(('iff-else', '%s:%d' % (self.file, self.buf[:self.pos].count(b'\n') + 1)))
                if self.skip_ws():
                    self.fatal("LOGIC::Not enough ; in 'else'")
                self.stmt(breaks)
                struct.pack_into('<h', self.code, op2, len(self.code) - op2)
                return self.skip_ws()
            self.pos = after
            return self.skip_ws()
        if w == b'while':
            mybreaks = []
            if self.skip_ws():
                self.fatal("LOGIC::Unended 'while'")
            if self.cur() != 0x28:
                self.fatal("LOGIC::'while' without (")
            self.pos += 1
            if self.skip_ws():
                self.fatal("LOGIC::Not enough ) in 'while'")
            top = len(self.code)
            self.expr()
            c = self.cur(); self.pos += 1
            if c != 0x29:
                self.fatal("LOGIC::'while' without )")
            self.emit(22); opnd = len(self.code); self.emit(0, 0)
            if self.skip_ws():
                self.fatal("LOGIC::Not enough ; in 'while'")
            self.stmt(mybreaks)
            self.emit(28); j = len(self.code); self.emit(0, 0)
            struct.pack_into('<h', self.code, j, top - j)
            end = len(self.code)
            struct.pack_into('<h', self.code, opnd, end - opnd)
            for b in mybreaks:
                struct.pack_into('<h', self.code, b, end - b)
            return self.skip_ws()
        if w == b'for':
            mybreaks = []
            if self.skip_ws():
                self.fatal("LOGIC::Unended 'for'")
            c = self.cur(); self.pos += 1
            if c != 0x28:
                self.fatal("LOGIC::'for' without (")
            if self.skip_ws():
                self.fatal("LOGIC::Not enough ) in 'for'")
            self.expr()
            c = self.cur(); self.pos += 1
            if c != 0x3b:
                self.fatal("LOGIC::Not enough first ; in 'for'")
            top = len(self.code)
            if self.skip_ws():
                self.fatal("LOGIC::Not enough ) in 'for'")
            self.expr()
            c = self.cur(); self.pos += 1
            if c != 0x3b:
                self.fatal("LOGIC::Not enough second ; in 'for'")
            if self.skip_ws():
                self.fatal("LOGIC::Not enough ) in 'for'")
            self.emit(22); opnd = len(self.code); self.emit(0, 0)
            incr = self.pos
            depth = 0
            while True:
                c = self.cur()
                if c == 0x28:
                    depth += 1
                elif c == 0x29:
                    depth -= 1
                self.pos += 1
                if depth < 0:
                    break
            self.stmt(mybreaks)
            after = self.pos
            self.pos = incr
            self.expr()
            self.pos = after
            self.emit(25, 28); j = len(self.code); self.emit(0, 0)
            struct.pack_into('<h', self.code, j, top - j)
            end = len(self.code)
            struct.pack_into('<h', self.code, opnd, end - opnd)
            for b in mybreaks:
                struct.pack_into('<h', self.code, b, end - b)
            return self.skip_ws()
        if w == b'break':
            if self.skip_ws():
                self.fatal("LOGIC::Unended 'break'")
            if self.cur() != 0x3b:
                self.fatal("LOGIC::'break' without ;")
            self.pos += 1
            if breaks is None:
                self.fatal("LOGIC::'break' without loop")
            self.emit(28); breaks.append(len(self.code)); self.emit(0, 0)
            return self.skip_ws()
        self.pos = start
        c = self.cur()
        if c == 0x7b:
            self.pos += 1
            if self.cur() != 0x7d:
                while True:
                    if self.skip_ws():
                        self.fatal('LOGIC::Not enough }')
                    if self.stmt(breaks):
                        self.fatal('LOGIC::Not enough }')
                    if self.cur() == 0x7d:
                        break
            self.pos += 1
            return self.skip_ws()
        if c == 0x23 and self.buf[self.pos + 1:self.pos + 8] == b'include':
            self.pos += 1
            self.ident()
            if self.skip_ws() or self.cur() != 0x22:
                self.fatal('LOGIC::Not enough include file name')
            self.pos += 1
            s = bytearray()
            while self.cur() != 0x22:
                if self.pos >= len(self.buf):
                    self.fatal('LOGIC::Not enough second "')
                s.append(self.cur()); self.pos += 1
            self.pos += 1
            self.include(s.decode('latin-1'))
            return self.skip_ws()
        if c == 0x23:
            self.expr()
            return self.skip_ws()
        self.expr()
        if self.cur() == 0x3b:
            self.pos += 1
            self.emit(25)
        else:
            self.fatal('LOGIC::Not enough ; in %r' % self.buf[max(0, self.pos - 10):self.pos + 10])
        return self.skip_ws()

    def resolve(self, name):
        p = os.path.join(self.root, *name.replace('\\', '/').split('/'))
        if os.path.exists(p):
            return p
        d = self.root   # case-insensitive lookup (the original runs on Windows)
        for part in name.replace('\\', '/').split('/'):
            cand = [x for x in os.listdir(d) if x.lower() == part.lower()]
            if not cand:
                raise CompileError("LOGIC::Can't open file %s" % name)
            d = os.path.join(d, cand[0])
        return d

    def include(self, name):
        saved = (self.buf, self.pos, self.file)
        self.buf = open(self.resolve(name), 'rb').read()
        self.pos = 0
        self.file = name
        if not self.skip_ws():
            while not self.stmt(None):
                pass
        self.buf, self.pos, self.file = saved

    def compile(self, name):
        self.calls = []; self.iff_sites = []; self.else_jumps = []; self.ops_seen = []
        self.linemap = []; self.builtin_nparams = {}; self.iff_starts = []
        self.in_function = False
        self.include(name)
        if struct.unpack_from('<I', self.code, 1)[0] == 0:
            raise CompileError('LOGIC::Not enough main() function')
        return self


def disasm(code, names=None):
    names = names or {}
    out = []
    i = 0
    while i < len(code):
        op = code[i]; a = i; i += 1
        n = OPNAMES.get(op)
        if op == 1:
            arg = '%d' % struct.unpack_from('<i', code, i)[0]; i += 4
        elif op == 2:
            e = code.index(0, i); arg = repr(bytes(code[i:e]))[1:]; i = e + 1
        elif op in (22, 28, 29):
            r = struct.unpack_from('<h', code, i)[0]; arg = '-> %d' % (i + r); i += 2
        elif op == 30:
            t = struct.unpack_from('<I', code, i)[0]; arg = '%d %s' % (t, names.get(t, '')); i += 4
        elif op in (23, 24, 32, 33, 34, 35):
            arg = '#%d' % code[i]; i += 1
        elif n:
            arg = ''
        else:
            n = 'BUILTIN'; arg = str(op)
        out.append('%5d  %-8s %s' % (a, n, arg))
    return '\n'.join(out)


def where(c, off):
    best = None
    for (o, f, l) in c.linemap:
        if o <= off:
            best = (f, l)
        else:
            break
    return '%s:%d' % best if best else '?'


def analyze(c):
    """Linear stack-depth simulation of the bytecode, statement by statement (expressions are
    straight-line code: the VM has no short-circuit evaluation).  Reports constructs that the
    original VM executes in a surprising way."""
    code = c.code
    targets = set()
    i = 0
    insns = []
    while i < len(code):
        op = code[i]; a = i; i += 1
        arg = None
        if op == 1: arg = struct.unpack_from('<i', code, i)[0]; i += 4
        elif op == 2: i = code.index(0, i) + 1
        elif op in (22, 28, 29): r = struct.unpack_from('<h', code, i)[0]; targets.add(i + r); arg = i + r; i += 2
        elif op == 30: arg = struct.unpack_from('<I', code, i)[0]; targets.add(arg); i += 4
        elif op in (23, 24, 32, 33, 34, 35): arg = code[i]; i += 1
        insns.append((a, op, arg))
    entries = {f['entry'] for f in c.funcs if f['op'] == 30}
    notes = []
    depth = 0; called = False
    depth_at = {}
    jmp_targets = {arg for (a, op, arg) in insns if op == 28 and a != 5}
    for (s0, f, l) in c.iff_starts:
        if s0 in jmp_targets:
            notes.append(('iff-reached-by-jmp', '%s:%d' % (f, l)))
    for (a, op, arg) in insns:
        if a in targets or a in entries:
            depth = 0; called = False
        depth_at[a] = depth
        if op in (1, 2, 23, 32, 33, 34, 35):
            depth += 1
        elif op in (3, 4, 5, 6, 7, 10, 11, 12, 13, 14, 15, 16, 17, 18, 20, 21, 37, 38, 36):
            depth -= 1
            if depth < 1 and op != 36 or depth < 0:
                notes.append(('stack-underflow', where(c, a)))
        elif op == 24:
            if depth < 1: notes.append(('stack-underflow', where(c, a)))
        elif op in (22, 29, 25):
            if op == 29 and called:
                notes.append(('iff-with-user-call', where(c, a)))
            if depth > (2 if op == 22 else 1):
                notes.append(('stack-leftover', where(c, a)))
            depth = 0; called = False
        elif op == 30:
            if depth > 0:
                notes.append(('user-call-not-first', where(c, a)))
            depth = 1; called = True
        elif op == 31:
            depth = 0; called = False
        elif op == 28:
            depth = 0
        elif op >= 64:
            n = c.builtin_nparams.get(op, 0)
            depth -= n
            if depth < 0:
                notes.append(('builtin-underflow', '%s op%d' % (where(c, a), op)))
                depth = 0
            if op in PUSHING_BUILTINS:
                depth += 1
    for (name, op, argc, start) in c.calls:
        if op == 30 and depth_at.get(start, 0) > 0:
            notes.append(('user-call-not-first', '%s %s' % (where(c, start), name.decode())))
    return notes


def check(root):
    files = sorted(set(glob.glob(os.path.join(root, 'maps', '*.ini')) + glob.glob(os.path.join(root, 'maps', '*.INI'))
                       + glob.glob(os.path.join(root, 'maps', 'levels', '*.INI')) + glob.glob(os.path.join(root, 'maps', 'net', '*.INI'))))
    ok = 0
    allnotes = {}
    for f in files:
        rel = os.path.relpath(f, root).replace('/', '\\')
        base = os.path.basename(f).lower()
        if base in ('header.ini', 'locale.ini', 'music.ini'):
            continue
        try:
            c = Compiler(root).compile(rel)
        except CompileError as e:
            print('%-28s ERROR %s' % (rel, e)); continue
        ok += 1
        ncells = len(c.vars)
        nint = sum(1 for v in c.vars if v[1] & F_INT)
        print('%-28s code=%5d bytes  cells=%3d (int %3d)  funcs=%3d defines=%3d iff=%3d %s' % (
            rel, len(c.code), ncells, nint, len(c.funcs), len(c.defines), len(c.iff_sites),
            'OVER-30006!' if len(c.code) > 30006 else ''))
        for k, t in c.notes + analyze(c):
            allnotes.setdefault(k, set()).add(t)
        # user-function calls with arguments nested inside a larger expression
        for (name, op, argc, start) in c.calls:
            pass
    print('\ncompiled %d scripts' % ok)
    for k in sorted(allnotes):
        print('%s (%d): %s' % (k, len(allnotes[k]), ', '.join(sorted(allnotes[k])[:40])))


if __name__ == '__main__':
    root = os.environ.get('GROMADA_DATA', '/root/gromada-data/game')
    if len(sys.argv) >= 3 and sys.argv[1] == 'compile':
        c = Compiler(root).compile(sys.argv[2])
        names = {f['entry']: f['name'].decode() for f in c.funcs if f['op'] == 30}
        print('code %d bytes, %d cells, %d funcs, %d defines' % (len(c.code), len(c.vars), len(c.funcs), len(c.defines)))
        if '--dis' in sys.argv:
            print(disasm(c.code, names))
    elif len(sys.argv) >= 2 and sys.argv[1] == 'check':
        check(root)
    else:
        print(__doc__)
