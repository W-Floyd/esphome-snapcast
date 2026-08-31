#!/usr/bin/env python3
"""Grade the arms of injector-ab.py: which injector owns the commanded differential rate noise.

Reads /tmp/injector-ab.json (arm windows) and test.csv, and prints per arm:

  sd(trim-int) per board   the COMMANDED rate noise. P-term + align kick, the quantity being split.
  sd(trim_diff)            its differential, which is what the wire can see.
  sd(wire slope)           the ACHIEVED differential rate (R10.5's estimator, ~400x better than fs).
  SF(tau) 1/2/5/10 s       the wire's position structure -- the grade, per R5.3's split. The
                           plateau is NOT claimed from 10-minute arms.
  ac(10 s) of the 1 s wire series   the stairstep test: align kicks fire every 10.0 s, so
                           align_apply 0 should flatten any 10 s structure.

Every number carries n and the arm's row rate (R10.1). Blocks failing the rival gate are dropped
before anything is computed, matching wire-window.py's population.
"""
import csv, json, statistics as st, sys, collections, bisect

RIVAL = 0.5


def rows_in(path, t0, t1):
    out = []
    with open(path) as fh:
        it = (r for r in fh if not r.startswith("#"))
        for r in csv.DictReader(it):
            try:
                t = float(r["unix_s"])
            except (ValueError, KeyError, TypeError):
                continue
            if not (t0 <= t <= t1):
                continue
            try:
                if r.get("rival") and float(r["rival"]) > RIVAL:
                    continue
            except ValueError:
                pass
            try:
                o = float(r["offset_ns"]) / 1000.0
            except (ValueError, KeyError, TypeError):
                continue
            if o != o:
                continue
            def n(k):
                v = r.get(k)
                try:
                    return float(v) if v not in (None, "") else None
                except ValueError:
                    return None
            out.append((t, o, n("trim_a_ppm"), n("trim_b_ppm"), n("int_a_ppm"), n("int_b_ppm")))
    return out


def slope(ts, xs):
    n = len(ts)
    if n < 3:
        return None
    mt, mx = sum(ts) / n, sum(xs) / n
    den = sum((t - mt) ** 2 for t in ts)
    return sum((t - mt) * (x - mx) for t, x in zip(ts, xs)) / den if den > 0 else None


def sf(ts, xs, tau, tol=0.1):
    d = []
    for i, t in enumerate(ts):
        j = bisect.bisect_left(ts, t + tau)
        for k in (j - 1, j):
            if 0 <= k < len(ts) and k != i and abs((ts[k] - t) - tau) <= tol * tau:
                d.append(xs[k] - xs[i]); break
    return st.stdev(d) if len(d) > 8 else None


def main():
    arms = json.load(open(sys.argv[1] if len(sys.argv) > 1 else "/tmp/injector-ab.json"))
    print(f"{'arm':<10} {'n':>6} {'r/s':>5} | {'sdP_A':>6} {'sdP_B':>6} {'sd(trimdiff)':>12} "
          f"{'sd(slope)':>9} | {'SF1':>5} {'SF2':>5} {'SF5':>5} {'SF10':>5} | {'ac10s':>6}")
    for a in arms:
        d = rows_in("test.csv", a["t0"], a["t1"])
        if len(d) < 200:
            print(f"{a['arm']:<10} only {len(d)} rows"); continue
        span = d[-1][0] - d[0][0]
        ts = [x[0] for x in d]; xs = [x[1] for x in d]
        # commanded rate noise per board = trim - int (P-term + kick)
        pa = [x[2] - x[4] for x in d if x[2] is not None and x[4] is not None]
        pb = [x[3] - x[5] for x in d if x[3] is not None and x[5] is not None]
        td = [x[3] - x[2] for x in d if x[2] is not None and x[3] is not None]
        # achieved differential rate from the wire slope, 2.5 s bins
        b = collections.defaultdict(list); t0 = ts[0]
        for t, o in zip(ts, xs): b[int((t - t0) // 2.5)].append((t, o))
        sl = [s for s in (slope([p[0] for p in v], [p[1] for p in v])
                          for v in b.values() if len(v) >= 8) if s is not None]
        # 1 s means -> autocorrelation at 10 s (the stairstep test)
        b1 = collections.defaultdict(list)
        for t, o in zip(ts, xs): b1[int(t - t0)].append(o)
        ser = [st.mean(v) for k, v in sorted(b1.items()) if len(v) >= 3]
        ac10 = float("nan")
        if len(ser) > 60:
            m = st.mean(ser); v = [x - m for x in ser]; den = sum(x * x for x in v)
            if den > 0:
                ac10 = sum(v[i] * v[i + 10] for i in range(len(v) - 10)) / den
        f = lambda x: f"{x:.3f}" if x is not None else "  -- "
        print(f"{a['arm']:<10} {len(d):>6} {len(d)/span:>5.1f} | "
              f"{st.stdev(pa) if len(pa)>8 else float('nan'):>6.3f} "
              f"{st.stdev(pb) if len(pb)>8 else float('nan'):>6.3f} "
              f"{st.stdev(td) if len(td)>8 else float('nan'):>12.3f} "
              f"{st.stdev(sl) if len(sl)>8 else float('nan'):>9.3f} | "
              f"{f(sf(ts,xs,1)):>5} {f(sf(ts,xs,2)):>5} {f(sf(ts,xs,5)):>5} {f(sf(ts,xs,10)):>5} | "
              f"{ac10:>+6.3f}")
    print("\n  sdP_* = sd(trim - int) = commanded rate noise per board (P-term + align kick), ppm")
    print("  ac10s = autocorrelation of the 1 s wire series at 10 s lag -- the stairstep test")
    print("  READ: arm1 (align off) cuts sdP hard => kick dominant; arm2 (tau 480) cuts it => P-term")


if __name__ == "__main__":
    main()
