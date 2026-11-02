"""Sound-effect system of the original (issue #18): the sound object at 0x42e0c8 driven through
its real functions with a fake DirectSound that records every operation.

Functions run unmodified (thiscall, `this` = a synthetic sound object, docs/re/runtime.md §9.1):
  0x4080c0 constructor            0x408dd0 queue / PlayPositional (nsfx, pan, att), ret 0xc
  0x408f20 Tact (end of frame)    0x408910 channel choice / LoadBuffer (called by Tact), ret 4
  0x409550 StopSFX()              0x408d80 StopSFX(n) / stop one effect, ret 4
  0x408bf0 channel release        0x408c50 PlayWAV(file), ret 4
  0x408b30 buffer fill (Lock/memcpy/Unlock, called by 0x408910)
and, on the real fw.res, 0x407640 (open the .res) + 0x408730 (descriptor table load) with
0x408570 (WAV loader); C runtime file functions are replaced by Python (real file).

Fake DirectSound: an IDirectSound whose vtable has CreateSoundBuffer (+0x0c) and
DuplicateSoundBuffer (+0x14) and an IDirectSoundBuffer vtable with Release (+0x08),
GetStatus (+0x24), Lock (+0x2c), Play (+0x30), SetCurrentPosition (+0x34), SetVolume (+0x3c),
SetPan (+0x40), Stop (+0x48), Unlock (+0x4c); every other slot raises UnexpectedCall. All
methods are stdcall (COM): the harness checks the bytes popped. A buffer is "playing" from Play
until Stop, Release, or a FINISH event chosen by the generator (only for non-looping buffers:
the sound reached its end). Stop keeps the play position, a finished buffer is back at 0.

Output: one row per sequence of events (flat integers, see SEQUENCE_FORMAT) with the
DirectSound operations each event produced and the final state of the channels and queue.
"""
import os
import struct
import zlib

from common import CpuFault, UnexpectedCall, rng
from oracle import exe_path, s32

CTOR, QUEUE, ALLOC, TACT = 0x4080c0, 0x408dd0, 0x408910, 0x408f20
STOP_ALL, STOP_ONE, RELEASE, PLAY_WAV = 0x409550, 0x408d80, 0x408bf0, 0x408c50
RES_OPEN, LOAD_TABLE, LOAD_WAV = 0x407640, 0x408730, 0x408570
RES_OBJECT, RES_FILE = 0x42df98, 0x42e0b0
CLOCK, WAV_NAME = 0x42bf94, 0x42e390
LOG_ERROR, FATAL_ERROR = 0x407fb0, 0x407f10
FOPEN, FCLOSE, FREAD, FSEEK, FTELL = 0x41bf48, 0x41bf5b, 0x41c122, 0x41c20a, 0x41bfca
MALLOC, FREE = 0x41bfb1, 0x41bfbf

SND_SIZE = 0x2d0
DESC = 27                      # descriptor: +0 data, +4 bytes, +8 type, +9 WAVEFORMATEX (18)
CH_BASE = 0x210                # 8 channels x 16: +0 active, +4 buffer, +8 nsfx, +0xc time
QUEUE_BASE = 0x8               # 32 entries x 16: +0 nsfx, +8 pan, +0xc attenuation
DISABLED, DSOUND = 0x2a4, 0x20c

DS_OK, DSERR_INVALIDPARAM = 0, 0x80070057
DSBSTATUS_PLAYING, DSBSTATUS_LOOPING = 1, 4
DSBPLAY_LOOPING = 1
BUFFER_FLAGS = 0xe2            # STATIC | CTRLFREQUENCY | CTRLPAN | CTRLVOLUME

# events (row encoding: code, fixed arguments, number of ops, ops)
EV_TACT, EV_REQUEST, EV_STOP_ALL, EV_STOP_ONE, EV_PLAY_WAV, EV_FINISH = 0, 1, 2, 3, 4, 5
EV_ARGS = {EV_TACT: 1, EV_REQUEST: 3, EV_STOP_ALL: 0, EV_STOP_ONE: 1, EV_PLAY_WAV: 3, EV_FINISH: 1}
# DirectSound operations (code, fixed arguments)
OP_STATUS, OP_PAN, OP_VOLUME, OP_PLAY, OP_STOP, OP_LOAD, OP_RELEASE, OP_SETPOS = 1, 2, 3, 4, 5, 6, 7, 8
OP_ARGS = {OP_STATUS: 2, OP_PAN: 2, OP_VOLUME: 2, OP_PLAY: 2, OP_STOP: 1, OP_LOAD: 3, OP_RELEASE: 1, OP_SETPOS: 2}
WAV_NOT_FOUND, WAV_INVALID, WAV_LOADED = 0, 1, 2

