"""MSVC CRT rand()/srand() (0x41c7b5/0x41c7ab) and the script builtin Random(max) (id 90)."""
from common import CpuFault, Fatal, edge_ints, family, pushi, rng, vm_program
from structs import RAND_STATE

SRAND, RAND = 0x41c7ab, 0x41c7b5
N_OUT = 48


def gen_sequences(o):
    r = rng("rand")
    seeds = [0, 1, 2, 0x8f, 0x00910dc1, 0x7fff, 0x8000, 0xffff, 0x10000, 0x7fffffff, 0x80000000,
             0xffffffff, 0xfffffffe, 214013, 2531011, 0x12345678, 0xdeadbeef]
    seeds += [r.getrandbits(32) for _ in range(160)]
    seeds += [r.randrange(0, 1 << 24) for _ in range(40)]  # timeGetTime()-like seeds
    cases = []
    for seed in seeds:
        o.call(SRAND, [seed])               # cdecl: caller pops
        outs = [o.call(RAND) for _ in range(N_OUT)]
        cases.append([seed] + outs + [o.u32(RAND_STATE)])
    return family("rand", "srand 0x41c7ab + rand 0x41c7b5 (cdecl)",
                  ["seed"] + [f"r{i}" for i in range(N_OUT)] + ["state"], cases,
                  notes="srand(seed) then 48 calls of rand(); state = 32-bit seed at 0x42aebc afterwards")


def gen_random_builtin(o):
    """Random(max) = rand() % (max+1) with idiv: max=-1 is a division by zero (crash),
    max+1 overflows for INT_MAX. Executed through the real VM (opcode 90)."""
    r = rng("random_builtin")
    maxes = edge_ints() + [r.randrange(-1000, 1000) for _ in range(150)] + \
        [r.randrange(-(1 << 31), 1 << 31) for _ in range(60)]
    cases = []
    for i, m in enumerate(maxes):
        for seed in (1, 0x00910dc1, r.getrandbits(32)):
            o.heap_reset()
            o.w32(RAND_STATE, seed)
            try:
                flag, value = vm_program(o, pushi(m) + bytes([90]))
                crash = 0
            except (CpuFault, Fatal):
                flag, value, crash = 0, 0, 1
            cases.append([seed, m, crash, value, o.u32(RAND_STATE)])
    return family("random_builtin", "script builtin Random(max) id 90 (0x4130a4) through the VM 0x41a790",
                  ["seed", "max", "crash", "result", "state"], cases,
                  notes="crash=1: the original raises #DE (max == -1)")


def generate(o):
    return [gen_sequences(o), gen_random_builtin(o)]
