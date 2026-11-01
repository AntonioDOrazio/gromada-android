#!/usr/bin/env python3
"""Dump the simulation-relevant fields of every Vid and Weapon in fw.res.

Used to cross-check docs/re/engine.md (tick periods, speeds, weapons) against real data.
Prints a table to stdout; never commit its output (it is derived from the game data).

Usage: python3 -I tools/re/vid_stats.py [path/to/fw.res] [--weapons] [--summary]
Default path: $GROMADA_DATA/fw.res (or /root/gromada-data/game/fw.res).

Vid header layout (file == in-memory layout of gromada.exe, struct of 0x136 bytes, see
docs/re/engine.md "Vid in memoria"):
  0x00 name[34] 0x22 category 0x23 class 0x24 flags(u16) 0x26 collisionMask 0x27 sizeX
  0x29 sizeY 0x2b sizeZ 0x2d maxHP 0x2e visibilityRadius 0x30 unused 0x31 speedX 0x33 speedY
  0x35 acceleration 0x37 rotationPeriod/stepLength 0x38 army 0x39 weapon 0x3a unused
  0x3b deathDamageRadius 0x3d deathDamage 0x3e linkX 0x3f linkY 0x40 linkZ 0x41 linkedVid
  0x43 unused 0x45 directionsCount 0x46 zLayer 0x47 animLen[16] 0x57 nsfx[16]
  0x77 childOffX[16] 0x97 childOffY[16] 0xb7 childOffZ[16] 0xd7 childNvid[16]
  0xf7 childCount[16] 0x107 dataSizeOrNvid(i32) then graphics header:
  dataFormat(u8) frameDuration(u16) numOfFrames(u16) dataSize(u32) width(u16) height(u16)
"""
import collections
import os
import struct
import sys

CLASSES = ["Terrain", "Static", "Vehicle", "Building", "AviaVehicle", "Projectile", "CannonMissile",
           "DownedAvia", "NA", "Superstructure", "Image", "Effect1", "Effect2", "Mine", "Shell",
           "AviaMissile", "Debris", "Kassandra", "Bonus", "Font", "RepairCannon"]


def sections(data):
    count = struct.unpack_from("<I", data, 0)[0]
    pos = 4
    for _ in range(count):
        stype, nxt, elems, doff = struct.unpack_from("<BIIH", data, pos)
        if nxt == 0:
            break
        yield stype, elems, pos + 11 + doff, pos + 5 + nxt
        pos += 5 + nxt


def parse_vid(data, start):
    h = data[start:start + 0x10b]
    v = {
        "name": h[:34].split(b"\0", 1)[0].decode("cp866", "replace"),
        "category": h[0x22], "cls": h[0x23], "flags": struct.unpack_from("<H", h, 0x24)[0],
        "collisionMask": h[0x26],
        "sizeX": struct.unpack_from("<h", h, 0x27)[0], "sizeY": struct.unpack_from("<h", h, 0x29)[0],
        "sizeZ": struct.unpack_from("<h", h, 0x2b)[0], "maxHP": h[0x2d],
        "visibility": struct.unpack_from("<H", h, 0x2e)[0],
        "speedX": struct.unpack_from("<h", h, 0x31)[0], "speedY": struct.unpack_from("<h", h, 0x33)[0],
        "accel": struct.unpack_from("<H", h, 0x35)[0], "rot": struct.unpack_from("<b", h, 0x37)[0],
        "army": h[0x38], "weapon": h[0x39],
        "deathRadius": struct.unpack_from("<H", h, 0x3b)[0], "deathDamage": h[0x3d],
        "linkedVid": struct.unpack_from("<H", h, 0x41)[0], "dirs": h[0x45], "zLayer": h[0x46],
        "animLen": list(h[0x47:0x57]),
        "childNvid": list(struct.unpack_from("<16h", h, 0xd7)),
        "childCount": list(h[0xf7:0x107]),
    }
    ds = struct.unpack_from("<i", h, 0x107)[0]
    if ds < 0:
        v["graphicsFrom"] = -ds
        v["frameDuration"] = None
    else:
        fmt, fd, nf = struct.unpack_from("<BHH", data, start + 0x10b)
        v["frameDuration"] = fd
        v["frames"] = nf
    return v


def main(argv):
    args = [a for a in argv if not a.startswith("--")]
    path = args[0] if args else os.path.join(os.environ.get("GROMADA_DATA", "/root/gromada-data/game"), "fw.res")
    data = open(path, "rb").read()
    vids, weapons = [], []
    for stype, elems, begin, end in sections(data):
        if stype == ord("!"):
            vids.append(parse_vid(data, begin))
        elif stype == ord("#"):
            for k in range(elems):
                tc, fl, z, rng, sc, cd = struct.unpack_from("<BBHHBH", data, begin + 9 * k)
                weapons.append((k, tc, fl, rng, sc, cd))
    # graphics shared from another vid: frameDuration comes from the source vid (exe 0x406604)
    for v in vids:
        if v["frameDuration"] is None and 0 <= v["graphicsFrom"] < len(vids):
            v["frameDuration"] = vids[v["graphicsFrom"]]["frameDuration"]
    if "--weapons" in argv:
        print("nweapon targetCategory flags range scatter cooldown(x10ms)")
        for w in weapons:
            print("%3d 0x%02x 0x%02x %5d %3d %5d" % w)
        return
    if "--summary" in argv:
        by_cls = collections.defaultdict(list)
        for v in vids:
            by_cls[v["cls"]].append(v["frameDuration"] or 0)
        for c, fds in sorted(by_cls.items()):
            name = CLASSES[c] if c < len(CLASSES) else str(c)
            hist = collections.Counter(fds).most_common(6)
            print(f"{name:15} n={len(fds):3} frameDuration(ms) most common: {hist}")
        return
    print("nvid class           tick(ms) speedX speedY accel rot dirs hp  wpn death/r  name")
    for i, v in enumerate(vids):
        c = CLASSES[v["cls"]] if v["cls"] < len(CLASSES) else str(v["cls"])
        print(f"{i:4} {c:15} {v['frameDuration'] or 0:7} {v['speedX']:6} {v['speedY']:6} {v['accel']:5} "
              f"{v['rot']:3} {v['dirs']:4} {v['maxHP']:3} {v['weapon']:4} {v['deathDamage']:3}/{v['deathRadius']:<4} {v['name']}")


if __name__ == "__main__":
    main(sys.argv[1:])