SEQUENCE_FORMAT = (
    "row = [N, type[0..N] (N+1 values), nEvents, events..., channels, queue]. "
    "event = [code, args..., nOps, ops...]: 0 TACT(clock), 1 REQUEST(nsfx, pan, att) -> 0x408dd0, "
    "2 STOPALL -> 0x409550, 3 STOPONE(nsfx) -> 0x408d80, 4 PLAYWAV(file, load, ret) -> 0x408c50 "
    "(file = name id, load 0 = fopen fails, 1 = opened but no WAV data, 2 = loaded; ret = return value), "
    "5 FINISH(ch) = the non-looping sound of channel ch reached its end (device event, no call). "
    "op = [code, args...]: 1 GetStatus(ch, status), 2 SetPan(ch, pan), 3 SetVolume(ch, att), "
    "4 Play(ch, flags), 5 Stop(ch), 6 LOAD(ch, nsfx, src) = CreateSoundBuffer+fill (src -1) or "
    "DuplicateSoundBuffer of channel src's buffer, 7 Release(ch), 8 SetCurrentPosition(ch, pos). "
    "channels = 8 x [active, hasBuffer, nsfx, time]; queue = [count, count x [index, nsfx, pan, att]] "
    "(entries with nsfx >= 0). Channels and queue are read after the last event.")


class Buf:
    __slots__ = ("ch", "nsfx", "playing", "looping", "data", "lock")

    def __init__(self, ch, nsfx, data):
        self.ch, self.nsfx, self.data = ch, nsfx, data
        self.playing = self.looping = False
        self.lock = None


