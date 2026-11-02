#!/usr/bin/env python3
"""Compares a game trace of the emulated original (trace_game.py) with one of our core
(tools/game_trace.cpp), frame by frame (issue #13, docs/trace-oracle.md).

    python3 -I tools/oracle/compare_traces.py ORIG.jsonl[.gz] CORE.jsonl[.gz] [--at F] [--json OUT]

Report: first divergent frame for each aspect (clock, rand() state, camera, sprite count,
layer CRCs, dynamic sprites, builtin calls, sound requests and channel operations), the first differing sprites/fields/calls with
their values, and statistics over the whole demo (frames with a difference, per field).
`--at F` prints every difference of frame F. The traces contain game data: keep them, and
the report, outside the repository.
"""
import argparse
import collections
import gzip
import json
import sys


def open_trace(path):
    f = gzip.open(path, "rt") if path.endswith(".gz") else open(path)
    head = json.loads(f.readline())
    return head, f


def frames(f):
    for line in f:
        if line.strip():
            yield json.loads(line)


def sprite_map(rec, fields):
    m = {}
    for s in rec["s"]:
        key = (s[0], s[fields.index("parent")]) if s[0] == 0 else s[0]
        m[key] = s
    return m


def describe(fields, s):
    return " ".join(f"{k}={v}" for k, v in zip(fields, s))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("orig")
    ap.add_argument("core")
    ap.add_argument("--at", type=int, action="append", default=[], help="print all differences of frame F")
    ap.add_argument("--max-examples", type=int, default=8)
    ap.add_argument("--ignore", default="", help="comma-separated sprite fields not compared")
    ap.add_argument("--json", help="write the summary as JSON")
    args = ap.parse_args()

    ho, fo = open_trace(args.orig)
    hc, fc = open_trace(args.core)
    fields = ho["fields"]
    assert fields == hc["fields"], "different sprite layouts"
    ignore = {x for x in args.ignore.split(",") if x}
    idx = {k: i for i, k in enumerate(fields)}
    cmp_fields = [k for k in fields if k not in ignore and k != "uid"]

    first = {}                       # aspect -> (frame, detail)
    count = collections.Counter()    # aspect -> frames with a difference
    field_frames = collections.Counter()
    field_first = {}
    nframes = 0
    examples = []
    plays = {"orig": collections.Counter(), "core": collections.Counter()}   # (op, nsfx) -> count
    end_note = ""
    go, gc = frames(fo), frames(fc)
    prev = None
    while True:
        a = next(go, None)
        b = next(gc, None)
        if a is None or b is None:
            if a is not None or b is not None:
                end_note = (f"the original has more frames (core ended at {nframes})" if a is not None
                            else f"the core has more frames (original ended at {nframes})")
            break
        f = a["f"]
        nframes += 1
        verbose = f in args.at
        diffs = []

        def mark(aspect, detail):
            count[aspect] += 1
            if aspect not in first:
                first[aspect] = (f, detail)
            if verbose:
                diffs.append(f"{aspect}: {detail}")

        if a.get("post") and a.get("post") == b.get("post"):
            # both scripts asked for another map: the original posts it (the world is sampled
            # before the load), our session loads it inside the frame: only the calls compare
            ba, bb = a.get("b", []), b.get("b", [])
            if ba != bb:
                mark("builtins", f"last frame: orig {ba[:4]} core {bb[:4]}")
            break
        for k in ("clock", "rng", "n"):
            if a[k] != b[k]:
                mark(k, f"orig {a[k]} core {b[k]}")
        if prev is not None and "rcam" in a:
            # the recorded camera at the start of frame f is the camera after frame f - 1
            if prev[0]["cam"] != a["rcam"]:
                mark("cam-vs-recording-orig", f"frame {f - 1}: emulated {prev[0]['cam']} recorded {a['rcam']}")
            if prev[1]["cam"] != a["rcam"]:
                mark("cam-vs-recording-core", f"frame {f - 1}: core {prev[1]['cam']} recorded {a['rcam']}")
        prev = (a, b)
        if a["cam"] != b["cam"]:
            mark("cam", f"orig {a['cam']} core {b['cam']}")
        bad_layers = [i for i, (x, y) in enumerate(zip(a["crc"], b["crc"])) if x != y]
        if bad_layers:
            mark("crc", f"layers {bad_layers}")
        sa, sb = sprite_map(a, fields), sprite_map(b, fields)
        missing = [k for k in sa if k not in sb]
        extra = [k for k in sb if k not in sa]
        if missing or extra:
            mark("sprites", f"missing in core {[describe(fields, sa[k]) for k in missing[:3]]} (+{max(0, len(missing) - 3)}), "
                            f"extra in core {[describe(fields, sb[k]) for k in extra[:3]]} (+{max(0, len(extra) - 3)})")
        frame_fields = set()
        field_diffs = []
        for k in sa:
            if k not in sb:
                continue
            x, y = sa[k], sb[k]
            bad = [n for n in cmp_fields if x[idx[n]] != y[idx[n]]]
            if bad:
                frame_fields.update(bad)
                field_diffs.append((k, bad, x, y))
                for n in bad:
                    if n not in field_first:
                        field_first[n] = (f, k, x[idx[n]], y[idx[n]], describe(fields, x))
        for n in frame_fields:
            field_frames[n] += 1
        if field_diffs:
            k, bad, x, y = field_diffs[0]
            mark("fields", f"{len(field_diffs)} sprites; first uid {k}: " +
                 ", ".join(f"{n} {x[idx[n]]}/{y[idx[n]]}" for n in bad) + f"  [orig {describe(fields, x)}]")
            if verbose:
                for k, bad, x, y in field_diffs:
                    diffs.append(f"  uid {k}: " + ", ".join(f"{n} {x[idx[n]]}/{y[idx[n]]}" for n in bad))
        ba, bb = a.get("b", []), b.get("b", [])
        if ba != bb:
            i = next((i for i, (x, y) in enumerate(zip(ba, bb)) if x != y), min(len(ba), len(bb)))
            mark("builtins", f"call #{i}: orig {ba[i] if i < len(ba) else None} core {bb[i] if i < len(bb) else None} "
                             f"({len(ba)} vs {len(bb)} calls)")
        ra = [x[1:] for x in a.get("a", []) if x[0] == "r"]
        rb = [x[1:] for x in b.get("a", []) if x[0] == "r"]
        oa = [x for x in a.get("a", []) if x[0] != "r"]
        ob = [x for x in b.get("a", []) if x[0] != "r"]
        for side, ops in (("orig", oa), ("core", ob)):
            for x in ops:
                plays[side][(x[0], x[2])] += 1
        if ra != rb:
            if sorted(ra) == sorted(rb):
                mark("sound-req-order", f"same requests, other order: orig {ra[:6]} core {rb[:6]}")
            else:
                mark("sound-req", f"orig {ra[:8]} core {rb[:8]} ({len(ra)} vs {len(rb)})")
        if oa != ob:
            i = next((i for i, (x, y) in enumerate(zip(oa, ob)) if x != y), min(len(oa), len(ob)))
            mark("sound-ops", f"op #{i}: orig {oa[i] if i < len(oa) else None} core {ob[i] if i < len(ob) else None}; "
                              f"orig {oa[:6]} core {ob[:6]}")
        if a.get("post") != b.get("post"):
            mark("post", f"orig {a.get('post')} core {b.get('post')}")
        if verbose:
            examples.append((f, diffs))

    out = {"orig": args.orig, "core": args.core, "frames": nframes, "end": end_note,
           "first": {k: {"frame": v[0], "detail": v[1]} for k, v in sorted(first.items(), key=lambda kv: kv[1][0])},
           "frames_with_difference": dict(count),
           "fields": {n: {"frames": field_frames[n], "first_frame": field_first[n][0], "uid": str(field_first[n][1]),
                          "orig": field_first[n][2], "core": field_first[n][3], "sprite": field_first[n][4]}
                      for n in sorted(field_frames, key=lambda n: field_first[n][0])}}
    sound = {}
    for key in sorted(set(plays["orig"]) | set(plays["core"]), key=lambda k: (k[0], k[1])):
        x, y = plays["orig"][key], plays["core"][key]
        if x != y:
            sound[f"{key[0]}:{key[1]}"] = [x, y]
    out["sound_ops_per_nsfx"] = sound
    out["sound_ops_total"] = {side: {op: sum(v for k, v in c.items() if k[0] == op) for op in "pus"}
                              for side, c in plays.items()}
    print(f"compared {nframes} frames ({ho.get('demo')}){'; ' + end_note if end_note else ''}")
    if not first:
        print("no difference")
    for k, v in out["first"].items():
        print(f"first {k:9} difference at frame {v['frame']:5} ({count[k]} frames): {v['detail'][:600]}")
    if out["fields"]:
        print("sprite fields (frames with a difference, first occurrence):")
        for n, v in out["fields"].items():
            print(f"  {n:9} {v['frames']:5} frames, first at {v['first_frame']} uid {v['uid']}: orig {v['orig']} core {v['core']}")
    print(f"sound channel operations (p play, u pan/volume, s stop): orig {out['sound_ops_total']['orig']} "
          f"core {out['sound_ops_total']['core']}")
    if sound:
        worst = sorted(sound.items(), key=lambda kv: -abs(kv[1][0] - kv[1][1]))[:12]
        print("  differing op:nsfx counts (orig, core): " + ", ".join(f"{k} {v[0]}/{v[1]}" for k, v in worst))
    for f, diffs in examples:
        print(f"--- frame {f}")
        for d in diffs:
            print("  " + d[:2000])
    if args.json:
        with open(args.json, "w") as j:
            json.dump(out, j, indent=1)
    return 0 if not first else 1


if __name__ == "__main__":
    sys.exit(main())
