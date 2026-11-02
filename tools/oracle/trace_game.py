#!/usr/bin/env python3
"""Game-level oracle (issue #13): replays a recorded demo with gromada.exe's own code.

The whole executable runs inside unicorn (see oracle.py for the image mapping and the stub
mechanism, winenv.py for the Win32 environment):

1. the statically linked MSVC CRT starts normally (heap, stdio, static constructors) up to
   WinMain (0x401550), which is not executed (window, DirectDraw, DirectPlay);
2. Field::Init (0x415af0) loads fw.res (vids, weapons), the rotation tables and the players,
   like WinMain does after creating the window; Sound::Init gets a fake DirectSound
   (dsound.py) that records the channel operations;
3. Field::LoadMap (0x4149b0) loads `maps\\demoN.dem`: the original code reads the demo
   header, loads the map with the recorded seed and clock, compiles the script and restores
   the recorded int cells;
4. every frame calls Field::Tact (0x4141f0), the body of the main loop: the original reads
   the clock and the 12-byte input of the next record, runs the script main(), World::Tact,
   the menus, the scroll, the players, the squads and the sound tact. timeGetTime advances
   by 1 s per call, so the demo pacing never waits and drawThisFrame stays 0 (World::Tact
   does not draw).

After each frame the world is sampled into one JSON line (see docs/trace-oracle.md):
clock, rand() state, sprite count, a CRC per z layer over every sprite, the detailed state
of the "dynamic" sprites (everything but terrain, static scenery, menus and texts) and the
builtin calls made by the script during the frame, the sound requests (0x408dd0) and the
DirectSound channel operations of Sound::Tact (0x408f20) and StopSFX. tools/game_trace.cpp writes the same
format from our session + engine; tools/oracle/compare_traces.py compares them.

Usage:
    /root/tools/oracle-venv/bin/python -I tools/oracle/trace_game.py 1 -o /path/demo1.orig.jsonl
        [--frames N] [--full-at F,F,...] [--verbose]

The trace contains game data (positions, nvids, script strings): keep it out of the repo.
"""
import argparse
import gzip
import json
import os
import re
import struct
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import oracle as oracle_mod  # noqa: E402
from oracle import Oracle, s32  # noqa: E402
from unicorn import UC_HOOK_CODE, UcError  # noqa: E402
from unicorn.x86_const import UC_X86_REG_EIP  # noqa: E402
from dsound import FakeDirectSound  # noqa: E402
from winenv import VIRT_BASE, VIRT_SIZE, WINHEAP_BASE, WinEnv, resolve_ci  # noqa: E402

ENTRY = 0x41d2bb            # CRT entry point
WINMAIN = 0x401550
FIELD = 0x439d68            # the Field object (this of Field::*)
FIELD_INIT = 0x415af0
LOAD_MAP = 0x4149b0
FIELD_TACT = 0x4141f0
SOUND_INIT = 0x4083f0
SOUND_REQUEST = 0x408dd0    # PlaySFX / PlayPositional(nsfx, pan, attenuation), thiscall
SPRITE_INIT = 0x40bf30
BUILTIN_DISPATCH = 0x412b30
BUILTIN_RETURN = 0x41b118   # instruction after the dispatcher call in the VM
CURSOR_SPRITE = 0x440900

CURCLOCK, RAND_STATE = 0x42bf94, 0x42aebc
LAYERS = 0x439cf8
VIDS_PTR, NVIDS = 0x42df57, 0x42df4f
VID_SIZE = 0x136
CAMERA = 0x439cf4           # s16 x, s16 y
SCREEN_W, SCREEN_H = 0x42da00, 0x42da04
FIELD_DEMO_PLAY = 0x32f3
WM_COMMAND, CMD_LOAD = 0x111, 0x9c4c

# vtable -> C++ class (docs/re/engine.md §2.3, CreateUnit 0x40e1b0)
VTABLE_CLASS = {0x426208: 0, 0x426214: 1, 0x42620c: 2, 0x426210: 3, 0x426204: 4, 0x426200: 5,
                0x4261fc: 6, 0x4261f8: 7, 0x4261f4: 8, 0x4261c4: 9, 0x4261f0: 10, 0x4261ec: 11,
                0x4261e8: 12, 0x4261e4: 13, 0x4261e0: 14, 0x4261dc: 15, 0x4261d8: 16, 0x4261d4: 17,
                0x4261d0: 18, 0x4261cc: 19, 0x4261c8: 20}