class FakeDirectSound:
    """IDirectSound + IDirectSoundBuffer implemented by unicorn hooks (see module doc)."""

    BUF_METHODS = {0x08: ("Release", 4), 0x24: ("GetStatus", 8), 0x2c: ("Lock", 32), 0x30: ("Play", 16),
                   0x34: ("SetCurrentPosition", 8), 0x3c: ("SetVolume", 8), 0x40: ("SetPan", 8),
                   0x48: ("Stop", 4), 0x4c: ("Unlock", 20)}

    def __init__(self, o):
        self.o = o
        self.buffers = {}
        self.ops = []
        self.null_duplicates = 0
        self.snd = self.table = 0
        self.ds_vt = o.alloc(4 * 0x10)
        for i in range(0x10):
            o.w32(self.ds_vt + 4 * i, o.new_stub(self._unexpected(f"IDirectSound+{4 * i:#x}")))
        o.w32(self.ds_vt + 0x0c, o.new_stub(self._create, 16))
        o.w32(self.ds_vt + 0x14, o.new_stub(self._duplicate, 12))
        self.buf_vt = o.alloc(4 * 0x16)
        for i in range(0x16):
            name, popped = self.BUF_METHODS.get(4 * i, (None, 0))
            cb = getattr(self, "_" + name[0].lower() + name[1:]) if name else self._unexpected(f"IDirectSoundBuffer+{4 * i:#x}")
            o.w32(self.buf_vt + 4 * i, o.new_stub(cb, popped))

    def reset(self, snd, table):
        """New sequence: a fresh IDirectSound object for the sound object `snd`."""
        self.buffers.clear()
        self.ops = []
        self.snd, self.table = snd, table
        self.ds = self.o.alloc(8)
        self.o.w32(self.ds, self.ds_vt)
        return self.ds

    @staticmethod
    def _unexpected(name):
        def cb(o, args):
            raise UnexpectedCall(f"{name} called")
        return cb

    def _buf(self, args):
        b = self.buffers.get(args(0) & 0xFFFFFFFF)
        if b is None:
            raise UnexpectedCall(f"method called on a dead buffer {args(0) & 0xFFFFFFFF:#x}")
        return b

    def _channel_of_slot(self, out):
        ch, rem = divmod(out - (self.snd + CH_BASE + 4), 16)
        assert rem == 0 and 0 <= ch < 8, hex(out)
        return ch

    def _new_buffer(self, out, nsfx, data):
        ch = self._channel_of_slot(out)
        p = self.o.alloc(8)
        self.o.w32(p, self.buf_vt)
        self.buffers[p] = Buf(ch, nsfx, data)
        self.o.w32(out, p)
        return ch

    def desc_data(self, nsfx):
        d = self.table + DESC * nsfx
        ptr, size = self.o.u32(d), self.o.u32(d + 4)
        return self.o.read(ptr, size) if ptr and size else b""

    # IDirectSound
    def _create(self, o, args):
        this, desc, out, outer = (args(i) & 0xFFFFFFFF for i in range(4))
        assert this == self.ds and outer == 0
        size, flags, nbytes, reserved, wfx = struct.unpack("<5I", o.read(desc, 20))
        assert size == 0x14 and flags == BUFFER_FLAGS and reserved == 0, (size, flags, reserved)
        nsfx, rem = divmod(wfx - 9 - self.table, DESC)
        assert rem == 0 and nbytes == o.u32(self.table + DESC * nsfx + 4)
        ch = self._new_buffer(out, nsfx, self.desc_data(nsfx))
        self.ops.append([OP_LOAD, ch, nsfx, -1])
        return DS_OK

    def _duplicate(self, o, args):
        this, orig, out = (args(i) & 0xFFFFFFFF for i in range(3))
        assert this == self.ds
        if orig == 0:      # real DirectSound: DSERR_INVALIDPARAM, nothing written
            self.null_duplicates += 1
            return DSERR_INVALIDPARAM
        src = self.buffers[orig]
        assert o.u32(self.snd + CH_BASE + 16 * src.ch + 4) == orig
        ch = self._new_buffer(out, src.nsfx, src.data)
        self.ops.append([OP_LOAD, ch, src.nsfx, src.ch])
        return DS_OK

    # IDirectSoundBuffer
    def _release(self, o, args):
        b = self._buf(args)
        del self.buffers[args(0) & 0xFFFFFFFF]
        self.ops.append([OP_RELEASE, b.ch])
        return 0

    def _getStatus(self, o, args):
        b = self._buf(args)
        st = (DSBSTATUS_PLAYING | (DSBSTATUS_LOOPING if b.looping else 0)) if b.playing else 0
        o.w32(args(1) & 0xFFFFFFFF, st)
        self.ops.append([OP_STATUS, b.ch, st])
        return DS_OK

    def _lock(self, o, args):
        b = self._buf(args)
        off, nbytes, pp1, pl1, pp2, pl2, flags = (args(i) & 0xFFFFFFFF for i in range(1, 8))
        assert off == 0 and flags == 0 and nbytes == len(b.data) and b.lock is None
        b.lock = o.alloc(nbytes + 4)
        o.w32(pp1, b.lock)
        o.w32(pl1, nbytes)
        o.w32(pp2, 0)
        o.w32(pl2, 0)
        return DS_OK

    def _unlock(self, o, args):
        b = self._buf(args)
        p1, l1, p2, l2 = (args(i) & 0xFFFFFFFF for i in range(1, 5))
        assert p1 == b.lock and l1 == len(b.data) and p2 == 0 and l2 == 0
        assert o.read(p1, l1) == b.data, "buffer fill differs from the descriptor data"
        b.lock = None
        return DS_OK

    def _play(self, o, args):
        b = self._buf(args)
        assert args(1) == 0 and args(2) == 0
        flags = args(3) & 0xFFFFFFFF
        b.playing, b.looping = True, bool(flags & DSBPLAY_LOOPING)
        self.ops.append([OP_PLAY, b.ch, flags])
        return DS_OK

    def _setCurrentPosition(self, o, args):
        b = self._buf(args)
        self.ops.append([OP_SETPOS, b.ch, args(1)])
        return DS_OK

    def _setVolume(self, o, args):
        b = self._buf(args)
        self.ops.append([OP_VOLUME, b.ch, args(1)])
        return DS_OK

    def _setPan(self, o, args):
        b = self._buf(args)
        self.ops.append([OP_PAN, b.ch, args(1)])
        return DS_OK

    def _stop(self, o, args):
        b = self._buf(args)
        b.playing = False
        self.ops.append([OP_STOP, b.ch])
        return DS_OK


