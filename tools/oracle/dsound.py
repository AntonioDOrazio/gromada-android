"""Fake DirectSound for the game-level oracle (issue #13).

gromada.exe's sound object (0x42e0c8, docs: src/game/sound_channels.hpp) talks to
IDirectSound / IDirectSoundBuffer through their vtables. These fakes let the original code
run its real scheduler (queue 0x408dd0, channel choice 0x408910, Sound::Tact 0x408f20) and
record what it asks DirectSound to do:

- DirectSoundCreate (DSOUND ordinal 1) returns a fake IDirectSound; CreateSoundBuffer and
  DuplicateSoundBuffer return fake buffers; Lock hands out a scratch area (the sample copy
  of 0x408b30 runs but nothing is heard);
- Play/Stop/SetVolume/SetPan are logged as channel operations (channel = index of the
  buffer in the object's 8 channel slots);
- GetStatus models playback with the game clock: a buffer plays for bytes*1000/avgBytes ms
  after Play (looping buffers until Stop). tools/game_trace.cpp uses the same model for our
  core, so both schedulers see the same finished channels.
"""
import struct

SND = 0x42e0c8              # the Sound object
CURCLOCK = 0x42bf94
CHANNELS = 0x210            # 8 x {active, buffer, nsfx, time}
DSOUND_METHODS = 11
BUFFER_METHODS = 21
SCRATCH_SIZE = 0x400000


