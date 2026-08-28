#!/usr/bin/env python3
"""Score the model against what the bench actually recorded.

Usage:
    python3 check_real.py [--minutes 60] [--bucket 30] [--a ../../a.log] [--b ../../b.log]
                          [--csv ../../test.csv]

Three comparisons, in order of how much they constrain the model:

  1. WIRE vs ON-DEVICE, the open question. Both series are re-bucketed on ABSOLUTE wall time
     (bucketing each from its own first sample leaves the grids a third of a bucket apart) and
     correlated. The bench reads r = -0.92..-0.98 with equal amplitudes; the model, carrying
     only documented error terms, reads r > 0. Whichever way this comes out on fresh data it
     is the discriminating measurement.
  2. THE FLOOR. Per-sample and per-bucket spread of the on-device differential phase, against
     the model's prediction that it is set by played-frame reporting granularity.
  3. THE PLANT. Depth, trim and their differentials, which the model needs as inputs and
     which also bound how big the lever-arm terms can be.
"""

from __future__ import annotations

import argparse
import datetime as dt

import numpy as np

import ingest
import params as P
import stats


def pair_on_s_ts(ra: ingest.Raw, rb: ingest.Raw):
    """Pair the two boards on the SERVER's chunk timestamp -- the same number on every device
    for the same audio, so there is no sampling-instant error to bound."""
    ia = {int(v): i for i, v in enumerate(ra.s_ts)}
    keys, idx_a, idx_b = [], [], []
    for j, v in enumerate(rb.s_ts):
        i = ia.get(int(v))
        if i is not None:
            keys.append(v)
            idx_a.append(i)
            idx_b.append(j)
    return np.asarray(keys), np.asarray(idx_a, dtype=int), np.asarray(idx_b, dtype=int)