class SoundSystem:
    """A synthetic sound object (constructed by the original constructor) with N effects."""

    def __init__(self, o):
        self.o = o
        self.ds = FakeDirectSound(o)
        self.logs = 0
        self.frees = []
        self.wav = {"mode": WAV_LOADED, "file": 0x0F00F000}
        o.stub(LOG_ERROR, self._log)
        o.stub(FATAL_ERROR, self._fatal)
        o.stub(FREE, lambda o, args: self.frees.append(args(0) & 0xFFFFFFFF))
        o.stub(FOPEN, self._fopen)
        o.stub(FCLOSE, lambda o, args: 0)
        o.stub(LOAD_WAV, self._load_wav, popped=8)
        self.names = []
        for i in range(4):
            p = o.alloc(32)
            o.write(p, f"wav\\{i:02}.wav".encode() + b"\0")
            self.names.append(p)
        self.mark = o.heap_top

    def _log(self, o, args):
        self.logs += 1
        return 0

    @staticmethod
    def _fatal(o, args):
        raise UnexpectedCall("fatal error handler called")

    def _fopen(self, o, args):
        assert o.read(args(1) & 0xFFFFFFFF, 3) == b"rb\0"
        return 0 if self.wav["mode"] == WAV_NOT_FOUND else self.wav["file"]

    def _load_wav(self, o, args):
        """0x408570(index, FILE*) for PlayWAV: fills descriptor N like a valid (or invalid) WAV."""
        idx, f = args(0), args(1) & 0xFFFFFFFF
        assert idx == self.n and f == self.wav["file"]
        d = self.table + DESC * idx
        o.w32(d + 4, 0)
        if self.wav["mode"] == WAV_LOADED:
            size = self.wav["size"]
            p = o.alloc(size)
            o.write(p, bytes((7 * i + size) & 0xFF for i in range(size)))
            o.w32(d, p)
            o.w32(d + 4, size)
            o.write(d + 9, struct.pack("<HHIIHHH", 1, 1, 11025, 11025, 1, 8, 0))
        return 0

    def setup(self, types, r):
        """Fresh object: N = len(types) - 1 effects, descriptor i with type types[i]."""
        o = self.o
        o.heap_top = self.mark
        self.n = n = len(types) - 1
        snd = self.snd = o.alloc(SND_SIZE)
        o.call(CTOR, [], conv="thiscall", this=snd)   # a global in .bss: zero, then the constructor
        table = self.table = o.alloc(DESC * (n + 1))
        for i, t in enumerate(types):
            d = table + DESC * i
            if i < n:
                size = r.randrange(1, 48)
                p = o.alloc(size)
                o.write(p, bytes(r.randrange(256) for _ in range(size)))
                o.w32(d, p)
                o.w32(d + 4, size)
                o.write(d + 9, struct.pack("<HHIIHHH", 1, 1, 22050, 44100, 2, 16, 0))
            o.w8(d + 8, t)
        o.w32(snd, n)
        o.w32(snd + 4, table)
        o.w32(snd + DSOUND, self.ds.reset(snd, table))
        o.w32(snd + DISABLED, 0)
        o.write(WAV_NAME, b"\0" * 16)
        self.logs = 0
        self.frees = []

    # ---- calls into the original ------------------------------------------------------------
    def call(self, addr, args=()):
        self.ds.ops = []
        self.o.call(addr, list(args), conv="thiscall", this=self.snd)
        self.check()
        return self.ds.ops

    def tact(self, clock):
        self.o.w32(CLOCK, clock)
        return self.call(TACT)

    def request(self, nsfx, pan, att):
        return self.call(QUEUE, [nsfx, pan, att])

    def stop_all(self):
        return self.call(STOP_ALL)

    def stop_one(self, nsfx):
        return self.call(STOP_ONE, [nsfx])

    def play_wav(self, name, mode, r):
        self.wav["mode"] = mode
        self.wav["size"] = r.randrange(1, 40)
        self.ds.ops = []
        ret = self.o.call(PLAY_WAV, [self.names[name]], conv="thiscall", this=self.snd)
        self.check()
        return ret, self.ds.ops

    def finish(self, ch):
        b = self.ds.buffers[self.o.u32(self.snd + CH_BASE + 16 * ch + 4)]
        assert b.playing and not b.looping
        b.playing = False

    # ---- state ------------------------------------------------------------------------------
    def channels(self):
        o, out = self.o, []
        for i in range(8):
            c = self.snd + CH_BASE + 16 * i
            out.append((o.u32(c) != 0, o.u32(c + 4), o.s32(c + 8), o.u32(c + 12)))
        return out

    def queue(self):
        o, out = self.o, []
        for i in range(32):
            q = self.snd + QUEUE_BASE + 16 * i
            if o.s32(q) >= 0:
                out.append([i, o.s32(q), o.s32(q + 8), o.s32(q + 12)])
        return out

    def playing(self, ch):
        p = self.o.u32(self.snd + CH_BASE + 16 * ch + 4)
        b = self.ds.buffers.get(p)
        return bool(b and b.playing and not b.looping)

    def check(self):
        """Every buffer belongs to the channel slot that holds it; no leaks."""
        held = set()
        for i, (_, buf, _, _) in enumerate(self.channels()):
            if buf:
                assert buf in self.ds.buffers and self.ds.buffers[buf].ch == i, (i, hex(buf))
                held.add(buf)
        assert held == set(self.ds.buffers), "leaked DirectSound buffer"

    def snapshot(self):
        row = []
        for active, buf, nsfx, time in self.channels():
            row += [int(active), int(buf != 0), nsfx, time]
        q = self.queue()
        row.append(len(q))
        for e in q:
            row += e
        return row


