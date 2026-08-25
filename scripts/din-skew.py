#!/usr/bin/env python3
"""Inter-device playout skew measured on the wire, from the I2S DIN lines.

Every firmware metric is measured against that device's own predicted playout, so a
modelling error displaces the audio and the metric together and reads as zero -- the
blind spot scripts/raw-sync.py exists to work around. This closes the loop from
outside the firmware: it watches the DIN line of two boards with a 2-channel USB
scope, so the number it reports is what the DACs actually received.

Probe board A's DIN to CH1, board B's DIN to CH2, grounds commoned. Both boards must
play the SAME stream. Then:

    python3 scripts/din-skew.py --samples 8000000 --interval 5

Start with --probe (one capture, per-channel signal facts, no measurement) whenever
results look wrong; it answers "is this even DIN" before any lag is believed.

HOW THE LAG IS OBTAINED, and why the obvious version fails.

The two channels are binarised and cross-correlated: c[k] = sum a[n]*b[n+k] peaks at
the delay of CH2 behind CH1, so positive means board B is later. Binarising discards
amplitude, so mismatched probe attenuation does not matter.

Correlating the whole capture as one constant lag does NOT work, for two reasons that
were measured, not guessed:

  * The two DIN lines are clocked independently. Each board resamples the stream onto
    its own clock, so the offset is SLIDING while the capture runs -- at 3 ppm it moves
    25 samples across 175 ms -- and the low bits of the PCM genuinely differ between
    boards even on identical audio. That caps the correlation coefficient near 0.3
    (measured 0.28); only the upper bits agree. Decoding DIN to PCM and correlating
    the audio instead does not rescue this and was tried.
  * The audio content is self-similar. Measured: each channel autocorrelates at 0.48
    at a 6.44 us lag, ABOVE the 0.33 cross-channel match, so a whole-record argmax
    lands on an arbitrary member of an alias family spaced ~313 samples apart
    (-93/-408/-716/-1024 were all observed on one capture).

So the capture is cut into blocks, each block correlated separately, and a line fitted
robustly through (block time, block lag). The line handles the slide; outlier rejection
discards blocks that locked onto a content alias; `prefer` keeps successive captures on
the same alias so a 6.4 us step never enters a series whose real motion is nanoseconds.
That yields, per capture:

    offset -- the line at the capture midpoint. Reproducible to +-2 ns across block
              sizes from 0.5 to 5 ms on real hardware.
    ppm    -- the SLOPE, i.e. the relative rate difference between the two boards'
              clocks, measured inside a single 175 ms capture rather than needing two
              captures minutes apart. Verified against synthetic drift at 0.5/3/12 ppm.

Quality is "what fraction of blocks agree with the line", not peak prominence: with a
0.28 ceiling on the coefficient, prominence is meaningless, while unrelated signals
never produce a consistent line at all.

  --mode bitphase
      Pair each CH1 edge with the nearest CH2 edge, sub-sample. Gives skew only modulo
      the DIN bit period, so it cannot see the absolute offset, but it needs no pattern
      match at all. Drift rate is NOT recoverable here unless drift_ppm x interval_s
      < 0.177 -- past that the phase wraps between captures and the script says so
      rather than fitting a bogus slope. Absolute offset and drift are xcorr's job.

Results append to a CSV as they are taken, so a long run survives an interrupt, and the
plot (dependency-free SVG) is rewritten after each measurement.

CAVEATS worth knowing before trusting a number:
  * Check the bit-period line reads `ok`. It compares the measured DIN bit period
    against audio_rate x bits x 2; a x2 mismatch means the driver gave half the
    requested per-channel rate and every skew is mis-scaled.
  * A lag pinned at the +-maxlag search boundary is the boundary, not a measurement;
    the script flags it and asks for a bigger --maxlag-us.
  * The scope drops off the USB bus fairly readily (and never survives a SIGPIPE mid
    transfer, which is why capture output is never piped). If captures start failing,
    check `ioreg -p IOUSB | grep OSC` and replug.

Requires: sigrok-cli, numpy. Verify the analysis path without hardware via --simulate,
which feeds synthetic DIN with a known offset and known drift through the same code.
"""

import argparse
import io
import math
import os
import subprocess
import sys
import time

try:
    import numpy as np
except ImportError:
    sys.exit("needs numpy: pip install numpy (or brew install python-numpy)")

# The hantek-6xxx driver claims this scope (VID:PID 8102:8102, which it knows as the
# Sainsmart DDS120 -- same Rocktech hardware rebadged). sigrok's dedicated Loto
# driver is still 'planned', so this is the working path.
DEFAULT_DRIVER = "hantek-6xxx"

# Correlating the whole capture as ONE constant lag does not work, and the reason is
# worth recording. Measured on two real boards: each channel is self-similar at 6.44 us
# with coefficient 0.48 (the audio content's own repetition), while the cross-channel
# match is only 0.33. The content's comb is TALLER than the signal, so a whole-record
# argmax lands on an arbitrary member of an alias family spaced 6.44 us apart
# (-93/-408/-716/-1024 samples were all observed) and peak prominence looks terrible
# no matter how long the capture.
#
# Blocks fix it: within a short block the lag is single-valued, and while any one block
# may pick an alias, the TRUE lag is the one that recurs across blocks. Consensus
# across blocks, not prominence within one correlation, is the quality signal -- so the
# gate is "how many blocks agree", which is also what distinguishes a real measurement
# from noise (unrelated signals produce scattered lags, never a cluster).
MIN_COEF = 0.12          # a block whose peak is below this contributes nothing
MIN_AGREEMENT = 0.50     # fraction of usable blocks that must land in one cluster
CLUSTER_TOL = 8.0        # samples; residual from the fitted line that still counts as agreeing
# Half-width of the initial search around the expected lag, used to shut out blocks
# that locked onto a content alias. Must sit below half the alias spacing (measured
# 313 samples on real content) while still admitting the real slide within a capture.
ALIAS_WINDOW = 150.0