def load_window(path_a, path_b, minutes, nbytes):
    ra = ingest.read_raw(path_a, nbytes)
    rb = ingest.read_raw(path_b, nbytes)
    if len(ra) == 0 or len(rb) == 0:
        raise SystemExit(f"no RAW lines in the tail of {path_a} / {path_b}")
    wa, wb = ingest.unwrap_wall(ra.wall_s), ingest.unwrap_wall(rb.wall_s)
    t_end = min(wa.max(), wb.max())
    t_start = t_end - minutes * 60
    return ra, rb, wa, wb, t_start, t_end


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", default="../../a.log")
    ap.add_argument("--b", default="../../b.log")
    ap.add_argument("--csv", default="../../test.csv")
    ap.add_argument("--minutes", type=float, default=60.0)
    ap.add_argument("--bucket", type=float, default=30.0)
    ap.add_argument("--bytes", type=int, default=120_000_000)
    args = ap.parse_args()

    ra, rb, wa, wb, t0, t1 = load_window(args.a, args.b, args.minutes, args.bytes)
    print(f"window: {dt.timedelta(seconds=round(t0 % 86400))}..{dt.timedelta(seconds=round(t1 % 86400))} "
          f"({args.minutes:.0f} min)   a rows {len(ra)}  b rows {len(rb)}")

    keys, ia, ib = pair_on_s_ts(ra, rb)
    if keys.size == 0:
        raise SystemExit("the two boards share no chunk timestamps in this window -- "
                         "different streams, or the tails do not overlap in time")
    t_pair = wa[ia]
    sel = (t_pair >= t0) & (t_pair <= t1)
    ia, ib, t_pair = ia[sel], ib[sel], t_pair[sel]
    print(f"paired chunks: {ia.size}")

    # ---- 3. the plant
    print("\nPLANT")
    for name, r, idx in (("a", ra, ia), ("b", rb, ib)):
        d = r.depth_us[idx]
        print(f"  {name}: depth {np.median(d)/1000:7.1f} ms  (sd {d.std()/1000:.1f} ms)")
    dd = rb.depth_us[ib] - ra.depth_us[ia]
    print(f"  depth differential B-A: median {np.median(dd)/1000:+.1f} ms   "
          f"-> lever term at 1 ppm of common trim: {np.median(dd)*1e-6:+.2f} us/ppm")
    for name, path in (("a", args.a), ("b", args.b)):
        tt, pp = ingest.read_trim(path, args.bytes)
        if tt.size:
            tt = ingest.unwrap_wall(tt)
            m = (tt >= t0) & (tt <= t1)
            if m.sum():
                print(f"  {name}: trim {np.median(pp[m]):+7.2f} ppm  "
                      f"p2p {pp[m].max()-pp[m].min():6.1f}  sd {pp[m].std():5.2f}  n={m.sum()}")

    # ---- 2. the floor
    dev = rb.phase_us[ib] - ra.phase_us[ia]
    print("\nON-DEVICE DIFFERENTIAL PHASE (B - A)")
    print("  " + stats.summarise(dev, "per chunk"))
    # A one-frame accounting difference is 22.7 us; the model says the floor is set by the
    # granularity of the played/played_ts pair, so its size in frames is the thing to read.
    print(f"  in frames: {np.median(dev)/P.FRAME_US:+.2f} (median), "
          f"{stats.mad(dev)/P.FRAME_US:.2f} (MAD)")

    ti, dev_b = stats.bucket_medians(t_pair, dev, args.bucket)

    # ---- 1. wire vs on-device
    print(f"\nWIRE vs ON-DEVICE ({args.bucket:.0f} s bucket medians, absolute wall time)")
    try:
        csv = ingest.read_analyser(args.csv)
    except Exception as exc:                      # noqa: BLE001
        print(f"  analyser CSV unreadable ({exc}) -- skipped")
        return
    unix = np.asarray(csv["unix_s"], dtype=float)
    off = np.asarray(csv["offset_ns"], dtype=float) / 1000.0     # us, B - A, + = B later
    ok = np.isfinite(unix) & np.isfinite(off)
    unix, off = unix[ok], off[ok]
    # unix -> seconds since local midnight, matching the log timestamps, then unwrapped the
    # same way so a window spanning midnight still lines up.
    local = np.asarray([(dt.datetime.fromtimestamp(u) -
                         dt.datetime.fromtimestamp(u).replace(hour=0, minute=0, second=0,
                                                              microsecond=0)).total_seconds()
                        for u in unix])
    local = ingest.unwrap_wall(local)
    # put both series on the same absolute day-offset as the logs
    while local.max() < t0 - 43200:
        local += 86400.0
    m = (local >= t0) & (local <= t1)
    print(f"  analyser rows in window: {m.sum()}  (of {off.size} finite)")
    if m.sum() < 30:
        print("  not enough overlap -- is the analyser running, and does test.csv cover this window?")
        return
    wi, wire_b = stats.bucket_medians(local[m], off[m], args.bucket)
    common = np.intersect1d(wi, ti)
    if common.size < 4:
        print(f"  only {common.size} shared buckets -- widen --minutes or --bucket")
        return
    wv = np.array([wire_b[np.where(wi == c)[0][0]] for c in common])
    dv = np.array([dev_b[np.where(ti == c)[0][0]] for c in common])
    dv = dv - np.median(dv)          # the absolute phase constant is meaningless
    wv0 = wv - np.median(wv)
    print("   bucket      wire     on-device   (both B - A, us, medians centred)")
    for c, a_, b_ in zip(common, wv0, dv):
        print(f"   {int(c)*args.bucket/60:8.1f}m  {a_:+8.2f}   {b_:+8.2f}")
    r = stats.corr(wv0, dv)
    slope, _icept, resid, n = stats.robust_fit(wv0, dv)
    print(f"\n  n={common.size}  r={r:+.3f}  slope={slope:+.3f} (arithmetic demands +1)  "
          f"resid {resid:.2f} us")
    print(f"  wire sd {wv0.std():.2f} us   on-device sd {dv.std():.2f} us   "
          f"amplitude ratio {dv.std()/max(wv0.std(),1e-9):.2f}")
    print(f"  wire  per-sample: {stats.summarise(off[m], 'wire')}")


if __name__ == "__main__":
    main()