# ---- sequences -----------------------------------------------------------------------------
class Recorder:
    """Builds one row: runs events on the original and appends them with their ops."""

    def __init__(self, s, types, r):
        self.s, self.r = s, r
        s.setup(types, r)
        self.row = [len(types) - 1] + list(types)
        self.events = []

    def _add(self, ev, ops):
        flat = list(ev) + [len(ops)]
        for op in ops:
            assert len(op) == 1 + OP_ARGS[op[0]]
            flat += op
        self.events.append(flat)

    def tact(self, clock):
        clock &= 0xFFFFFFFF
        self._add([EV_TACT, clock], self.s.tact(clock))

    def request(self, nsfx, pan=0, att=0):
        self._add([EV_REQUEST, s32(nsfx), s32(pan), s32(att)], self.s.request(nsfx, pan, att))

    def stop_all(self):
        self._add([EV_STOP_ALL], self.s.stop_all())

    def stop_one(self, nsfx):
        self._add([EV_STOP_ONE, s32(nsfx)], self.s.stop_one(nsfx))

    def play_wav(self, name, mode):
        ret, ops = self.s.play_wav(name, mode, self.r)
        self._add([EV_PLAY_WAV, name, mode, ret], ops)

    def finish(self, ch):
        self.s.finish(ch)
        self._add([EV_FINISH, ch], [])

    def finish_some(self, p):
        for ch in range(8):
            if self.s.playing(ch) and self.r.random() < p:
                self.finish(ch)

    def done(self):
        row = self.row + [len(self.events)]
        for e in self.events:
            row += e
        return row + self.s.snapshot()


def random_types(r, n):
    types = [r.choice([1, 40, 50, 90, 100, r.randrange(1, 256)])]          # sound 0 (never played)
    for _ in range(1, n):
        types.append(r.choices([0, 100, 1, 40, 50, 90, r.randrange(2, 256)], [25, 12, 25, 9, 8, 8, 13])[0])
    types.append(r.choice([0, 0, 100, r.randrange(256)]))                    # slot N (PlayWAV) before any WAV
    return types


def pick_pan(r):
    return r.choices([0, r.randrange(-3000, 3001), r.randrange(-12000, 12001), 10000, -10000, 10001, -10001,
                      r.randrange(-(1 << 31), 1 << 31), 1, -1], [6, 8, 5, 1, 1, 1, 1, 0.5, 0.5, 0.5])[0]


def pick_att(r):
    return r.choices([0, r.randrange(-2000, 1), r.randrange(-2100, 1), -2000, -2001, -1999, -1,
                      r.randrange(1, 30000), r.randrange(-(1 << 31), -2000), r.randrange(1, 1 << 31)],
                     [5, 10, 4, 1, 1, 1, 1, 0.6, 0.6, 0.3])[0]


def pick_dt(r):
    return r.choices([16, 17, 33, 39, 49, 50, 51, 0, r.randrange(1, 120), 499, 500, 501, 502,
                      r.randrange(120, 600), r.randrange(600, 5000), r.randrange(1 << 31, 1 << 32)],
                     [6, 6, 3, 4, 1, 1, 1, 1, 6, 0.7, 0.7, 0.7, 0.3, 3, 1, 0.2])[0]


def random_sequence(s, r):
    n = r.choices([r.randrange(2, 8), r.randrange(8, 20), r.randrange(33, 46)], [5, 4, 0.4])[0]
    types = random_types(r, n)
    rec = Recorder(s, types, r)
    hot = [r.randrange(1, n) for _ in range(r.randrange(1, 6))]
    if r.random() < 0.15:
        hot.append(n)                      # the PlayWAV slot
    clock = r.choice([0, r.randrange(0, 1000), r.randrange(0, 1 << 32), (1 << 32) - r.randrange(1, 3000)])
    frames = r.choices([r.randrange(2, 8), r.randrange(8, 20), r.randrange(20, 45)], [4, 5, 1])[0]
    p_finish = r.choice([0.0, 0.2, 0.5, 0.9])

    def nsfx():
        if r.random() < 0.8:
            return r.choice(hot)
        return r.choices([r.randrange(1, n + 1), 0, -1, n, n + 1, r.randrange(-(1 << 31), 1 << 31)],
                         [8, 1, 1, 1, 1, 0.3])[0]

    for _ in range(frames):
        rec.finish_some(p_finish)
        clock += pick_dt(r)
        k = r.choices([0, 1, 2, 3, 4, r.randrange(5, 12), r.randrange(12, 41)], [3, 5, 3, 2, 1, 1.2, 0.3])[0]
        calls = [("req",)] * k
        if r.random() < 0.05:
            calls.append(("stopall",))
        if r.random() < 0.04:
            calls.append(("stopone",))
        if r.random() < 0.04:
            calls.append(("wav",))
        r.shuffle(calls)
        for c in calls:
            if c[0] == "req":
                rec.request(nsfx(), pick_pan(r), pick_att(r))
            elif c[0] == "stopall":
                rec.stop_all()
            elif c[0] == "stopone":
                rec.stop_one(r.choice(hot + [n, r.randrange(-1, n + 2)]))
            else:
                rec.play_wav(r.randrange(3), r.choices([WAV_LOADED, WAV_NOT_FOUND, WAV_INVALID], [7, 1.5, 1.5])[0])
        rec.tact(clock)
        if r.random() < 0.05:
            rec.tact(clock + r.choice([0, 1, 50, 51]))
    return rec.done()