def sigrok_capture(args):
    """One 2-channel capture. Returns (n, 2) float array, CH1 first."""
    cmd = [
        args.sigrok_cli, "-d", args.driver + (f":conn={args.conn}" if args.conn else ""),
        "-c", f"samplerate={args.samplerate}",
        "--channels", "CH1,CH2",
        "--samples", str(args.samples),
        "-O", "csv",
    ]
    # NEVER pipe sigrok's stdout somewhere that can close early: a SIGPIPE mid bulk
    # transfer wedges this scope off the USB bus until it is physically replugged.
    # subprocess.run reads to completion, which is the point.
    try:
        proc = subprocess.run(cmd, capture_output=True, timeout=args.timeout)
    except subprocess.TimeoutExpired:
        raise RuntimeError(f"sigrok-cli did not finish within {args.timeout}s")
    if proc.returncode != 0 or not proc.stdout:
        err = proc.stderr.decode(errors="replace").strip()
        if "LIBUSB_ERROR_ACCESS" in err or "Failed to open device" in err:
            raise RuntimeError(
                "cannot claim the scope. If it also does not show in "
                "`ioreg -p IOUSB | grep OSC`, it has dropped off the bus -- replug it.\n  " + err)
        raise RuntimeError(err or "sigrok-cli produced no samples")
    return parse_csv(proc.stdout)


def parse_csv(blob):
    """sigrok's CSV, tolerantly: keep only lines that start like a number."""
    rows = (ln.decode() for ln in blob.splitlines() if ln[:1] in b"0123456789-+.")
    arr = np.loadtxt(rows, delimiter=",", ndmin=2)
    if arr.shape[1] < 2:
        raise RuntimeError(
            f"got {arr.shape[1]} channel column(s), need 2 -- is CH2 probed and enabled?")
    return arr[:, :2]


# Hysteresis dead band, as a fraction of the swing either side of mid. A plain
# mid-threshold comparator turns edge ringing into pairs of spurious transitions:
# measured on real probes, 1% of edge gaps were ~4 samples (~85 ns) where the true
# bit period is 34 -- sub-bit "bits" that cannot exist. Those extra edges decorrelate
# the two channels and destroy the GCD period estimate. 0.2 rejects ringing well
# inside a logic swing while still tripping on any real transition.
HYST_FRAC = 0.2


def levels(x):
    """Low/high rails, mid threshold and hysteresis band. Percentiles, not min/max,
    so a single spike or a clipped rail does not move the threshold."""
    lo, hi = np.percentile(x, [2, 98])
    return lo, hi, (lo + hi) / 2.0, (hi - lo) * HYST_FRAC


def binarize(x):
    """DIN is a logic line, so recover the logic state and discard the amplitude.

    This is why probe attenuation need not match between channels, and why clipping
    the 3.3V swing against a small V/div is harmless (even helpful -- it steepens the
    edges). Only the crossing times carry information.

    Schmitt, not a bare comparator, for the reason on HYST_FRAC. Vectorised: mark the
    samples that decide a state, then forward-fill the last decision across the
    undecided ones inside the dead band.
    """
    lo, hi, mid, band = levels(x)
    above, below = x > mid + band, x < mid - band
    idx = np.where(above | below, np.arange(x.size), -1)
    idx = np.maximum.accumulate(idx)
    out = np.zeros(x.size, dtype=bool)
    ok = idx >= 0
    out[ok] = above[idx[ok]]
    return out


def edge_times(x, max_back=24):
    """Sub-sample crossing times, in samples, for every transition.

    The hysteresis state flips once the signal clears the dead band, which is later
    than the mid crossing, so each flip is walked back to the mid crossing and
    linearly interpolated between the two samples straddling it. That resolves edges
    well below the 20.8 ns grid -- worth having, since the grid is coarse next to the
    skew being measured. Note the interpolation assumes both channels have similar
    edge shapes; badly mismatched probe attenuation biases it.
    """
    d = binarize(x)
    flips = np.flatnonzero(np.diff(d.astype(np.int8)) != 0)
    if flips.size == 0:
        return np.empty(0)
    _, _, mid, _ = levels(x)
    out = np.empty(flips.size)
    for k, i in enumerate(flips):
        rising = bool(d[i + 1])
        j, lim = i + 1, max(0, i - max_back)
        while j > lim and (x[j] >= mid) == rising:
            j -= 1
        a, b = x[j], x[j + 1]
        frac = (mid - a) / (b - a) if b != a else 0.5
        out[k] = j + min(max(frac, 0.0), 1.0)
    return out


def _coarse_period(gaps):
    """Fundamental gap spacing, from the spacing BETWEEN gap clusters.

    Edge gaps in I2S data are integer multiples of the bit period, so their histogram
    is a comb: tight clusters at 1T, 2T, 3T... plus a low cluster of ringing artefacts
    that is not a multiple of anything. Earlier attempts tried to pick the fundamental
    as a quantile (wrong -- single-bit gaps are a minority in real audio) and then as a
    GCD seeded from the smallest gap (wrong -- ringing owns the smallest gaps).

    Taking the spacing between ADJACENT clusters sidesteps both. Consecutive real
    clusters are exactly one bit apart, so their median difference is the bit period
    even when the 1T cluster is absent entirely, and an artefact cluster contributes
    one anomalous difference that a median absorbs.
    """
    if gaps.size < 64:
        return float("nan")
    top = float(np.percentile(gaps, 92))
    if not np.isfinite(top) or top <= 2:
        return float("nan")
    bins = np.arange(0, top + 0.5, 0.5)
    h, _ = np.histogram(gaps, bins=bins)
    h = np.convolve(h, np.ones(3) / 3, mode="same")  # tolerate jitter across bin edges
    floor = 0.004 * gaps.size                        # a cluster, not a stray gap
    peaks = [i for i in range(1, h.size - 1)
             if h[i] >= floor and h[i] >= h[i - 1] and h[i] > h[i + 1]]
    if len(peaks) < 2:
        return float("nan")
    centers = (bins[peaks] + 0.25)
    return float(np.median(np.diff(centers)))


