#!/usr/bin/env python3
"""Grade a delay-loop measurement window from the bench logs.

    dl-window.py --a-off N --b-off N [--discard 150] [--min-samples 100]

Anchors on BYTE OFFSETS (never timestamps: the logs span days without a date). Discards the
first --discard seconds after the first DLLOOP line past each offset (boot/engage transient),
then reports per board: loop-error median/MAD/p5/p95, group render delta median/MAD, and the
A-B correlation plus the DIFFERENTIAL error (A-B) median/MAD -- the on-device proxy for the
wire. Ends with an event census.

PLAN-timing-v2 stages 1-2: also parses the DECIDE census line (every non-idle decision, one
fires per chunk; idle acts=none are throttled <= 2 Hz) and the GDIN pairing-input shadow line,
and reconciles the DECIDE frame sums against the Sync: aggregate counters
(corrected -D/+I frames, H hard resyncs). See CLAUDE.md: formats must be parsed with every
trailing field optional -- the 256-byte ceiling truncates lines and a dropped whole line is
silent data loss.

Run `--self-test` to verify the parsers against truncated / absent-field lines before trusting
any count taken from a log.
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

# --- PLAN-timing-v2 Stage 2 DECIDE census ---
# Fixed fields in order. gd (INTERDIGITAL signed or "unknown") and the trailing t= uptime are
# OPTIONAL: a line truncated at the 256-byte ceiling loses exactly those near-the-end fields, and
# dropping the whole line would lose its frames count (CLAUDE.md: never require a trailing field).
DECIDE = re.compile(
    r"\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*?\bDECIDE src=(\w+) cls=(\w+) gate=(\w+) act=(\w+) "
    # gd is matched as a VALUE, not as \S+. Two boards' log tasks occasionally interleave
    # mid-line on the UART (measured 17 lines in 51,606, 0.03%), and \S+ happily captured the
    # ANSI prefix of the intruding line -- int() then raised and the whole run died. A pattern
    # loose enough to swallow corruption turns a 0.03% nuisance into a total loss; matching the
    # shape means such a line simply reads as gd absent, which it is.
    # sk= (chunks the idle throttle suppressed since the last emit) is OPTIONAL: it postdates
    # the first DECIDE builds, and a log from either era must parse. Like gd and t it sits at
    # the end, so a truncated line loses it rather than losing the frames count.
    r"frames=([+-]\d+) pend=([+-]\d+)(?: gd=([+-]\d+|unknown))?(?: sk=(\d+))?(?: t=(\d+))?"
)
# --- PLAN-timing-v2 Stage 1 GDIN pairing-input shadow ---
# raw = un-halved pairwise phase difference (pre-mean, what the analyser can grade);
# gd = the halved control-path delta; gap/drift/extrap = the pairing inputs. Trailing t optional.
GDIN = re.compile(
    r"\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*?\bGDIN raw=([+-]\d+) gd=([+-]\d+) n=(\d+) "
    r"gap=([+-]\d+) drift=([+-]?\d+\.\d+) extrap=([+-]?\d+\.\d+)(?: t=(\d+))?"
)
# --- Sync report aggregate for DECIDE reconciliation ---
# Sync: avg X us, peak Y us, median Z us | corrected -D/+I frames, H hard resyncs over N chunks
SYNC = re.compile(
    r"\[(\d\d):(\d\d):(\d\d)\.(\d\d\d)\].*?\bSync: avg [-+]?\d+ us, peak [-+]?\d+ us, "
    r"median [-+]?\d+ us \| corrected -(\d+)/\+(\d+) frames, (\d+) hard resyncs over (\d+) chunks"
)

# Timespan of a "quiet 30-minute window", in seconds, for the reconciliation pass-condition.
RECONCILE_WINDOW_S = 1800.0


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


def _ts(m):
    return int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3)) + int(m.group(4)) / 1000.0


def decide_rows(data: str):
    """(sec, src, gate, act, frames, pend, gd_or_none) for every DECIDE line, including
    truncated ones (gd and t are optional in the regex - a dropped line loses its frames)."""
    out = []
    for m in DECIDE.finditer(data):
        gd = m.group(11)
        out.append((_ts(m), m.group(5), m.group(7), m.group(8),
                    int(m.group(9)), int(m.group(10)), None if gd in (None, "unknown") else int(gd),
                    int(m.group(12)) if m.group(12) is not None else None))
    return out


def gdin_rows(data: str):
    """(sec, raw, gd, n, gap, drift, extrap) for every GDIN shadow line."""
    out = []
    for m in GDIN.finditer(data):
        out.append((_ts(m), int(m.group(5)), int(m.group(6)), int(m.group(7)), int(m.group(8)),
                    float(m.group(9)), float(m.group(10))))
    return out


def sync_aggregates(data: str):
    """(sec, dropped, inserted, hard_resyncs, chunks) for each Sync line."""
    return [(_ts(m), int(m.group(5)), int(m.group(6)), int(m.group(7)), int(m.group(8)))
            for m in SYNC.finditer(data)]


def reconcile_decide(rows, t0: float, t1: float):
    """Stage 2 census over [t0, t1): exactly-one-act/one-gate accounting and the signed frame
    sums that must reconcile with the Sync aggregate. Idle (act=none) lines carry frames=+0 and ar
    throttled <= 2 Hz, so they are not per-chunk -- only non-idle decisions contribute frames."""
    rows = [r for r in rows if t0 <= r[0] < t1]
    acts = collections.Counter(r[3] for r in rows)
    gates = collections.Counter(r[2] for r in rows)
    malformed = [r for r in rows if not r[3] or r[2] is None]
    # SOFT frame sums: exclude act=resync -- hard resyncs drop/insert WHOLE chunks that the
    # firmware counts in `hard_resyncs` (and resync_drops), NOT in soft_dropped/_inserted_frames.
    # Only splices/steps/steps contribute to the Sync `corrected -D/+I` counters.
    drops = sum(r[4] for r in rows if r[4] > 0 and r[3] != "resync")
    inserts = -sum(r[4] for r in rows if r[4] < 0 and r[3] != "resync")
    # CHUNKS, not lines. A throttled chunk contributes zero frames but is still a chunk, and
    # sk= is what keeps "every chunk accounted for" true while the line rate stays low. A log
    # predating sk reports None, which is "not counted", NOT zero -- so say unknown rather than
    # silently claiming the emitted lines were the whole census.
    sk = [r[7] for r in rows if r[7] is not None]
    chunks = (len(rows) + sum(sk)) if len(sk) == len(rows) and rows else None
    return {"acts": acts, "gates": gates, "drops": drops, "inserts": inserts,
            "resyncs": acts["resync"], "n": len(rows), "malformed": len(malformed),
            "suppressed": sum(sk) if sk else None, "chunks": chunks}


def self_test() -> None:
    """Verify the parsers never drop a line over a truncated or absent trailing field
    (CLAUDE.md: a half-printed line must not become silent data loss)."""
    good = (
        "[01:02:03.456][D][snapclient.client:123]: DECIDE src=tag cls=none gate=none act=trim "
        "frames=+0 pend=+0 gd=+265 t=98765"
    )
    trunc_t = good[: good.rindex("t=")] + "t=12"       # t cut mid-token by the 256-byte ceiling
    no_gd = good[: good.index(" gd=")]                  # gd and t both gone (cut after pend)
    splice = (
        "[01:02:04.000][D][snapclient.client:124]: DECIDE src=ledger cls=none gate=gd act=step "
        "frames=+42 pend=+23 gd=unknown t=98800"
    )
    gdin_good = (
        "[01:02:05.000][V][snapclient.client:88]: GDIN raw=+531 gd=+265 n=2 gap=+12 "
        "drift=+41.50 extrap=+0.50 t=99100"
    )
    gdin_not = gdin_good[: gdin_good.rindex(" t=")]   # trailing t cut

    d = decide_rows(good + "\n" + trunc_t + "\n" + no_gd + "\n" + splice)
    assert len(d) == 4, f"DECIDE: expected 4 rows (truncated variants), got {len(d)}"
    assert {r[1] for r in d} == {"tag", "ledger"}
    assert {r[3] for r in d} == {"trim", "step"}
    assert sorted(r[4] for r in d) == [0, 0, 0, 42]
    splice_row = [r for r in d if r[3] == "step"][0]
    assert splice_row[6] is None, "gd=unknown must parse to None, not a number"

    g = gdin_rows(gdin_good + "\n" + gdin_not)
    assert len(g) == 2, f"GDIN: expected 2 rows, got {len(g)}"
    assert [x[1] for x in g] == [531, 531], "GDIN: raw must survive truncation"
    assert g[0][4] == 12 and abs(g[0][5] - 41.5) < 1e-9

    sync = sync_aggregates(
        "[02:00:00.000][D][snapclient.client:1]: Sync: avg +3 us, peak +9 us, median +2 us | "
        "corrected -0/+0 frames, 0 hard resyncs over 300 chunks"
    )
    assert sync == [(7200.0, 0, 0, 0, 300)]

    rep = reconcile_decide(d, 3722, 3725)
    assert rep["drops"] == 42 and rep["inserts"] == 0 and rep["resyncs"] == 0 and rep["malformed"] == 0
    print("self-test OK: DECIDE (full/truncated-t/no-gd), GDIN (full/truncated-t), SYNC, reconcile "
          "all survive absent or truncated trailing fields.")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--a-off", type=int)
    ap.add_argument("--b-off", type=int)
    ap.add_argument("--discard", type=float, default=150.0, help="seconds after first DLLOOP to drop")
    ap.add_argument("--min-samples", type=int, default=100)
    ap.add_argument("--self-test", action="store_true", help="verify the DECIDE/GDIN/SYNC parsers against truncated lines")
    args = ap.parse_args()

    if args.self_test:
        self_test()
        return
    if not (args.a_off and args.b_off):
        ap.error("--a-off and --b-off are required unless --self-test is given")

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

        # STAGE 2 DECIDE census over the SAME settled window, reconciled to the Sync aggregate.
        decide = decide_rows(data)
        # t0..t1 is a quiet 30-min window if the settled span reaches that far.
        t_end = ev[-1][0] if ev else t0
        rep = reconcile_decide(decide, t0, t_end) if ev else None
        if rep and rep["n"] > 0:
            supp = ("" if rep["suppressed"] is None
                    else f" +{rep['suppressed']} throttled = {rep['chunks']} chunks")
            print(f"        DECIDE n={rep['n']} lines{supp} acts={dict(rep['acts'])}")
            print(f"               gates={dict(rep['gates'])} malformed={rep['malformed']}")
            # Cross-check frame sums against the Sync aggregates: each Sync line carries the
            # incremental -D/+I/H since the previous Sync (its counters reset after printing), so
            # SUM all in-window lines rather than taking only the last.
            syn = [s for s in sync_aggregates(data) if t0 <= s[0] <= t_end]
            if syn:
                sd = sum(s[1] for s in syn)
                si = sum(s[2] for s in syn)
                sh = sum(s[3] for s in syn)
                ok = (sd == rep["drops"] and si == rep["inserts"] and sh == rep["resyncs"])
                print(f"        reconcile: DECIDE drops={rep['drops']} inserts={rep['inserts']} "
                      f"resyncs={rep['resyncs']} vs Sync -{sd}/+{si}, {sh} -> "
                      f"{'OK' if ok else 'MISMATCH'}")
                if not ok:
                    print("        !! mismatch: either a DECIDE line lost its frames to truncation, or"
                          "        the ladder description in PLAN-timing-v2 is wrong. See CLAUDE.md.")
        # STAGE 1 GDIN: the un-halved pairwise delta beside the halved control-path delta.
        gd = gdin_rows(data)
        gd_in = [r for r in gd if t0 <= r[0] <= t_end] if ev else []
        if gd_in:
            raws = [r[1] for r in gd_in]
            gds = [r[2] for r in gd_in]
            rmed, _, _, _ = summarize(raws)
            gmed, _, _, _ = summarize(gds)
            ratio = (rmed / gmed) if gmed else float("nan")
            print(f"        GDIN n={len(gd_in)} raw(median)={rmed:+.0f} gd(median)={gmed:+.0f} us "
                  f"raw/gd={ratio:.2f} (expect ~2 for a two-speaker group; un-halved vs halved)")


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