# ---- hand-written edge cases ---------------------------------------------------------------
def edge_sequences(s, r):
    rows = []

    def new(types):
        return Recorder(s, types, r)

    # A looping engine requested every `period` ms for a while, then no more: kept alive,
    # stopped when not requested for more than 500 ms (boundary 500/501), restarted later.
    for period in (16, 17, 33, 39, 50, 100, 250, 499, 500, 501, 520):
        for gap in (499, 500, 501, 502):
            rec = new([1, 0, 1, 100, 0, 7])
            clock = 1000
            for f in range(12):
                rec.request(1, 400 * f - 2000, -50 * f)
                rec.tact(clock)
                clock += period
            last = clock - period
            for t in (last + gap - 1, last + gap, last + gap + 1):
                rec.tact(t)
            rec.request(1)
            rec.tact(last + gap + 40)
            rec.request(1)
            rec.tact(last + gap + 120)
            rows.append(rec.done())
    # Restart guard of 0x408910 around 50 ms for a one-shot and for a loop, finished or not.
    for kind in (1, 0, 100):
        for dt in (0, 1, 49, 50, 51, 52):
            for finished in (False, True):
                rec = new([1, kind, kind, 1, 0])
                rec.request(1, 100, -300)
                rec.tact(5000)
                if finished and kind != 0:
                    rec.finish(0)
                rec.request(1, -100, -200)
                rec.tact(5000 + dt)
                rec.request(1)
                rec.tact(5000 + dt + 60)
                rows.append(rec.done())
    # More requests than free channels: weakest dropped (ties: the last), loops already
    # playing only re-panned, channels that finish this frame count as free.
    for variant in range(24):
        types = [1] + [r.choice([0, 1, 40, 100]) for _ in range(14)] + [0]
        rec = new(types)
        clock = r.randrange(0, 1 << 31)
        for f in range(4):
            if f:
                rec.finish_some(r.choice([0.0, 0.3, 1.0]))
            atts = [r.choice([0, -100, -100, -500, -2000, r.randrange(-2000, 1)]) for _ in range(r.randrange(6, 15))]
            for i, a in enumerate(atts):
                rec.request(r.randrange(1, 15), r.randrange(-500, 501), a)
            rec.tact(clock)
            clock += r.choice([17, 60, 600])
        rows.append(rec.done())
    # Positive attenuations (never produced by the game) in the drop loop: entry 0 free
    # compares with attenuation 0.
    for variant in range(8):
        types = [1] + [1] * 14 + [0]
        rec = new(types)
        for i in range(12):
            rec.request(i + 1, 0, r.choice([1, 5, 100, 0, -1]) if variant % 2 else r.randrange(1, 1000))
        if variant >= 4:
            rec.request(13, 0, 0)
        rec.tact(100)
        rows.append(rec.done())
    # Full queue: 40 distinct one-shots, then 40 requests of one loop (no merge for loops).
    for kind in (1, 0, 100):
        rec = new([1] + [kind] * 45 + [0])
        for i in range(40):
            rec.request(i + 1, i * 100, -i * 10)
        rec.tact(10)
        for i in range(40):
            rec.request(3, i, -i)
        rec.tact(1000)
        rows.append(rec.done())
    # Merging of equal requests (one-shot and non-positional), clamped pans, -2000/-2001.
    for kind in (1, 100, 0):
        for atts in ((-500, -300), (-2000, -2000), (0, 0), (-2001, -100), (-100, -2001), (-1999, -1, -1000),
                     (5000, -2000), (30000, 30000), (-2000, 7000)):
            for pans in ((-1000, 2000), (30000, -30000), (10000, 10000), (-10001, 3)):
                rec = new([1, kind, 1, 0])
                for i, a in enumerate(atts):
                    rec.request(1, pans[i % len(pans)], a)
                rec.request(2, 0, 0)
                rec.tact(777)
                rows.append(rec.done())
    # Invalid effect numbers; PlayWAV slot without a WAV.
    rec = new([1, 1, 0, 100, 1])
    for nsfx in (0, -1, 4, 5, 6, -(1 << 31), (1 << 31) - 1, 3, 1):
        rec.request(nsfx)
    rec.tact(5)
    rows.append(rec.done())
    # StopSFX(): channels stop (the PlayWAV slot keeps playing), the queue survives; a channel
    # stopped by StopSFX is reused later (Play resumes); StopSFX(n) releases.
    for order in range(6):
        rec = new([1, 0, 1, 100, 1, 0])
        rec.play_wav(0, WAV_LOADED)
        for nsfx in (1, 2, 3, 4):
            rec.request(nsfx, 100 * nsfx, -100 * nsfx)
        rec.tact(1000)
        rec.request(2)
        if order % 2:
            rec.stop_all()
        else:
            rec.stop_one(r.choice([1, 2, 5, 3]))
        rec.tact(1017)
        rec.stop_all()
        rec.request(1)
        rec.request(2)
        rec.tact(1100 + 50 * order)
        rec.stop_one(5)
        rec.stop_one(1)
        rec.request(1)
        rec.tact(1120 + 50 * order)
        rec.tact(1700 + 50 * order)
        rows.append(rec.done())
    # PlayWAV: same file while playing (nothing), another file, missing / invalid file, after
    # StopMusic (StopSFX(N)), looping and exempt from the 500 ms expiry.
    for seq in range(12):
        rec = new([1, 1, 0, 1, r.randrange(256)])
        clock = 100
        for step in range(8):
            what = r.choice(["same", "other", "missing", "invalid", "stop", "req", "finish", "stopall"])
            if what in ("same", "other", "missing", "invalid") or step == 0:
                name = 0 if what == "same" or step == 0 else r.randrange(3)
                mode = {"missing": WAV_NOT_FOUND, "invalid": WAV_INVALID}.get(what, WAV_LOADED)
                rec.play_wav(name, mode)
            elif what == "stop":
                rec.stop_one(4)
            elif what == "req":
                rec.request(4, r.randrange(-500, 500), -r.randrange(0, 2000))
            elif what == "stopall":
                rec.stop_all()
            rec.request(2)
            rec.tact(clock)
            clock += r.choice([17, 60, 700, 3000])
        rows.append(rec.done())
    # More than 8 channels busy with long one-shots: requests dropped, nothing cut; channels
    # freed one by one; reuse of a finished channel with the same sound; duplicates.
    for variant in range(10):
        rec = new([1] + [1] * 12 + [0])
        clock = 0
        for f in range(10):
            for i in range(r.randrange(1, 11)):
                rec.request(r.randrange(1, 13), 0, -r.randrange(0, 2000))
            rec.tact(clock)
            clock += r.choice([51, 100, 17])
            for ch in range(8):
                if s.playing(ch) and r.random() < 0.25:
                    rec.finish(ch)
        rows.append(rec.done())
    # Clock wrap-around (unsigned comparisons) and a clock that goes back.
    for start in ((1 << 32) - 30, (1 << 32) - 600, 10):
        rec = new([1, 0, 1, 0])
        for t in range(0, 1400, 39):
            if t < 600:
                rec.request(1)
            rec.request(2, 0, -100)
            rec.tact(start + t)
            if s.playing(1) and t % 3 == 0:
                rec.finish(1)
        rec.request(1)
        rec.tact(start)
        rec.request(1)
        rec.tact(start - 100)
        rows.append(rec.done())
    return rows