class FakeDirectSound:
    def __init__(self, o):
        self.o = o
        self.buffers = {}          # address -> dict(bytes, avg, playing, loop, start)
        self.ops = []              # channel operations of the current frame
        self.scratch = o.alloc(SCRATCH_SIZE)
        ds = [None] * DSOUND_METHODS
        ds[0] = (self._fail, 3)                # QueryInterface
        ds[1] = (self._one, 1)                 # AddRef
        ds[2] = (self._one, 1)                 # Release
        ds[3] = (self._create_buffer, 4)
        ds[4] = (self._zero, 2)                # GetCaps
        ds[5] = (self._duplicate, 3)
        ds[6] = (self._zero, 3)                # SetCooperativeLevel
        ds[7] = (self._zero, 1)                # Compact
        ds[8] = (self._zero, 2)                # GetSpeakerConfig
        ds[9] = (self._zero, 2)                # SetSpeakerConfig
        ds[10] = (self._zero, 2)               # Initialize
        self.ds_vtable = self._vtable(ds)
        b = [None] * BUFFER_METHODS
        b[0] = (self._fail, 3)                 # QueryInterface
        b[1] = (self._one, 1)                  # AddRef
        b[2] = (self._release, 1)
        b[3] = (self._zero, 2)                 # GetCaps
        b[4] = (self._position, 3)             # GetCurrentPosition
        b[5] = (self._zero, 4)                 # GetFormat
        b[6] = (self._zero, 2)                 # GetVolume
        b[7] = (self._zero, 2)                 # GetPan
        b[8] = (self._zero, 2)                 # GetFrequency
        b[9] = (self._status, 2)
        b[10] = (self._zero, 3)                # Initialize
        b[11] = (self._lock, 8)
        b[12] = (self._play, 4)
        b[13] = (self._zero, 2)                # SetCurrentPosition
        b[14] = (self._zero, 2)                # SetFormat
        b[15] = (self._volume, 2)
        b[16] = (self._pan, 2)
        b[17] = (self._zero, 2)                # SetFrequency
        b[18] = (self._stop, 1)
        b[19] = (self._zero, 5)                # Unlock
        b[20] = (self._zero, 1)                # Restore
        self.buf_vtable = self._vtable(b)
        self.ds = o.alloc(16)
        o.w32(self.ds, self.ds_vtable)

    def _vtable(self, methods):
        vt = self.o.alloc(4 * len(methods))
        for i, (fn, nargs) in enumerate(methods):
            self.o.w32(vt + 4 * i, self.o.new_stub(lambda o, a, fn=fn: fn(a), popped=4 * nargs))
        return vt

    # ---------------------------------------------------------------- helpers
    def clock(self):
        return self.o.u32(CURCLOCK)

    def channel_of(self, buf):
        for c in range(8):
            if self.o.u32(SND + CHANNELS + 16 * c + 4) == buf:
                return c, self.o.s32(SND + CHANNELS + 16 * c + 8)
        return -1, -1

    def _new_buffer(self, nbytes, avg):
        p = self.o.alloc(16)
        self.o.w32(p, self.buf_vtable)
        self.buffers[p] = {"bytes": nbytes, "avg": avg, "playing": False, "loop": False, "start": 0}
        return p

    # ---------------------------------------------------------------- IDirectSound
    def _fail(self, a):
        return 0x80004002      # E_NOINTERFACE

    def _one(self, a):
        return 1

    def _zero(self, a):
        return 0

    def _create_buffer(self, a):
        desc, out = a(1) & 0xFFFFFFFF, a(2) & 0xFFFFFFFF
        nbytes = self.o.u32(desc + 8)
        wfx = self.o.u32(desc + 16)
        avg = self.o.u32(wfx + 8) if wfx else 0
        self.o.w32(out, self._new_buffer(nbytes, avg))
        return 0

    def _duplicate(self, a):
        src, out = a(1) & 0xFFFFFFFF, a(2) & 0xFFFFFFFF
        s = self.buffers.get(src)
        if s is None:
            # 0x408a2b also tries the channel being allocated, whose buffer is still null:
            # DirectSound refuses, the scheduler then creates a buffer
            return 0x80070057      # DSERR_INVALIDPARAM
        self.o.w32(out, self._new_buffer(s["bytes"], s["avg"]))
        return 0

    # ---------------------------------------------------------------- IDirectSoundBuffer
    def _release(self, a):
        return 0

    def _position(self, a):
        for k in (1, 2):
            if a(k):
                self.o.w32(a(k) & 0xFFFFFFFF, 0)
        return 0

    def playing(self, b):
        if not b["playing"]:
            return False
        if b["loop"]:
            return True
        dur = b["bytes"] * 1000 // b["avg"] if b["avg"] else 0
        return ((self.clock() - b["start"]) & 0xFFFFFFFF) < dur

    def _status(self, a):
        b = self.buffers.get(a(0) & 0xFFFFFFFF)
        st = 0
        if b is not None and self.playing(b):
            st = 1 | (4 if b["loop"] else 0)
        self.o.w32(a(1) & 0xFFFFFFFF, st)
        return 0

    def _lock(self, a):
        nbytes = a(2) & 0xFFFFFFFF
        if nbytes > SCRATCH_SIZE:
            raise RuntimeError(f"sound buffer of {nbytes} bytes: scratch area too small")
        self.o.w32(a(3) & 0xFFFFFFFF, self.scratch)
        self.o.w32(a(4) & 0xFFFFFFFF, nbytes)
        if a(5):
            self.o.w32(a(5) & 0xFFFFFFFF, 0)
        if a(6):
            self.o.w32(a(6) & 0xFFFFFFFF, 0)
        return 0

    def _play(self, a):
        buf = a(0) & 0xFFFFFFFF
        b = self.buffers[buf]
        loop = bool(a(3) & 1)
        b.update(playing=True, loop=loop, start=self.clock())
        ch, nsfx = self.channel_of(buf)
        # SetPan + SetVolume just before Play on the same buffer belong to the play
        att, pan = b.get("vol", 0), b.get("pan", 0)
        if self.ops and self.ops[-1][0] == "u" and self.ops[-1][1] == ch:
            self.ops.pop()
        self.ops.append(["p", ch, nsfx, att, pan, int(loop)])
        return 0

    def _stop(self, a):
        buf = a(0) & 0xFFFFFFFF
        b = self.buffers.get(buf)
        # Only audible stops are logged: Sound::Tact stops every inactive channel holding a
        # looping sound again at every frame (0x409052 does not test the active flag)
        if b is not None and self.playing(b):
            ch, nsfx = self.channel_of(buf)
            self.ops.append(["s", ch, nsfx])
        if b is not None:
            b["playing"] = False
        return 0

    def _pan(self, a):
        b = self.buffers.get(a(0) & 0xFFFFFFFF)
        if b is not None:
            b["pan"] = a(1)
        return 0

    def _volume(self, a):
        buf = a(0) & 0xFFFFFFFF
        b = self.buffers.get(buf)
        if b is not None:
            b["vol"] = a(1)
            ch, nsfx = self.channel_of(buf)
            # the scheduler always sets the pan first, then the volume (0x408fe2, 0x40910a)
            self.ops.append(["u", ch, nsfx, a(1), b.get("pan", 0)])
        return 0
