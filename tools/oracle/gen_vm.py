"""Integer arithmetic of the script VM 0x41a790 (docs/re/runtime.md §2.4), executed through the
real interpreter loop on the bytecode `PUSHI a; PUSHI b; OP` (or `PUSHI a; OP` for unary ops)."""
from common import INT_MAX, INT_MIN, CpuFault, Fatal, family, pushi, rng, vm_program

BINARY = {3: "add", 4: "sub", 5: "mul", 6: "div", 7: "mod", 10: "xor", 11: "or", 12: "and", 13: "eq", 14: "lor",
          15: "gt", 16: "lt", 17: "ge", 18: "le", 20: "ne", 21: "land", 37: "shr", 38: "shl"}
UNARY = {8: "neg", 9: "not", 19: "lnot"}
EDGE = [0, 1, -1, 2, -2, 7, -7, 31, 32, 33, -31, -32, 100, 65536, INT_MIN, INT_MAX]


def generate(o):
    r = rng("vm")
    cases = []

    def run(op, a, b, unary):
        o.heap_reset()
        code = pushi(a) + (b"" if unary else pushi(b)) + bytes([op])
        try:
            flag, value = vm_program(o, code)
            assert flag == 2, (op, a, b, flag)
            cases.append([op, a, b, 0, value])
        except (CpuFault, Fatal):
            cases.append([op, a, b, 1, 0])

    for op in BINARY:
        pairs = [(a, b) for a in EDGE for b in EDGE]
        pairs += [(r.randrange(INT_MIN, INT_MAX + 1), r.randrange(INT_MIN, INT_MAX + 1)) for _ in range(30)]
        pairs += [(r.randrange(-1000, 1001), r.randrange(-40, 41)) for _ in range(30)]
        for a, b in pairs:
            run(op, a, b, False)
    for op in UNARY:
        for a in EDGE + [r.randrange(INT_MIN, INT_MAX + 1) for _ in range(40)]:
            run(op, a, 0, True)
    return [family("vm_arith", "script VM 0x41a790 integer opcodes (PUSHI a; PUSHI b; OP)",
                   ["op", "a", "b", "crash", "result"], cases,
                   notes="ops: " + ", ".join(f"{k}={v}" for k, v in {**BINARY, **UNARY}.items()) +
                         "; unary ops ignore b; crash=1: the original raises a CPU exception (#DE)")]