def gen_sequences(o, name, count, maker):
    r = rng(name)
    s = SoundSystem(o)
    rows, crashes = [], 0
    while len(rows) < count:
        try:
            rows.append(maker(s, r))
        except CpuFault:
            crashes += 1      # #DE in the merge division: an impossible input, regenerate
    assert crashes < count // 20, crashes
    return rows, s


def record(name, function, rows):
    return {"family": name, "function": function, "columns": ["sequence"], "notes": SEQUENCE_FORMAT, "cases": rows}


def gen_random(o, n=1100):
    rows, s = gen_sequences(o, "sound_random", n, random_sequence)
    print(f"  sound_random: duplicates with a NULL source {s.ds.null_duplicates}")
    return record("sound_random", "sound object 0x408dd0/0x408f20/0x408910/0x409550/0x408d80/0x408c50, random", rows)


def gen_edges(o):
    r = rng("sound_edge")
    s = SoundSystem(o)
    rows = edge_sequences(s, r)
    return record("sound_edge", "sound object 0x408dd0/0x408f20/0x408910/0x409550/0x408d80/0x408c50, edge cases", rows)


# ---- the descriptor table of the real fw.res (0x408730) --------------------------------------
def res_path():
    return os.path.join(os.path.dirname(exe_path()), "fw.res")