NO_HP_CLASSES = {9, 10, 12}           # base SPRITE only (0x3f bytes): no hp byte
UNIT_CLASSES = {2, 3, 4, 13, 17}      # target/army/behave/waypoint extension
SESSION_CLASSES = {10, 19}            # menus and texts: session objects in our core
STATIC_CLASSES = {0, 1}               # terrain and scenery: only in the layer CRCs

# Sprite record layout shared with tools/game_trace.cpp (one JSON array per sprite):
SPRITE_FIELDS = ["uid", "layer", "cls", "nvid", "x", "y", "z", "dir", "act", "frame", "state",
                 "hp", "speed", "vz", "goX", "goY", "goZ", "timer", "lastTact", "ncmd", "target",
                 "parent"]
SPRITE_FMT = "<hhhBBHhhhHIhh"         # x .. vz from +0x0c


def crc_record(nvid, x, y, z, d, act, frame, state):
    """Bytes hashed per sprite in the layer CRCs (little endian, 14 bytes)."""
    return struct.pack("<HhhhBBhH", nvid, x, y, z, d, act, frame, state)


class Game:
    def __init__(self, game_dir, verbose=False):
        # No instruction limit: unicorn implements `count` with a per-instruction hook, which
        # makes the emulation ~50x slower (4 M instead of 200 M instructions per second).
        oracle_mod.MAX_INSNS = 0
        self.game_dir = game_dir
        self.o = Oracle(os.path.join(game_dir, "gromada.exe"))
        self.verbose = verbose
        self.win = WinEnv(self.o, game_dir, log=self.log)
        self.uid = {}            # sprite pointer -> uid (creation order since the map load)
        self.next_uid = 1
        self.builtins = []       # calls of the current frame
        self.pending = None
        self.posted = []         # PostMessage(WM_COMMAND, CMD_LOAD, name) of the frame
        self.audio = []          # sound requests (0x408dd0) and channel operations of the frame
        self.arity = self._read_externs()
        # Hooks must exist before the code they watch is first translated: unicorn does not
        # re-translate cached blocks when a hook is added later.
        uc = self.o.uc
        uc.hook_add(UC_HOOK_CODE, self._on_init, begin=SPRITE_INIT, end=SPRITE_INIT)
        uc.hook_add(UC_HOOK_CODE, self._on_builtin, begin=BUILTIN_DISPATCH, end=BUILTIN_DISPATCH)
        uc.hook_add(UC_HOOK_CODE, self._on_builtin_ret, begin=BUILTIN_RETURN, end=BUILTIN_RETURN)
        uc.hook_add(UC_HOOK_CODE, self._on_sound_request, begin=SOUND_REQUEST, end=SOUND_REQUEST)

    def log(self, s):
        if self.verbose:
            print("[win]", s, file=sys.stderr)

    # ------------------------------------------------------------------ setup
    def _read_externs(self):
        """Parameters of each builtin (the extern lines of maps\\HEADER.INI)."""
        path = resolve_ci(self.game_dir, "maps/HEADER.INI")
        arity = {}
        with open(path, "rb") as f:
            for line in f.read().decode("latin-1").splitlines():
                m = re.match(r"\s*extern\s+(\w+)\s*\(([^)]*)\)\s*(\d+)", line)
                if m:
                    params = [p.strip() for p in m.group(2).split(",") if p.strip()]
                    arity[int(m.group(3))] = [p.split()[0] for p in params]
        return arity

    def run_until(self, begin, stop):
        o = self.o
        hit = []

        def h(uc, address, size, user):
            hit.append(address)
            uc.emu_stop()
        hook = o.uc.hook_add(UC_HOOK_CODE, h, begin=stop, end=stop)
        o.fault = None
        try:
            o.uc.emu_start(begin, 0xFFFFFFFF)
        except UcError as e:
            if o.fault is None:
                o.fault = RuntimeError(f"unicorn: {e} at eip {o.uc.reg_read(UC_X86_REG_EIP):#x}")
        finally:
            o.uc.hook_del(hook)
        if o.fault is not None:
            f, o.fault = o.fault, None
            raise f
        if not hit:
            raise RuntimeError(f"stop address {stop:#x} not reached")

    def startup(self):
        """CRT startup up to WinMain, then the part of WinMain the simulation needs."""
        o, w = self.o, self.win
        o.set_reg("esp", 0x20100000 - 0x100)
        self.run_until(ENTRY, WINMAIN)
        u = "USER32.dll!"
        w.const(u + "LoadCursorFromFileA", 1, 0x5001)
        w.const(u + "LoadCursorA", 2, 0x5002)
        w.const(u + "DestroyCursor", 1, 1)
        w.const(u + "SetCursor", 1, 0)
        w.stub(u + "PostMessageA", 4, self._post_message)
        w.stub("WINMM.dll!mciSendStringA", 4, self._mci)
        o.w32(0x42be6c, 0x400000)                       # hInstance
        # Screen size, set by SVGA::Init (0x404a00, DirectDraw: not run). The demos were
        # recorded at 640x480 (recorded screen mouse < 640x480, world - screen = camera).
        o.w32(SCREEN_W, 640)
        o.w32(SCREEN_H, 480)
        # Sound::Init (0x4083f0) runs with a fake DirectSound and no mixer/CD volume devices
        self.dsound = FakeDirectSound(o)
        self.dsound.ops = self.audio
        w.stub("DSOUND.dll!1", 3, self._direct_sound_create)
        for name in ("waveOutGetNumDevs", "auxGetNumDevs"):
            w.const("WINMM.dll!" + name, 0, 0)
        o.call(FIELD_INIT, [0x1234], conv="thiscall", this=FIELD)

    def _direct_sound_create(self, a):
        self.o.w32(a(1) & 0xFFFFFFFF, self.dsound.ds)
        return 0

    def _on_sound_request(self, uc, address, size, user):
        esp = self.o.reg("esp")
        self.audio.append(["r", self.o.s32(esp + 4), self.o.s32(esp + 8), self.o.s32(esp + 12)])

    def _post_message(self, a):
        msg, wp, lp = a(1), a(2), a(3) & 0xFFFFFFFF
        text = self.win.cstr(lp) if msg == WM_COMMAND and wp == CMD_LOAD else f"msg {msg:#x}"
        self.log(f"PostMessageA({msg:#x}, {wp:#x}, {text!r})")
        self.posted.append(text)
        return 1

    def _mci(self, a):
        cmd = self.win.cstr(a(0) & 0xFFFFFFFF)
        self.log(f"mciSendStringA({cmd!r})")
        if a(1) and a(2) > 0:
            self.o.w8(a(1) & 0xFFFFFFFF, 0)
        return 0

    # ------------------------------------------------------------------ hooks
    def vid_class(self, nvid):
        return self.o.u8(self.o.u32(VIDS_PTR) + VID_SIZE * nvid + 0x23)

    def _on_init(self, uc, address, size, user):
        o = self.o
        p = o.reg("ecx")
        nvid = o.u32(o.reg("esp") + 4) & 0xFFFF
        if p == CURSOR_SPRITE or nvid >= o.u32(NVIDS) or self.vid_class(nvid) in SESSION_CLASSES:
            self.uid.pop(p, None)
            return
        self.uid[p] = self.next_uid
        self.next_uid += 1

    def _cell(self, logic, i):
        o = self.o
        flag = o.u8(logic + 0x11c + 5 * i)
        v = o.u32(logic + 0x11d + 5 * i)
        if flag & 1:
            return self.win.cstr(v, 256)
        return self.handle(s32(v))

    def handle(self, v):
        """Script value: sprite pointers become uids, other heap objects "S"."""
        p = v & 0xFFFFFFFF
        if WINHEAP_BASE <= p < VIRT_BASE + VIRT_SIZE:
            # a sprite (also a destroyed one: uids are never forgotten), otherwise an object
            # of the session in our core (texts, menus)
            return self.uid.get(p, "S")
        return v

    def _on_builtin(self, uc, address, size, user):
        o = self.o
        logic = o.reg("ecx")
        bid = o.u32(o.reg("esp") + 4) & 0xFF
        sp = o.s32(logic + 0x114)
        n = len(self.arity.get(bid, []))
        args = [self._cell(logic, i) for i in range(sp - n, sp)]
        self.pending = (bid, args, logic, sp - n)

    def _on_builtin_ret(self, uc, address, size, user):
        if self.pending is None:
            return
        bid, args, logic, base = self.pending
        self.pending = None
        sp = self.o.s32(logic + 0x114)
        rec = [bid, args]
        if sp == base + 1:
            rec.append(self._cell(logic, base))
        self.builtins.append(rec)

    # ------------------------------------------------------------------ game
    def load(self, name):
        o = self.o
        self.uid.clear()
        self.next_uid = 1
        p = o.alloc(len(name) + 1)
        o.write(p, name.encode("latin-1") + b"\0")
        o.call(LOAD_MAP, [p], conv="thiscall", this=FIELD)
        self.builtins = []

    def demo_playing(self):
        return self.o.u32(FIELD + FIELD_DEMO_PLAY) != 0

    def frame(self):
        self.builtins = []
        self.posted = []
        self.audio.clear()       # shared with the fake DirectSound
        self.o.call(FIELD_TACT, [1], conv="thiscall", this=FIELD)

    # ------------------------------------------------------------------ sampling
    def sample(self, index, full=False):
        o = self.o
        total = 0
        crcs = []
        sprites = []
        for layer in range(16):
            p = o.u32(LAYERS + 4 * layer)
            crc_data = []
            while p:
                b = o.read(p, 0x4c)
                vt, nxt, child = struct.unpack_from("<III", b, 0)
                cls = VTABLE_CLASS.get(vt, -1)
                x, y, z, d, act, nvid, frame, _, _, state = struct.unpack_from(SPRITE_FMT, b, 0x0c)[:10]
                if cls not in SESSION_CLASSES:
                    total += 1
                    crc_data.append(crc_record(nvid, x, y, z, d, act, frame, state))
                    if full or cls not in STATIC_CLASSES:
                        self._sprite(sprites, p, b, layer, cls, 0)
                        c, parent = child, self.uid.get(p, 0)
                        while c:
                            cb = o.read(c, 0x4c)
                            ccls = VTABLE_CLASS.get(struct.unpack_from("<I", cb, 0)[0], -1)
                            self._sprite(sprites, c, cb, -1, ccls, parent)
                            parent = self.uid.get(c, 0)
                            c = struct.unpack_from("<I", cb, 8)[0]
                p = nxt
            crcs.append(zlib.crc32(b"".join(crc_data)))
        return {"f": index, "clock": o.u32(CURCLOCK), "rng": o.u32(RAND_STATE), "n": total,
                "cam": [o.s16(CAMERA), o.s16(CAMERA + 2)], "crc": crcs, "s": sprites,
                "b": self.builtins, "a": list(self.audio)}

    def _sprite(self, out, p, b, layer, cls, parent):
        (x, y, z, d, act, nvid, frame, _, _, state, last_tact,
         speed, vz) = struct.unpack_from(SPRITE_FMT, b, 0x0c)
        go_x, go_y, go_z, timer = struct.unpack_from("<hhhH", b, 0x2d)
        cmds = struct.unpack_from("<I", b, 0x3b)[0]
        ncmd = self.o.s16(cmds) if cmds else 0
        hp = b[0x3f] if cls not in NO_HP_CLASSES else 0
        target = self.handle(struct.unpack_from("<i", b, 0x40)[0]) if cls in UNIT_CLASSES else 0
        out.append([self.uid.get(p, 0), layer, cls, nvid, x, y, z, d, act, frame, state, hp, speed,
                    vz, go_x, go_y, go_z, timer, last_tact, ncmd, target, parent])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("demo", help="demo number (1..3) or map path such as maps\\demo1.dem")
    ap.add_argument("-o", "--out", required=True, help="output .jsonl or .jsonl.gz (outside the repo)")
    ap.add_argument("--frames", type=int, default=0, help="stop after N frames (0 = whole demo)")
    ap.add_argument("--full-at", default="", help="frames where every sprite is dumped")
    ap.add_argument("--data", default=os.environ.get("GROMADA_DATA", "/root/gromada-data/game"))
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()
    name = f"maps\\demo{args.demo}.dem" if args.demo.isdigit() else args.demo
    full_at = {int(x) for x in args.full_at.split(",") if x}

    t0 = time.time()
    g = Game(args.data, verbose=args.verbose)
    g.startup()
    g.load(name)
    g.win.time_step = 1000
    opener = (lambda p: gzip.open(p, "wt", compresslevel=3)) if args.out.endswith(".gz") else (lambda p: open(p, "w"))
    with opener(args.out) as out:
        out.write(json.dumps({"trace": "gromada-game-trace", "version": 1, "source": "original",
                              "demo": name, "fields": SPRITE_FIELDS}) + "\n")
        i = 0
        while g.demo_playing() and (args.frames == 0 or i < args.frames):
            g.frame()
            if "exit" in g.posted:      # end of the records (or a key): the demo is over
                break
            rec = g.sample(i, full=i in full_at)
            if g.posted:
                rec["post"] = g.posted
            out.write(json.dumps(rec, separators=(",", ":")) + "\n")
            i += 1
            if i % 1000 == 0:
                print(f"frame {i} clock {rec['clock']} sprites {rec['n']} ({time.time() - t0:.1f}s)",
                      file=sys.stderr, flush=True)
            if g.posted:
                break               # the script asked for another map: the demo ends here
    print(f"{name}: {i} frames in {time.time() - t0:.1f}s -> {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