def _coverage(gaps, T):
    """Fraction of gaps that a candidate period explains as an integer multiple."""
    if not (np.isfinite(T) and T > 0):
        return 0.0
    r = gaps / T
    n = np.round(r)
    return float(np.mean((n >= 1) & (n <= 40) & (np.abs(r - n) < 0.15)))


def _refine_period(gaps, T):
    """Least squares through the origin: every gap informs T, not just clustered ones."""
    for _ in range(3):
        n = np.round(gaps / T)
        ok = (n >= 1) & (n <= 24) & (np.abs(gaps / T - n) < 0.25)
        if ok.sum() < 32:
            return T
        g, nn = gaps[ok], n[ok]
        T = float(np.sum(g * nn) / np.sum(nn * nn))
    return T


def bit_period_samples(edges):
    """DIN bit period in samples.

    Cluster spacing gives the estimate, least squares gives the value, and coverage
    breaks the octave tie -- all three steps earned by a wrong answer:

      * A quantile of the gap distribution is not the period. Real audio has long runs
        (near silence, sign extension), so one-bit gaps are a minority and the 10th
        percentile read 603 ns against a true 354.
      * A GCD seeded from the smallest gap is worse: a few percent of gaps are ringing
        artefacts a fifth of a bit wide, and the search never reaches the truth.
      * Spacing between adjacent gap CLUSTERS is robust to both, but lands on 2T when
        odd multiples are the weaker clusters -- measured 34.09 samples where the
        quantiles (15/34/48/171 = 1/2/3/10 bits) plainly meant 17. Refinement cannot
        undo it, since at T=2T_true a one-bit gap rounds to n=0 and is discarded.

    So score candidate octaves by how much of the gap distribution each explains, and
    keep the LARGEST period that explains nearly as much as the best. T/2 always covers
    a superset of T's multiples, so "most coverage" alone would descend forever; the
    true period is the largest one that loses nothing.
    """
    if edges.size < 64:
        return float("nan")
    gaps = np.diff(edges)
    gaps = gaps[gaps > 0]
    coarse = _coarse_period(gaps)
    if not np.isfinite(coarse) or coarse <= 0:
        return float("nan")
    scored = []
    for m in (0.25, 0.5, 1.0, 2.0):
        T = _refine_period(gaps, coarse * m)
        if np.isfinite(T) and T > 1.0:
            scored.append((T, _coverage(gaps, T)))
    if not scored:
        return float("nan")
    best = max(c for _, c in scored)
    return max(T for T, c in scored if c >= best - 0.03)