def gen_table(o):
    """Runs the original .res opener and 0x408730 on the real fw.res: per descriptor the type
    byte, size and format of the data and its CRC32 (numbers only)."""
    path = res_path()
    with open(path, "rb") as f:
        blob = f.read()
    region, size = 0x40000000, 0x04000000   # malloc of the WAV data (fw.res sounds: tens of MB)
    if not getattr(o, "_sound_heap", False):
        o.uc.mem_map(region, size)
        o._sound_heap = True
    state = {"pos": 0, "top": region, "frees": 0}
    FILE = 0x0F00F000

    def fopen(o, args):
        name = o.read(args(0) & 0xFFFFFFFF, 16).split(b"\0")[0]
        assert name == b"fw.res", name
        return FILE

    def fread(o, args):
        buf, sz, cnt, f = (args(i) & 0xFFFFFFFF for i in range(4))
        assert f == FILE
        n = min(sz * cnt, max(0, len(blob) - state["pos"]))
        o.write(buf, blob[state["pos"]:state["pos"] + n])
        state["pos"] += n
        return n // sz if sz else 0

    def fseek(o, args):
        f, off, whence = args(0) & 0xFFFFFFFF, args(1), args(2)
        assert f == FILE
        state["pos"] = {0: 0, 1: state["pos"], 2: len(blob)}[whence] + off
        return 0

    def ftell(o, args):
        return state["pos"]

    def malloc(o, args):
        p = (state["top"] + 15) & ~15
        state["top"] = p + max(args(0), 1)
        assert state["top"] <= region + size
        return p

    def free(o, args):
        state["frees"] += 1
        return 0

    def log(o, args):
        raise UnexpectedCall("error logged while loading fw.res")

    for addr, cb in ((FOPEN, fopen), (FREAD, fread), (FSEEK, fseek), (FTELL, ftell), (MALLOC, malloc),
                     (FREE, free), (LOG_ERROR, log), (FATAL_ERROR, log), (FCLOSE, lambda o, a: 0)):
        o.stub(addr, cb)
    o.unstub(LOAD_WAV)          # the real WAV loader this time
    name = o.alloc(16)
    o.write(name, b"fw.res\0")
    o.write(RES_OBJECT, b"\0" * 0x120)
    assert o.call(RES_OPEN, [name], conv="thiscall", this=RES_OBJECT) == 0
    assert o.u32(RES_FILE) == FILE
    snd = o.alloc(SND_SIZE)
    o.call(CTOR, [], conv="thiscall", this=snd)
    o.w32(snd + DISABLED, 0)
    o.call(LOAD_TABLE, [], conv="thiscall", this=snd)
    n, table = o.s32(snd), o.u32(snd + 4)
    assert n > 0 and table
    cases = []
    for i in range(n + 1):
        d = table + DESC * i
        ptr, nbytes, typ = o.u32(d), o.u32(d + 4), o.u8(d + 8)
        tag, ch, rate, avg, align, bits, cb = struct.unpack("<HHIIHHH", o.read(d + 9, 18))
        data = o.read(ptr, nbytes) if ptr and nbytes else b""
        if i == n:
            assert ptr == 0, "slot N must be empty (PlayWAV)"
            typ = tag = ch = rate = avg = align = bits = cb = nbytes = 0   # never written (malloc garbage)
        cases.append([i, typ, nbytes, tag, ch, rate, avg, align, bits, zlib.crc32(data)])
    print(f"  sound_table: N = {n}, {state['frees']} free() calls")
    return {"family": "sound_table", "function": "descriptor table of fw.res loaded by 0x408730 (via 0x407640)",
            "columns": ["nsfx", "type", "bytes", "wFormatTag", "nChannels", "nSamplesPerSec", "nAvgBytesPerSec",
                        "nBlockAlign", "wBitsPerSample", "crc32"],
            "notes": "N = last row (the PlayWAV slot: no data). crc32 of the PCM data loaded for the descriptor.",
            "cases": cases}


def generate(o):
    out = [gen_edges(o), gen_random(o)]
    if os.path.exists(res_path()):
        out.append(gen_table(o))
    else:
        print("  sound_table: fw.res not found, skipped")
    return out
