#!/usr/bin/env python3
"""Grade a delay-loop measurement window from the bench logs.

    dl-window.py --a-off N --b-off N [--discard 150] [--min-samples 100]

Anchors on BYTE OFFSETS (never timestamps: the logs span days without a date). Discards the
first --discard seconds after the first DLLOOP line past each offset (boot/engage transient),
then reports per board: loop-error median/MAD/p5/p95, group render delta median/MAD, and the
A-B correlation plus the DIFFERENTIAL error (A-B) median/MAD -- the on-device proxy for the
wire. Ends with an event census. Signed numbers are parsed as integers, not sorted as text.
"""
import argparse
import bisect
import collections
import re
import statistics
import subprocess

ERR = re.compile(r"\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*DLLOOP err=([+-]\d+)")
RND = re.compile(r"\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*render ([+-]?\d+) us")
EVENTS = ("Hard resync", "Fast splice engaged", "OUT OF RANGE", "tags stale", "Muting",
          "deadline source switch", "SERVOPARAM", "SERVOTUNE", "integral restored")


def tail_from(path: str, off: int) -> str:
    return subprocess.run(["tail", "-c", f"+{off}", path], capture_output=True).stdout.decode(errors="replace")


def series(data: str, pat: re.Pattern):
    out = []
    for m in pat.finditer(data):
        h, mn, s, ms = (int(m.group(i)) for i in range(1, 5))
        out.append((h * 3600 + mn * 60 + s + ms / 1000, int(m.group(5))))
    return out


def summarize(vals):
    med = statistics.median(vals)
    mad = statistics.median(abs(x - med) for x in vals)
    s = sorted(vals)
    return med, mad, s[int(len(s) * 0.05)], s[int(len(s) * 0.95)]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--a-off", type=int, required=True)
    ap.add_argument("--b-off", type=int, required=True)
    ap.add_argument("--discard", type=float, default=150.0, help="seconds after first DLLOOP to drop")
    ap.add_argument("--min-samples", type=int, default=100)
    args = ap.parse_args()

    segs = {}
    for name, off in (("a.log", args.a_off), ("b.log", args.b_off)):
        data = tail_from(name, off)
        errs = series(data, ERR)
        if not errs:
            print(f"{name}: no DLLOOP lines past offset {off}")
            continue
        t0 = errs[0][0] + args.discard
        ev = [(t, v) for t, v in errs if t >= t0]
        if len(ev) < args.min_samples:
            print(f"{name}: only {len(ev)} settled samples (need {args.min_samples})")
            continue
        med, mad, p5, p95 = summarize([v for _, v in ev])
        span = ev[-1][0] - ev[0][0]
        print(f"{name}: DLLOOP n={len(ev)} over {span:.0f}s  median={med:+.0f} MAD={mad:.0f} p5={p5:+d} p95={p95:+d} us")
        rnd = [v for t, v in series(data, RND) if t >= t0]
        if rnd:
            rmed, rmad, _, _ = summarize(rnd)
            print(f"        render delta n={len(rnd)} median={rmed:+.0f} MAD={rmad:.0f} us")
        segs[name] = ev
        census = collections.Counter({k: data.count(k) for k in EVENTS})
        print(f"        events: {dict(census)}")

    if len(segs) == 2:
        a, b = segs["a.log"], segs["b.log"]
        bt = [t for t, _ in b]
        pairs = []
        for t, v in a:
            i = bisect.bisect_left(bt, t)
            for j in (i - 1, i):
                if 0 <= j < len(b) and abs(b[j][0] - t) < 0.7:
                    pairs.append((v, b[j][1]))
                    break
        if len(pairs) > 50:
            va = [p[0] for p in pairs]
            vb = [p[1] for p in pairs]
            dmed, dmad, dp5, dp95 = summarize([x - y for x, y in pairs])
            print(f"A-B: r={statistics.correlation(va, vb):+.3f}  differential err median={dmed:+.0f} "
                  f"MAD={dmad:.0f} p5={dp5:+d} p95={dp95:+d} us (n={len(pairs)})")


if __name__ == "__main__":
    main()