def _block_lag(x, y, maxlag):
    """One block: best lag within +-maxlag, and the correlation coefficient there.

    c[k] = sum_n x[n]*y[n+k] peaks at k = the delay of y behind x, so a POSITIVE
    result means CH2 is later -- board B is behind board A. Dividing by n makes the
    height a correlation coefficient: 1.0 identical, 0.0 unrelated, so it is directly
    comparable between blocks and captures (the old sigma-above-floor score was not).
    """
    n = x.size
    L = 1 << (2 * n - 1).bit_length()
    # A search range wider than the block is meaningless (and would index past the
    # correlation array): a lag of n samples leaves no overlap to correlate.
    maxlag = int(min(maxlag, n - 1, L // 2 - 1))
    c = np.fft.irfft(np.fft.rfft(x, L).conj() * np.fft.rfft(y, L), L) / n
    # Assembled in ASCENDING LAG ORDER, not FFT order, so that the peak's neighbours
    # really are its neighbours -- interpolating across the wrap seam would otherwise
    # produce nonsense for any lag near +-maxlag.
    idx = np.concatenate((np.arange(L - maxlag, L), np.arange(0, maxlag + 1)))
    w = c[idx]
    k = int(np.argmax(np.abs(w)))
    lag = float(k - maxlag)
    # Parabolic refinement. Binarising quantises edges to the sample grid, so this
    # recovers only part of the fraction -- but block lags are averaged across the
    # capture to get a ppm slope, where whole-sample quantisation is the dominant
    # error, and anything sub-sample helps there.
    if 0 < k < w.size - 1:
        y0, y1, y2 = abs(w[k - 1]), abs(w[k]), abs(w[k + 1])
        den = y0 - 2 * y1 + y2
        if abs(den) > 1e-12:
            lag += float(np.clip(0.5 * (y0 - y2) / den, -0.5, 0.5))
    return lag, float(w[k])


def xcorr_blocks(a, b, block, maxlag, prefer=None):
    """Lag of CH2 behind CH1 in samples, by consensus across blocks.

    Returns (lag, mean coefficient of agreeing blocks, agreement fraction, ppm), where
    ppm is the RELATIVE RATE difference measured inside this one capture, from the
    slope of block lag against block time. That is available because the two DIN lines
    are clocked independently: their offset is genuinely sliding while the capture
    runs, so a 175 ms capture measures the drift rate directly instead of needing two
    captures minutes apart.

    `prefer` is the previous accepted lag. The content's self-similarity puts rival
    clusters 6.4 us away (see MIN_COEF), and picking a different alias each time would
    inject 6.4 us steps into a series whose real motion is nanoseconds -- so among
    clusters of comparable support, the one nearest the previous answer wins.
    """
    nb = int(a.size // block)
    if nb < 4:
        return float("nan"), 0.0, 0.0, nb
    xs = binarize(a).astype(np.float32) * 2 - 1
    ys = binarize(b).astype(np.float32) * 2 - 1
    lags, coefs = [], []
    for i in range(nb):
        s = slice(i * block, (i + 1) * block)
        lg, cf = _block_lag(xs[s], ys[s], maxlag)
        lags.append(lg)
        coefs.append(cf)
    lags, coefs = np.array(lags), np.array(coefs)
    times = (np.arange(nb) + 0.5) * block          # block centres, in samples
    keep = np.abs(coefs) >= MIN_COEF
    if keep.sum() < 4:
        return (float("nan"), float(np.abs(coefs).max() if coefs.size else 0.0),
                0.0, float("nan"))
    lv, cv, tv = lags[keep], coefs[keep], times[keep]

    # Fit a LINE through (block time, block lag), not a constant. The two DIN lines are
    # clocked independently, so the offset genuinely slides during the capture -- at
    # 3 ppm it moves 25 samples across 175 ms, which a constant-lag consensus rejects
    # as disagreement even though every block is correct. The slope is the rate
    # difference in ppm; the value at the capture midpoint is the offset.
    #
    # Robust, because blocks that locked onto a content alias sit ~300 samples off and
    # must not drag the fit: seed from the previous answer (or the median), keep only
    # what lands near the line, refit.
    centre = prefer if prefer is not None else float(np.median(lv))
    inl = np.abs(lv - centre) <= ALIAS_WINDOW
    if inl.sum() < 4:
        inl = np.abs(lv - float(np.median(lv))) <= ALIAS_WINDOW
    if inl.sum() < 4:
        return float("nan"), float(np.mean(np.abs(cv))), 0.0, float("nan")
    A = [0.0, float(np.median(lv[inl]))]
    for _ in range(4):
        if inl.sum() >= 6 and np.ptp(tv[inl]) > 0:
            A = list(np.polyfit(tv[inl], lv[inl], 1))
        else:
            A = [0.0, float(np.median(lv[inl]))]
        resid = lv - np.polyval(A, tv)
        nxt = np.abs(resid) <= CLUSTER_TOL
        if nxt.sum() < 4:
            break
        if np.array_equal(nxt, inl):
            inl = nxt
            break
        inl = nxt
    lag = float(np.polyval(A, float(np.mean(tv))))
    ppm = float(A[0] * 1e6) if inl.sum() >= 6 else float("nan")
    return lag, float(np.mean(np.abs(cv[inl]))), float(inl.sum() / lv.size), ppm


def xcorr_lag(a, b):
    """Whole-record correlation. Kept for --probe's peak listing only; see MIN_COEF
    for why it is not used to measure."""
    x = binarize(a).astype(np.float32) * 2 - 1  # +-1: mean-free, amplitude-free
    y = binarize(b).astype(np.float32) * 2 - 1
    n = x.size
    L = 1 << (2 * n - 1).bit_length()  # zero-pad past 2n so nothing wraps around
    C = np.fft.rfft(x, L).conj() * np.fft.rfft(y, L)
    c = np.fft.irfft(C, L)

    k = int(np.argmax(np.abs(c)))
    peak = abs(c[k])
    z = (peak - np.abs(c).mean()) / (np.abs(c).std() + 1e-12)

    # Rival peak outside the guard band: how uniquely determined this lag is.
    masked = np.abs(c).copy()
    lo, hi = max(0, k - PEAK_GUARD), min(L, k + PEAK_GUARD + 1)
    masked[lo:hi] = 0
    if k < PEAK_GUARD:            # the guard wraps; blank the far end too
        masked[L - (PEAK_GUARD - k):] = 0
    ambig = float(masked.max() / (peak + 1e-12))

    # Parabolic interpolation over the peak's neighbours. Note this does NOT buy
    # sub-sample resolution here: binarizing snaps every edge to the sample grid, so
    # xcorr resolves to one sample (20.8 ns at 48 MS/s) and the interpolation only
    # smooths ties. That floor is irrelevant against ms-scale offsets, and it is a
    # fixed bias so it cancels out of the drift slope. --mode bitphase is the one
    # that genuinely resolves below a sample.
    y0, y1, y2 = np.abs(c[(k - 1) % L]), peak, np.abs(c[(k + 1) % L])
    denom = y0 - 2 * y1 + y2
    frac = 0.5 * (y0 - y2) / denom if abs(denom) > 1e-12 else 0.0
    lag = k + np.clip(frac, -0.5, 0.5)
    if lag > L / 2:               # unsigned FFT index -> signed lag
        lag -= L
    return float(lag), float(z), ambig


def bitphase_lag(a, b):
    """Skew modulo the DIN bit period, from nearest-edge pairing.

    Each CH1 edge is matched to the closest CH2 edge and the median difference taken.
    Median, not mean: a missing or extra edge at the capture boundary mispairs by a
    whole bit period and would drag a mean badly.
    """
    ea = edge_times(a)
    eb = edge_times(b)
    if ea.size < 32 or eb.size < 32:
        return float("nan"), 0.0, 1.0, float("nan")
    T = bit_period_samples(ea)
    j = np.clip(np.searchsorted(eb, ea), 1, eb.size - 1)
    d = np.where(np.abs(eb[j] - ea) < np.abs(eb[j - 1] - ea), eb[j] - ea, eb[j - 1] - ea)
    if np.isfinite(T) and T > 0:  # fold into [-T/2, T/2)
        d = (d + T / 2) % T - T / 2
    med = float(np.median(d))
    spread = float(np.median(np.abs(d - med)))  # MAD: edge-pairing consistency
    # Expressed on the same 0..1 scale as the correlation coefficient so one threshold
    # reads the same in both modes: 1.0 = every edge pair agrees, 0.0 = scattered.
    qual = max(0.0, 1.0 - spread / (0.25 * T)) if np.isfinite(T) and T > 0 else 0.0
    return med, float(qual), 1.0, T


def xcorr_peaks(a, b, n):
    """The n tallest, mutually separated correlation peaks -- shows whether the best
    lag actually stands out or merely won a coin toss among frame-period aliases."""
    x = binarize(a).astype(np.float32) * 2 - 1
    y = binarize(b).astype(np.float32) * 2 - 1
    L = 1 << (2 * x.size - 1).bit_length()
    c = np.abs(np.fft.irfft(np.fft.rfft(x, L).conj() * np.fft.rfft(y, L), L))
    best = c.max()
    out = []
    for _ in range(n):
        k = int(np.argmax(c))
        out.append(((k - L) if k > L / 2 else k, float(c[k] / (best + 1e-12))))
        c[max(0, k - PEAK_GUARD):k + PEAK_GUARD + 1] = 0
    return out


def probe(args, capture):
    """Dump what is actually on the two probes, before trusting any skew number.

    Low correlation quality has several very different causes -- a dead probe, boards
    further apart than the capture window, or genuinely different bits -- and they are
    indistinguishable from the skew number alone. These are the per-channel facts that
    separate them.
    """
    arr = capture()
    print(f"captured {arr.shape[0]} samples x {arr.shape[1]} ch "
          f"@ {args.sample_period_ns:.3f} ns/sample "
          f"({arr.shape[0] * args.sample_period_ns / 1e6:.2f} ms)\n")
    ns = args.sample_period_ns
    for i, name in enumerate(("CH1", "CH2")):
        x = arr[:, i]
        lo, hi = np.percentile(x, [2, 98])
        d = binarize(x)
        e = edge_times(x)
        T = bit_period_samples(e)
        print(f"{name}: range {x.min():+.3f}..{x.max():+.3f} V  "
              f"p2/p98 {lo:+.3f}/{hi:+.3f}  swing {hi-lo:.3f} V")
        print(f"     high {100*d.mean():.1f}% of samples   {e.size} edges "
              f"({e.size/(arr.shape[0]*ns/1e6):.0f} per ms)")
        # Attenuation itself is harmless -- binarize() discards amplitude, so a x10
        # probe reading 0.33 V for a 3.3 V line is perfectly usable. What matters is
        # how many of the 8-bit scope's ~20 mV codes the swing spans. Do not "fix" a
        # x10 reading by switching to x1 without checking the swing survives it.
        codes = (hi - lo) / 0.02
        if codes < 5:
            print(f"     WARNING: {hi-lo:.3f} V swing is ~{codes:.0f} codes -- this is "
                  f"the noise floor, not a signal. Probe not contacting DIN?")
        elif codes < 30:
            print(f"     note: {hi-lo:.3f} V swing is ~{codes:.0f} codes (x10 probe on a "
                  f"3.3 V line looks like this). Usable -- amplitude is discarded -- but "
                  f"edge interpolation is coarse; x1 is better IF the swing holds up")
        if e.size > 32:
            g = np.diff(e)
            qs = np.percentile(g, [1, 5, 25, 50, 90])
            print(f"     edge gaps (samples) p1/p5/p25/p50/p90: "
                  + "/".join(f"{q:.1f}" for q in qs))
            glitch = float(np.mean(g < T * 0.6)) if np.isfinite(T) and T > 0 else 0.0
            print(f"     bit period {T:.2f} samples = {T*ns:.0f} ns"
                  f"  -> implied bclk {1e9/(T*ns)/1e6:.3f} MHz")
            print(f"     sub-bit edge gaps: {100*glitch:.2f}% (of {g.size}) "
                  + ("(clean)" if glitch < 0.005 else
                     "(RINGING -- shorten the probe ground lead)"))
        print()
    if arr.shape[1] >= 2:
        expected = 1e9 / (args.audio_rate * args.bits * 2)
        print(f"expected bit period {expected:.0f} ns "
              f"(bclk {args.audio_rate*args.bits*2/1e6:.3f} MHz "
              f"= {args.audio_rate} Hz x {args.bits} bits x 2)\n")
        lag, z, ambig = xcorr_lag(arr[:, 0], arr[:, 1])
        print(f"xcorr: lag {lag*ns/1000:+.3f} us  z {z:.1f}  ambig {ambig:.3f}")
        print("  top correlation peaks (lag us / height relative to best):")
        for plag, rel in xcorr_peaks(arr[:, 0], arr[:, 1], 5):
            print(f"    {plag*ns/1000:+10.3f}  {rel:.3f}")
        print("  first 120 samples, binarized (CH1 over CH2):")
        b1 = "".join("#" if v else "." for v in binarize(arr[:, 0])[:120])
        b2 = "".join("#" if v else "." for v in binarize(arr[:, 1])[:120])
        print("   " + b1)
        print("   " + b2)


def measure(args, capture, prefer=None):
    """One capture -> one row. Returns dict, or a row with dt_ns NaN plus a reason."""
    arr = capture()
    ch1, ch2 = arr[:, 0], arr[:, 1]
    ns_per_sample = args.sample_period_ns

    T = bit_period_samples(edge_times(ch1))
    ppm = float("nan")
    at_limit = False
    if args.mode == "xcorr":
        block = max(2048, int(args.block_us * 1000 / ns_per_sample))
        maxlag = max(64, int(args.maxlag_us * 1000 / ns_per_sample))
        lag, coef, agree, ppm = xcorr_blocks(ch1, ch2, block, maxlag, prefer)
        at_limit = np.isfinite(lag) and abs(lag) > 0.95 * maxlag
    else:
        lag, coef, agree, T = bitphase_lag(ch1, ch2)

    row = {
        "dt_ns": lag * ns_per_sample,
        "coef": coef,
        "agree": agree,
        "ppm": ppm,
        "lag_samples": lag,
        "bit_ns": T * ns_per_sample if np.isfinite(T) else float("nan"),
        "reason": "",
    }
    if not np.isfinite(lag):
        row["reason"] = "no lock"
    elif args.mode == "xcorr" and at_limit:
        # A lag pinned against the search boundary is the boundary, not a measurement.
        row["reason"] = f"lag at +-{args.maxlag_us:g} us search limit (raise --maxlag-us)"
    elif coef < (MIN_COEF if args.mode == "xcorr" else 0.30):
        row["reason"] = f"weak (coef={coef:.2f})"
    elif args.mode == "xcorr" and agree < MIN_AGREEMENT:
        row["reason"] = f"blocks disagree ({agree:.0%})"
    if row["reason"]:
        row["dt_ns"] = float("nan")
    return row


def check_timebase(bit_ns, args):
    """Warn if the measured bit period contradicts the assumed sample rate.

    In 2-channel mode this driver can deliver half the requested rate per channel,
    which would scale every skew by 2x while looking perfectly plausible. The DIN bit
    period is known a priori (audio rate x bits x 2 channels), so comparing against it
    catches exactly that. Absolute skew is only as good as this ratio.
    """
    expected_ns = 1e9 / (args.audio_rate * args.bits * 2)
    if not np.isfinite(bit_ns) or bit_ns <= 0:
        return f"bit period unmeasurable (expected {expected_ns:.0f} ns) -- is DIN actually toggling?"
    ratio = bit_ns / expected_ns
    msg = f"DIN bit period {bit_ns:.0f} ns vs expected {expected_ns:.0f} ns (x{ratio:.2f})"
    if abs(ratio - 1) > 0.15:
        extra = " -- 2x off: per-channel rate is likely half the requested rate; pass --sample-period-ns" \
            if 1.6 < ratio < 2.4 else " -- time base is wrong; skews are mis-scaled by this factor"
        return "WARNING: " + msg + extra
    return "ok: " + msg


def load_existing(path):
    if not os.path.exists(path):
        return [], []
    ts, dts = [], []
    with open(path) as f:
        for line in f:
            if line.startswith("#") or line.startswith("elapsed"):
                continue
            parts = line.strip().split(",")
            if len(parts) >= 3:
                try:
                    ts.append(float(parts[0]))
                    dts.append(float(parts[2]))
                except ValueError:
                    pass
    return ts, dts


def unwrap_bitphase(dts, bit_ns):
    """Undo the modulo so a drifting phase reads as a continuous ramp."""
    out, offset, prev = [], 0.0, None
    for v in dts:
        if not math.isfinite(v):
            out.append(float("nan"))
            continue
        if prev is not None and math.isfinite(bit_ns) and bit_ns > 0:
            while v + offset - prev > bit_ns / 2:
                offset -= bit_ns
            while v + offset - prev < -bit_ns / 2:
                offset += bit_ns
        out.append(v + offset)
        prev = out[-1]
    return out


def bitphase_alias_ok(dts, bit_ns, interval):
    """Whether the bit-phase series can be unwrapped, or has aliased.

    Unwrapping assumes the skew moved less than half a bit period since the previous
    capture. Once it moves more, every step is indistinguishable from a step one bit
    the other way and the unwrapped ramp is meaningless -- it comes out near zero,
    which reads as "no drift" rather than as a failure. So check it explicitly.
    """
    if not (math.isfinite(bit_ns) and bit_ns > 0) or interval <= 0:
        return True, ""
    steps = [abs(b - a) for a, b in zip(dts, dts[1:])
             if math.isfinite(a) and math.isfinite(b)]
    if len(steps) < 3:
        return True, ""
    limit = bit_ns / 2
    safe = limit / 2          # keep a x2 margin against the wrap, not ride it
    if float(np.median(steps)) > safe:
        max_ppm = (safe / 1e3) / interval
        return False, (
            f"WARNING: bit-phase steps (median {np.median(steps):.0f} ns) are near the "
            f"+-{limit:.0f} ns wrap limit, so unwrapping has aliased and the slope below is "
            f"NOT the drift rate. At --interval {interval:g}s this mode only tracks drift "
            f"under {max_ppm:.3f} ppm; use --mode xcorr for drift.")
    return True, ""


def fit_slope(ts, ys):
    """Least-squares drift rate in us/s, which is the same number as ppm."""
    pts = [(t, y) for t, y in zip(ts, ys) if math.isfinite(y)]
    if len(pts) < 3:
        return None
    t = np.array([p[0] for p in pts])
    y = np.array([p[1] for p in pts]) / 1000.0  # ns -> us
    if t.max() - t.min() < 1e-6:
        return None
    slope, intercept = np.polyfit(t, y, 1)
    return float(slope), float(intercept)


def write_svg(path, ts, ys, title, ylabel, fit=None):
    """Plot without a plotting dependency -- the rest of scripts/ is stdlib too."""
    W, H, M = 900, 420, 70
    pts = [(t, y) for t, y in zip(ts, ys) if math.isfinite(y)]
    if not pts:
        open(path, "w").write(
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}">'
            f'<text x="{W/2}" y="{H/2}" text-anchor="middle" font-family="sans-serif">'
            f'no valid measurements yet</text></svg>')
        return
    xs = [p[0] for p in pts]
    vs = [p[1] for p in pts]
    x0, x1 = min(xs), max(xs)
    y0, y1 = min(vs), max(vs)
    if x1 - x0 < 1e-9:
        x1 = x0 + 1
    pad = (y1 - y0) * 0.1 or (abs(y0) * 0.1 or 1)
    y0, y1 = y0 - pad, y1 + pad

    def px(t):
        return M + (t - x0) / (x1 - x0) * (W - M - 40)

    def py(v):
        return H - M - (v - y0) / (y1 - y0) * (H - 2 * M)

    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
           f'font-family="-apple-system,sans-serif" font-size="12">',
           f'<rect width="{W}" height="{H}" fill="white"/>',
           f'<text x="{M}" y="26" font-size="15" font-weight="600">{title}</text>']
    for i in range(6):  # y grid + labels
        v = y0 + (y1 - y0) * i / 5
        yy = py(v)
        out.append(f'<line x1="{M}" y1="{yy:.1f}" x2="{W-40}" y2="{yy:.1f}" '
                   f'stroke="#e5e5e5"/>')
        out.append(f'<text x="{M-8}" y="{yy+4:.1f}" text-anchor="end" fill="#555">'
                   f'{v:.4g}</text>')
    for i in range(6):  # x ticks
        t = x0 + (x1 - x0) * i / 5
        xx = px(t)
        out.append(f'<line x1="{xx:.1f}" y1="{H-M}" x2="{xx:.1f}" y2="{H-M+5}" stroke="#888"/>')
        out.append(f'<text x="{xx:.1f}" y="{H-M+20}" text-anchor="middle" fill="#555">'
                   f'{t:.0f}</text>')
    out.append(f'<line x1="{M}" y1="{H-M}" x2="{W-40}" y2="{H-M}" stroke="#333"/>')
    out.append(f'<line x1="{M}" y1="{M}" x2="{M}" y2="{H-M}" stroke="#333"/>')
    out.append(f'<text x="{W/2}" y="{H-18}" text-anchor="middle" fill="#333">'
               f'elapsed (s)</text>')
    out.append(f'<text x="16" y="{H/2}" text-anchor="middle" fill="#333" '
               f'transform="rotate(-90 16 {H/2})">{ylabel}</text>')
    if fit:
        slope, intercept = fit
        fy0, fy1 = (intercept + slope * x0) * 1000, (intercept + slope * x1) * 1000
        out.append(f'<line x1="{px(x0):.1f}" y1="{py(fy0):.1f}" x2="{px(x1):.1f}" '
                   f'y2="{py(fy1):.1f}" stroke="#d9534f" stroke-width="1.5" '
                   f'stroke-dasharray="6 4"/>')
        out.append(f'<text x="{M+8}" y="{M-10}" fill="#d9534f">'
                   f'fit {slope:+.4f} us/s ({slope:+.4f} ppm)</text>')
    poly = " ".join(f"{px(t):.1f},{py(v):.1f}" for t, v in pts)
    out.append(f'<polyline points="{poly}" fill="none" stroke="#2b6cb0" stroke-width="1.5"/>')
    for t, v in pts:
        out.append(f'<circle cx="{px(t):.1f}" cy="{py(v):.1f}" r="2.5" fill="#2b6cb0"/>')
    out.append("</svg>")
    with open(path, "w") as f:
        f.write("\n".join(out))


def make_simulator(args):
    """Synthetic DIN with a known, drifting offset -- exercises everything but USB.

    Goes through parse_csv so the real capture path's parsing is covered too.
    """
    rng = np.random.default_rng(7)
    bit = args.sample_period_ns and (1e9 / (args.audio_rate * args.bits * 2)) / args.sample_period_ns
    state = {"i": 0}

    def capture():
        i = state["i"]
        state["i"] += 1
        # The two DIN lines are clocked independently, so the offset slides DURING a
        # capture as well as between captures. Modelling only the between-capture part
        # would leave the intra-capture ppm slope untested.
        lag0 = args.sim_lag_ns / args.sample_period_ns + \
            i * args.interval * args.sim_drift_ppm * 1e3 / args.sample_period_ns
        slope = args.sim_drift_ppm * 1e-6
        margin = int(abs(lag0)) + int(abs(slope) * args.samples) + 64
        n = args.samples
        nbits = int(n / bit) + 2 * int(margin / bit) + 8
        bits = rng.integers(0, 2, nbits).astype(np.float32)
        master = np.repeat(bits, max(1, int(round(bit))))
        master = np.convolve(master, np.ones(3) / 3, mode="same")  # analog rise time
        grid = np.arange(len(master), dtype=np.float64)
        idx = np.arange(n, dtype=np.float64) + margin
        drift = lag0 + slope * np.arange(n, dtype=np.float64)
        true_lag = float(lag0 + slope * n / 2)          # mid-capture, what we report
        ch1 = np.interp(idx, grid, master) * 3.3
        ch2 = np.interp(idx - drift, grid, master) * 2.7 + 0.2  # different probe scale
        ch1 += rng.normal(0, 0.03, n)
        ch2 += rng.normal(0, 0.03, n)
        blob = "\n".join([";sim", "CH1,CH2"] +
                         [f"{a:.4f},{b:.4f}" for a, b in zip(ch1, ch2)]).encode()
        state["true_ns"] = true_lag * args.sample_period_ns
        return parse_csv(blob)

    return capture, state


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--mode", choices=["xcorr", "bitphase"], default="xcorr")
    p.add_argument("--interval", type=float, default=5.0, help="seconds between captures")
    p.add_argument("--count", type=int, default=0, help="captures to take (0 = until Ctrl-C)")
    p.add_argument("--samples", type=int, default=500_000,
                   help="samples per capture. At 48 MS/s, 500k is a ~10.4 ms window, so "
                        "xcorr can see offsets up to about +-5 ms; raise it if the "
                        "boards are further apart than that")
    p.add_argument("--samplerate", default="48M")
    p.add_argument("--sample-period-ns", type=float, default=None,
                   help="override the assumed sample period (default 1e9/samplerate)")
    p.add_argument("--audio-rate", type=int, default=44100, help="stream sample rate")
    p.add_argument("--bits", type=int, default=16, help="I2S bits per channel slot")
    p.add_argument("--driver", default=DEFAULT_DRIVER)
    p.add_argument("--conn", default=None, help="sigrok conn= if several scopes")
    p.add_argument("--sigrok-cli", default="sigrok-cli")
    p.add_argument("--timeout", type=float, default=60.0)
    p.add_argument("--out", default="din-skew.csv")
    p.add_argument("--append", action="store_true",
                   help="continue a previous run's CSV instead of starting fresh")
    p.add_argument("--plot", default="din-skew.svg")
    p.add_argument("--block-us", type=float, default=2000.0,
                   help="xcorr block length. Short enough that the lag is constant "
                        "within a block, long enough for a unique peak")
    p.add_argument("--maxlag-us", type=float, default=50.0,
                   help="lag search half-range per block")
    p.add_argument("--probe", action="store_true",
                   help="one capture, dump per-channel signal facts, exit")
    p.add_argument("--simulate", action="store_true",
                   help="no hardware: synthetic DIN with a known offset, to check this script")
    p.add_argument("--sim-lag-ns", type=float, default=20_007.3)
    p.add_argument("--sim-drift-ppm", type=float, default=20.0)
    args = p.parse_args()

    if args.sample_period_ns is None:
        mult = {"K": 1e3, "M": 1e6, "G": 1e9}
        s = args.samplerate.strip()
        rate = float(s[:-1]) * mult[s[-1].upper()] if s[-1].upper() in mult else float(s)
        args.sample_period_ns = 1e9 / rate

    if args.simulate:
        capture, sim = make_simulator(args)
        if args.count == 0:
            args.count = 12
    else:
        capture, sim = (lambda: sigrok_capture(args)), None

    if args.probe:
        try:
            probe(args, capture)
        except RuntimeError as e:
            sys.exit(f"capture failed: {e}")
        return

    # A run is one continuous acquisition; see i2s-skew.py for why appending across
    # invocations misleads. Fresh CSV by default, --append to override.
    ts, dts, ppms = [], [], []
    if args.append:
        ts, dts = load_existing(args.out)
        if ts:
            print(f"appending to {len(ts)} existing rows in {args.out}")
        new_file = not os.path.exists(args.out)
        log = open(args.out, "a", buffering=1)
    else:
        prev, _ = load_existing(args.out) if os.path.exists(args.out) else ([], [])
        if prev:
            print(f"replacing {args.out} ({len(prev)} rows from a previous run)")
        new_file = True
        log = open(args.out, "w", buffering=1)
    if new_file:
        log.write("# elapsed_s,unix_s,dt_ns,coef,agree,ppm_intra,bit_ns,reason\n")
        log.write("elapsed_s,unix_s,dt_ns,coef,agree,ppm_intra,bit_ns,reason\n")

    t_start = time.time() - (ts[-1] if ts else 0.0)
    checked, aliased_warned, n, prefer = False, False, 0, None
    if ts and dts and math.isfinite(dts[-1]):
        prefer = dts[-1] / args.sample_period_ns   # keep the alias across a resume
    print(f"mode={args.mode} samples={args.samples} @ {args.samplerate} "
          f"({args.sample_period_ns:.3f} ns/sample, "
          f"{args.samples * args.sample_period_ns / 1e6:.2f} ms window)")
    print(f"{'elapsed':>9} {'dt':>13} {'coef':>6} {'agree':>6} {'ppm':>8}  note")
    try:
        while args.count == 0 or n < args.count:
            t_wall = time.time()
            elapsed = (n * args.interval) if args.simulate else (t_wall - t_start)
            try:
                row = measure(args, capture, prefer)
            except RuntimeError as e:
                print(f"{elapsed:9.1f}   capture failed: {e}", file=sys.stderr)
                n += 1
                if args.count and n >= args.count:
                    break
                if not args.simulate:
                    time.sleep(args.interval)
                continue

            if math.isfinite(row["ppm"]):
                ppms.append(row["ppm"])
            if not checked and math.isfinite(row["bit_ns"]):
                print("  " + check_timebase(row["bit_ns"], args))
                checked = True

            ts.append(elapsed)
            dts.append(row["dt_ns"])
            if math.isfinite(row["dt_ns"]):
                prefer = row["lag_samples"]
            log.write(f'{elapsed:.3f},{t_wall:.3f},{row["dt_ns"]:.1f},{row["coef"]:.3f},'
                      f'{row["agree"]:.3f},{row["ppm"]:.3f},{row["bit_ns"]:.1f},'
                      f'{row["reason"]}\n')

            shown = (f'{row["dt_ns"]/1000:>10.3f} us' if math.isfinite(row["dt_ns"])
                     else f'{"--":>13}')
            note = row["reason"]
            if args.simulate and math.isfinite(row["dt_ns"]):
                if args.mode == "xcorr":
                    note = f'true {sim["true_ns"]/1000:.3f} us, ' \
                           f'err {(row["dt_ns"]-sim["true_ns"]):+.1f} ns'
                elif sim.get("prev") is not None:
                    step = unwrap_bitphase([sim["prev"], row["dt_ns"]], row["bit_ns"])[1] \
                        - sim["prev"]
                    true_step = sim["true_ns"] - sim["prev_true"]
                    note = f'step {step:+.1f} ns (true {true_step:+.1f})'
                else:
                    note = "first sample (bitphase measures steps)"
                sim["prev"], sim["prev_true"] = row["dt_ns"], sim["true_ns"]
            ppm_s = f'{row["ppm"]:+8.2f}' if math.isfinite(row["ppm"]) else f'{"--":>8}'
            print(f'{elapsed:9.1f} {shown} {row["coef"]:6.3f} {row["agree"]:6.0%} '
                  f'{ppm_s}  {note}')

            plot_y = dts
            ylabel = "CH2 - CH1 (us)   [+ = board B later]"
            fit_ok = True
            if args.mode == "bitphase":
                plot_y = unwrap_bitphase(dts, row["bit_ns"])
                ylabel = "bit-phase skew, unwrapped (us)"
                fit_ok, warn = bitphase_alias_ok(dts, row["bit_ns"], args.interval)
                if warn and not aliased_warned:
                    print("  " + warn)
                    aliased_warned = True
            fit = fit_slope(ts, plot_y) if fit_ok else None
            write_svg(args.plot, ts, [v / 1000.0 for v in plot_y],
                      f"DIN skew ({args.mode})", ylabel, fit)

            n += 1
            if (args.count == 0 or n < args.count) and args.interval > 0 \
                    and not args.simulate:
                time.sleep(args.interval)
    except KeyboardInterrupt:
        print("\ninterrupted")

    good = [v for v in dts if math.isfinite(v)]
    print(f"\n{len(good)}/{len(dts)} usable")
    if ppms:
        print(f"  intra-capture rate difference: mean {np.mean(ppms):+.3f} ppm, "
              f"sd {np.std(ppms):.3f} ppm over {len(ppms)} captures")
    if good:
        print(f"  mean {np.mean(good)/1000:+.3f} us   sd {np.std(good)/1000:.3f} us   "
              f"range {(max(good)-min(good))/1000:.3f} us")
    if args.mode == "bitphase":
        plot_y = unwrap_bitphase(dts, row["bit_ns"])
        fit_ok, _ = bitphase_alias_ok(dts, row["bit_ns"], args.interval)
    else:
        plot_y, fit_ok = dts, True
    fit = fit_slope(ts, plot_y) if fit_ok else None
    if fit:
        print(f"  drift {fit[0]:+.4f} us/s ({fit[0]:+.4f} ppm) over {ts[-1]-ts[0]:.0f} s")
    elif not fit_ok:
        print("  drift not reported: bit-phase series aliased (see warning above)")
    print(f"  data {args.out}   plot {args.plot}")
    log.close()


if __name__ == "__main__":
    main()
