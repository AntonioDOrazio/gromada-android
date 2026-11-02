"""Helpers shared by the generators: output format, value pools, script VM runner."""
import json
import random
import struct

from oracle import CpuFault, Oracle, UnexpectedCall, s32  # noqa: F401  (re-exported)

INT_MIN, INT_MAX = -(1 << 31), (1 << 31) - 1

# Error/log functions of the original (docs/re/runtime.md): 0x408030 fatal error,
# 0x407ed0 fatal error (VM stack overflow), 0x407fb0 error log. The oracle never lets them run.
FATAL_ERROR, FATAL_ERROR2, LOG_ERROR = 0x408030, 0x407ed0, 0x407fb0


class Fatal(Exception):
    pass


def install_error_traps(o):
    def fatal(o, args):
        raise Fatal("original error handler called")
    for a in (FATAL_ERROR, FATAL_ERROR2, LOG_ERROR):
        o.stub(a, fatal)


def family(name, function, columns, cases, notes=None, arrays=None):
    """Output record of one generator. `cases` = list of integer lists matching `columns`."""
    for c in cases:
        assert len(c) == len(columns), (name, c)
        assert all(isinstance(v, int) for v in c), (name, c)
    out = {"family": name, "function": function, "columns": columns, "cases": cases}
    if arrays:
        out["arrays"] = arrays
    if notes:
        out["notes"] = notes
    return out


def dump(rec, path):
    """Compact JSON: one case per line (diff friendly, small)."""
    head = {k: v for k, v in rec.items() if k not in ("cases", "arrays")}
    lines = ["{"]
    for k, v in head.items():
        lines.append(f"{json.dumps(k)}: {json.dumps(v, separators=(',', ':'))},")
    if "arrays" in rec:
        lines.append('"arrays": {')
        items = list(rec["arrays"].items())
        for i, (k, v) in enumerate(items):
            lines.append(f"{json.dumps(k)}: {json.dumps(v, separators=(',', ':'))}" + ("," if i + 1 < len(items) else ""))
        lines.append("},")
    lines.append('"cases": [')
    cs = rec["cases"]
    for i, c in enumerate(cs):
        lines.append(json.dumps(c, separators=(",", ":")) + ("," if i + 1 < len(cs) else ""))
    lines.append("]}")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def edge_ints(bits=32):
    """Boundary values of a signed integer of `bits` bits plus small values."""
    lo, hi = -(1 << (bits - 1)), (1 << (bits - 1)) - 1
    v = {0, 1, -1, 2, -2, 3, 7, 8, 15, 16, 31, 32, 33, 63, 64, 100, 127, 128, 255, 256,
         1000, 1023, 1024, 4095, 32767, 32768, 65535, 65536, lo, lo + 1, hi, hi - 1, hi // 2, lo // 2}
    return sorted(x for x in v if lo <= x <= hi)


def rng(name):
    """Deterministic generator per family: regeneration reproduces the same inputs."""
    return random.Random(f"gromada-oracle-{name}")


# ---- script VM (0x41a790) --------------------------------------------------------------
VM_RUN = 0x41a790
LOGIC_SIZE = 0x2928
CELLS = 0x11c


def vm_program(o, code):
    """Run `code` (bytes) on a fresh LOGIC object with no globals; returns (flag, value) of the
    cell on top of the stack, or raises CpuFault/Fatal if the original crashes."""
    logic = o.alloc(LOGIC_SIZE)
    buf = o.alloc(len(code) + 16)
    o.write(buf, code)
    o.w32(logic + 0x100, buf)
    o.w32(logic + 0x104, len(code))
    o.w32(logic + 0x291c, 0)  # number of globals: the stack starts at cell 0
    o.call(VM_RUN, [], conv="thiscall", this=logic)
    sp = o.s32(logic + 0x114)
    top = sp - 1
    return o.u8(logic + CELLS + 5 * top), o.s32(logic + CELLS + 5 * top + 1)


def pushi(v):
    return b"\x01" + struct.pack("<i", s32(v))
