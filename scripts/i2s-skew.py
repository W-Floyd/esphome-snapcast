#!/usr/bin/env python3
"""Inter-device playout skew from decoded I2S, using a logic analyser.

Supersedes scripts/din-skew.py for this job. That script had only two analog probes,
so it could see DIN alone and had to infer everything from the data line: frame
boundaries were unknown, PCM could not be decoded, and it was reduced to correlating
raw bit patterns. Since the two boards are clocked independently, each resamples the
stream onto its own clock and their low PCM bits genuinely differ, capping the
bit-level correlation coefficient near 0.3 and leaving the lag ambiguous against the
audio's own self-similarity comb.

With BCLK and LRC probed on BOTH boards, none of that applies:

  * LRC marks the frame, so I2S decodes properly into PCM samples.
  * Correlating decoded PCM is immune to volume and gain differences (correlation is
    scale invariant) and to low-bit resampling noise, so it locks hard and identifies
    WHICH frame of board A corresponds to which frame of board B -- absolutely, with
    no modulo ambiguity.
  * Once frames are paired, the skew is just the time between the two boards' LRC
    edges for a matched pair. That is a direct edge-time measurement, not a
    correlation peak, so its precision is the analyser's sample period rather than a
    fraction of a bit.

The offset is therefore measured in two parts that add exactly: an integer number of
frames from the PCM match, plus the sub-frame residual from the LRC edges.

Channel assignment is read from a PulseView session file so it cannot drift out of
sync with the probes -- scripts/logic-analyzer.pvs by default, which maps:

    DIN_ONE/BCLK_ONE/LRC_ONE   board A        DIN_TWO/BCLK_TWO/LRC_TWO   board B

Usage (close PulseView first -- it holds the USB interface and sigrok-cli cannot
claim it while it is open):

    python3 scripts/i2s-skew.py --interval 5

Each capture yields the offset at its midpoint AND the relative clock rate in ppm from
the slope of skew against frame time within that one capture. Results append to a CSV;
a dependency-free SVG plot is rewritten after each capture.

Verify the whole analysis path without hardware via --simulate, which rasterises two
synthetic I2S buses with a known offset and known clock difference and checks the
recovered numbers against the truth.
"""

import argparse
import bisect
import concurrent.futures
import math
import queue
import threading
import os
import re
import subprocess
import sys
import time

try:
    import numpy as np
except ImportError:
    sys.exit("needs numpy: pip install numpy (or brew install python-numpy)")

DEFAULT_PVS = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           "logic-analyzer.pvs")
# fx2lafw tops out at 24 MHz across 8 channels. At a 1.41 MHz BCLK that is ~17 samples
# per bit -- ample for edge detection -- and gives 41.7 ns of timing resolution, which
# averaging over thousands of frames per capture takes well below a nanosecond.
DEFAULT_RATE = "24M"
# A capture must hold enough frames for the PCM correlation to be unambiguous. 100 ms
# is ~4400 frames at 44.1 kHz.
DEFAULT_SAMPLES = 2_400_000

MIN_PCM_COEF = 0.5      # decoded-PCM correlation below this is not a frame match
# A peak at least this fraction of the tallest is treated as tied with it, so continuity
# with the previous capture decides between them rather than noise.
RIVAL_MARGIN = 0.92
MIN_FRAMES = 200        # fewer frames than this in a capture is not worth fitting


def parse_pvs(path):
    """Channel name -> index, from a PulseView session file's [Dn] name= entries."""
    if not os.path.exists(path):
        return {}
    out, cur = {}, None
    for line in open(path, errors="replace"):
        line = line.strip()
        m = re.match(r"^\[D(\d+)\]$", line)
        if m:
            cur = int(m.group(1))
            continue
        if cur is not None and line.startswith("name="):
            out[line[5:].strip()] = cur
            cur = None
    return out


def capture_logic(args):
    """One capture. Returns a uint8 array, one byte per sample, bit i = channel Di.

    All eight channels are captured even though six are used: with 8 channels the
    unit size is one byte and bit i is Di unambiguously. Disabling channels would
    repack the bits and make the mapping depend on which were enabled.
    """
    cmd = [args.sigrok_cli, "-d", args.driver + (f":conn={args.conn}" if args.conn else ""),
           "-c", f"samplerate={args.samplerate}",
           "--samples", str(args.samples), "-O", "binary"]
    try:
        proc = subprocess.run(cmd, capture_output=True, timeout=args.timeout)
    except subprocess.TimeoutExpired:
        raise RuntimeError(f"sigrok-cli did not finish within {args.timeout}s")
    if proc.returncode != 0 or not proc.stdout:
        err = proc.stderr.decode(errors="replace").strip()
        if "Failed to detach kernel driver" in err or "LIBUSB_ERROR_ACCESS" in err:
            raise RuntimeError("cannot claim the analyser -- is PulseView open? It holds "
                               "the USB interface exclusively.\n  " + err)
        raise RuntimeError(err or "sigrok-cli produced no samples")
    buf = np.frombuffer(proc.stdout, dtype=np.uint8)
    if buf.size < args.samples // 2:
        raise RuntimeError(f"expected ~{args.samples} bytes of logic data, got {buf.size}")
    return buf


def stream_blocks(args):
    """Yield fixed-size blocks from a long-lived sigrok process.

    Restarting sigrok per capture costs the decode time as blind time -- measured 0.24 s
    of every 1.27 s, a 77% duty cycle. Reading blocks out of one running acquisition
    instead gives 99%, and the blocks are contiguous: the BCLK cadence continues across
    the seam (gap 18 vs a median of 17 samples), so nothing is missed between them.

    --continuous emits nothing on this build/driver, so each acquisition is bounded by
    --time and simply restarted when it ends. The restart IS a real gap, which is why
    it defaults to once a minute rather than once a second.
    """
    block = args.samples
    while True:
        cmd = [args.sigrok_cli,
               "-d", args.driver + (f":conn={args.conn}" if args.conn else ""),
               "-c", f"samplerate={args.samplerate}",
               "--time", str(int(args.stream_seconds * 1000)), "-O", "binary"]
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                bufsize=0)
        # sigrok's stderr must be drained continuously: if that 64 KB pipe fills, the
        # process blocks on write and the acquisition stops dead.
        errbuf = []
        threading.Thread(target=lambda: errbuf.append(proc.stderr.read()),
                         daemon=True).start()
        try:
            while True:
                chunks, got = [], 0
                while got < block:
                    d = proc.stdout.read(min(1 << 22, block - got))
                    if not d:
                        break
                    chunks.append(d)
                    got += len(d)
                if got < block:
                    break                      # acquisition ended; restart below
                yield np.frombuffer(b"".join(chunks), dtype=np.uint8)
        finally:
            if proc.poll() is None:
                proc.kill()
            err = (errbuf[0].decode(errors="replace").strip() if errbuf else "")
        if err and ("LIBUSB" in err or "Failed to open" in err):
            raise RuntimeError("cannot claim the analyser -- is PulseView open?\n  " + err)


def stream_reader(args, depth=3):
    """stream_blocks in a thread, buffered, so slow processing cannot stall sigrok.

    Anything that takes longer than a block -- the first log parse reads tens of MB, and
    was measured stalling the loop for 30 s -- otherwise stops the pipe being read, the
    acquisition backs up behind it, and coverage is silently lost. Here the reader keeps
    consuming; if the queue is full the block is DROPPED and counted, so falling behind
    shows up as a reported gap instead of a stall.
    """
    q = queue.Queue(maxsize=depth)
    state = {"dropped": 0, "error": None, "done": False}

    def run():
        try:
            for blk in stream_blocks(args):
                try:
                    q.put_nowait(blk)
                except queue.Full:
                    state["dropped"] += 1
        except Exception as e:                      # surfaced on the consumer side
            state["error"] = e
        finally:
            state["done"] = True
            try:
                q.put_nowait(None)
            except queue.Full:
                pass

    threading.Thread(target=run, daemon=True).start()

    def take():
        while True:
            if state["error"]:
                raise RuntimeError(str(state["error"]))
            try:
                blk = q.get(timeout=5.0)
            except queue.Empty:
                if state["done"]:
                    raise RuntimeError("stream ended")
                continue
            if blk is None:
                raise RuntimeError("stream ended")
            return blk

    return take, state


def bit(buf, ch):
    return ((buf >> ch) & 1).astype(bool)


def rising(d):
    return np.flatnonzero(~d[:-1] & d[1:]) + 1


def falling(d):
    return np.flatnonzero(d[:-1] & ~d[1:]) + 1


def clock_health(bclk):
    """(median BCLK period, worst/median ratio) -- a dropped chunk shows as a huge gap.

    Returned rather than raised on, so the caller can report it alongside the numbers it
    would invalidate.
    """
    r = rising(bclk)
    if r.size < 64:
        return float("nan"), float("nan")
    g = np.diff(r).astype(np.float64)
    med = float(np.median(g))
    return med, float(g.max() / med) if med > 0 else float("nan")


def decode_i2s(bclk, ws, din, bits=None, delay=None):
    """Decode one I2S bus.

    Returns (left, right, frame_t, bits, delay):
      left/right  int arrays of PCM sample values
      frame_t     analyser sample index of the LRC falling edge starting each frame,
                  i.e. the precise TIME of each frame, which is what the skew is
                  ultimately measured from
      bits/delay  what was used, whether passed in or detected

    Data is sampled on BCLK rising edges, MSB first. Standard I2S puts the first data
    bit one BCLK after the LRC transition; some implementations do not. Rather than
    trust either, both are decoded and the one whose output looks like audio wins --
    misalignment shreds a waveform into noise, so sample-to-sample correlation
    separates them unambiguously.
    """
    br = rising(bclk)
    if br.size < 64:
        raise RuntimeError("no BCLK edges -- is BCLK probed and the bus running?")
    ws_at = ws[br]
    din_at = din[br]
    # LRC transitions as seen on the BCLK grid, and the true edge times off the raw line
    turns = np.flatnonzero(ws_at[:-1] != ws_at[1:]) + 1
    if turns.size < 4:
        raise RuntimeError("no LRC transitions -- is LRC probed?")
    if bits is None:
        # Slot width is the spacing between LRC transitions in BCLK cycles.
        bits = int(round(float(np.median(np.diff(turns)))))
        if not 8 <= bits <= 32:
            raise RuntimeError(f"implausible slot width of {bits} BCLK cycles")

    def build(dly):
        starts = turns[:-1]
        n = starts.size
        idx = starts[:, None] + dly + np.arange(bits)[None, :]
        ok = idx[:, -1] < din_at.size
        idx, sel = idx[ok], starts[ok]
        b = din_at[idx].astype(np.int64)
        v = np.zeros(idx.shape[0], dtype=np.int64)
        for k in range(bits):
            v = (v << 1) | b[:, k]
        sign = 1 << (bits - 1)
        v = np.where(v >= sign, v - (1 << bits), v)
        return v, ws_at[sel], sel

    def audioness(v):
        if v.size < 32:
            return -1.0
        u = v - v.mean()
        return -1.0 if u.std() == 0 else float(np.mean(u[:-1] * u[1:]) / np.var(u))

    if delay is None:
        cands = [(audioness(build(d)[0]), d) for d in (1, 0)]
        delay = max(cands)[1]
    v, phase, sel = build(delay)
    # ws low = left channel in standard I2S
    left, right = v[~phase], v[phase]
    # Frame time: the LRC FALLING edge on the raw line, found near each left-slot start.
    fall = falling(ws)
    lt = br[sel[~phase]]
    j = np.clip(np.searchsorted(fall, lt), 0, max(fall.size - 1, 0))
    j = np.where((j > 0) & (np.abs(fall[np.maximum(j - 1, 0)] - lt) <
                            np.abs(fall[j] - lt)), j - 1, j)
    frame_t = fall[j].astype(np.float64)
    n = min(left.size, right.size, frame_t.size)
    return left[:n], right[:n], frame_t[:n], bits, delay


def frame_lag(a, b, maxlag, prefer=None):  # prefer kept for call compatibility
    """Integer frame offset of b relative to a, by normalised PCM correlation.

    Scale invariant, so a volume or gain difference between boards is irrelevant --
    which is exactly why this works where correlating raw DIN bits did not.

    Returns (lag, coefficient, rival ratio). Repetitive audio puts near-equal peaks at
    the content's own period, and the tallest is then a coin toss: measured on a real
    run, six captures agreed at -337 frames while one picked +264, a 13.6 ms jump that
    is physically impossible between captures 2 s apart. So when a previous lag is
    known, any peak within RIVAL_MARGIN of the best counts as a tie and the one nearest
    that previous lag wins. Continuity is only a tiebreak -- a genuinely taller peak
    still moves the answer, so a real resync is still tracked.
    """
    n = min(a.size, b.size)
    if n < MIN_FRAMES:
        return 0, 0.0, 1.0, 0.0
    x = a[:n] - a[:n].mean()
    y = b[:n] - b[:n].mean()
    nx, ny = np.linalg.norm(x), np.linalg.norm(y)
    if nx == 0 or ny == 0:
        return 0, 0.0, 1.0, 0.0
    L = 1 << (2 * n - 1).bit_length()
    c = np.fft.irfft(np.fft.rfft(x, L).conj() * np.fft.rfft(y, L), L) / (nx * ny)
    maxlag = int(min(maxlag, n - 1, L // 2 - 1))
    idx = np.concatenate((np.arange(L - maxlag, L), np.arange(0, maxlag + 1)))
    w = np.abs(c[idx])
    kbest = int(np.argmax(w))
    best = float(w[kbest])
    # Rival: the tallest peak outside a small guard around the winner.
    m = w.copy()
    m[max(0, kbest - 8):kbest + 9] = 0
    krival = int(np.argmax(m))
    rival = float(m[krival] / (best + 1e-12))
    # k is the plain argmax on purpose. Biasing it toward the previous capture's value
    # was wrong: which content frame each board's decode starts on depends on where the
    # capture window fell relative to that board's LRC edges, so the integer lag varies
    # between captures by design. Pinning it produced a 22.7 us square wave, and it also
    # corrupted the interpolation below by centring it on the wrong peak. Protection
    # against the genuinely bad case -- a repetitive passage matching hundreds of frames
    # away -- belongs at the main loop's --max-jump-frames gate, which can see the whole
    # series, rather than here.
    k = kbest
    # Sub-frame refinement of the peak. Both boards sample the SAME continuous waveform,
    # just at times differing by the skew, so the correlation peak sits at
    # skew/frame_period and its fractional part is a coarse but unambiguous estimate of
    # the sub-frame offset. That is what resolves which whole frame the precise LRC
    # measurement belongs to.
    frac = 0.0
    if 0 < k < w.size - 1:
        y0, y1, y2 = float(w[k - 1]), float(w[k]), float(w[k + 1])
        den = y0 - 2 * y1 + y2
        if abs(den) > 1e-12:
            frac = float(np.clip(0.5 * (y0 - y2) / den, -0.5, 0.5))
    return k - maxlag, float(w[k]), rival, frac


def rolling_lag(la, lb, k0, win, hop, span):
    """Frame lag as a function of time WITHIN one capture: [(frame_index, lag), ...].

    The capture-level correlation yields a single alignment, which is wrong if the
    boards step mid-capture -- a hard resync inside a 1 s window would be averaged
    across rather than seen. This re-estimates the lag in a sliding window.

    The search is deliberately confined to k0 +- span. An unconstrained short window is
    not trustworthy: with the boards 194 ms apart and no shared audio, 12 ms windows
    happily produced coefficients of 0.9 at lags scattered over +-174 frames. Anchoring
    to the capture-level answer means this can only report a CHANGE of a few frames,
    which is exactly what a resync looks like, and cannot invent a large one.
    """
    n = min(la.size, lb.size)
    out = []
    if n < win + hop:
        return out
    span = int(span)
    for start in range(0, n - win, hop):
        x = la[start:start + win].astype(np.float64)
        x = x - x.mean()
        nx = np.linalg.norm(x)
        if nx == 0:
            continue
        best, bestc = k0, -2.0
        for d in range(k0 - span, k0 + span + 1):
            lo, hi = start + d, start + d + win
            if lo < 0 or hi > n:
                continue
            y = lb[lo:hi].astype(np.float64)
            y = y - y.mean()
            ny = np.linalg.norm(y)
            if ny == 0:
                continue
            c = float(np.dot(x, y) / (nx * ny))
            if c > bestc:
                best, bestc = d, c
        out.append((start + win // 2, best, bestc))
    return out


def skew_series(buf, chan, args, prefer=None):
    """Per-frame skew between the boards: (skew_ns, frame_rate, info).

    One value per matched frame pair -- ~44100 per second of capture -- each the time
    between the two boards' LRC edges for frames carrying the same audio. This is the
    raw material for both the single-number offset and the stability statistics; the
    latter needs the whole series, since what varies is not the mean but the wander.
    """
    ns = 1e9 / args_rate_hz(args)
    la, _, ta, bits_a, dly_a = decode_i2s(
        bit(buf, chan["BCLK_ONE"]), bit(buf, chan["LRC_ONE"]),
        bit(buf, chan["DIN_ONE"]), args.bits, args.bit_delay)
    lb, _, tb, bits_b, dly_b = decode_i2s(
        bit(buf, chan["BCLK_TWO"]), bit(buf, chan["LRC_TWO"]),
        bit(buf, chan["DIN_TWO"]), args.bits, args.bit_delay)

    def frame_rate(t):
        """By regression, not median-of-diffs: at 24 MS/s a 44.1 kHz frame is 544.2
        samples, so a median of integer diffs always reads exactly 24e6/544."""
        if t.size < 32:
            return float("nan")
        sl = np.polyfit(np.arange(t.size, dtype=np.float64), t, 1)[0]
        return args_rate_hz(args) / sl if sl > 0 else float("nan")

    info = {"frames": (la.size, lb.size), "bits": (bits_a, bits_b),
            "delay": (dly_a, dly_b), "fs": (frame_rate(ta), frame_rate(tb))}
    for tag, ch in (("A", "BCLK_ONE"), ("B", "BCLK_TWO")):
        per, ratio = clock_health(bit(buf, chan[ch]))
        info[f"bclk_{tag}"], info[f"gap_{tag}"] = per, ratio

    # The search range is bounded by the capture itself: an offset larger than the
    # window means the two boards share no audio in it, so there is nothing to match.
    # Default to 40% of the window, which keeps at least 60% overlap at full deflection.
    nframes = min(la.size, lb.size)
    if args.maxlag_ms:
        maxlag = max(4, int(args.maxlag_ms * 1e-3 * info["fs"][0])) \
            if np.isfinite(info["fs"][0]) else 4096
    else:
        maxlag = max(4, int(nframes * 0.40))
    info["window_ms"] = nframes / info["fs"][0] * 1e3 if np.isfinite(info["fs"][0]) else 0
    k, coef, rival, frac = frame_lag(la, lb, maxlag, prefer)
    info["frame_lag"], info["coef"], info["rival"] = k, coef, rival
    info["overlap"] = max(0.0, 1.0 - abs(k) / max(nframes, 1))
    if coef < MIN_PCM_COEF:
        # Distinguish "no match" from "match is outside what this capture can see".
        # Measured: with the boards 194 ms apart a 100 ms capture shares no audio at
        # all and correlates noise at coef 0.40 -- indistinguishable from a fault
        # unless the window limit is stated.
        info["hint"] = (f"best coef {coef:.2f} over a {info['window_ms']:.0f} ms window; "
                        f"if the boards are further apart than that they share no audio "
                        f"in it -- raise --samples (1 s = 24000000 at 24 MS/s)")
        return np.empty(0), info["fs"][0], info
    # frame_lag returns k with b[m] == a[m - k], so board B's frame m carries the audio
    # board A played in its frame m-k; the skew is between those frames' LRC edges. This
    # is where the absolute, non-modulo offset comes from: the PCM match gives the
    # whole-frame part, the edge times the sub-frame remainder.
    m_ = np.arange(max(0, k), min(tb.size, ta.size + k))
    ib, ia = m_, m_ - k
    keep = (ia >= 0) & (ia < ta.size) & (ib >= 0) & (ib < tb.size)
    ia, ib = ia[keep], ib[keep]
    if ia.size < MIN_FRAMES:
        return np.empty(0), info["fs"][0], info
    info["pairs"] = int(ia.size)
    # The pairing is content-correct by construction, so this is the physical skew --
    # up to a whole-frame ambiguity when the correlation picks a neighbouring lag, which
    # adjacent audio frames (lag-1 autocorrelation ~0.997) make easy. That ambiguity is
    # resolved by continuity in the caller, not here: within a capture every frame shares
    # one k, so the series is internally consistent regardless.
    skew = (tb[ib] - ta[ia]) * ns
    return skew, info["fs"][0], info


def measure_capture(buf, chan, args, prefer=None, dump=None, dump_t0=0.0):
    """One capture -> offset at midpoint (ns), rate difference (ppm), diagnostics."""
    skew, fr, info = skew_series(buf, chan, args, prefer)
    if dump is not None and skew.size and np.isfinite(fr):
        # One row per audio frame: the LRC edges already give ~44100 skew values per
        # second of capture, which the per-capture row reduces to a line fit. Formatted
        # in one numpy call -- a Python loop over 44k rows per block is real time in a
        # loop that only has ~0.7 s of slack.
        t = dump_t0 + np.arange(skew.size) / fr
        np.savetxt(dump, np.column_stack((t, skew)), fmt="%.6f,%.1f")
    if skew.size == 0 or not np.isfinite(fr):
        return float("nan"), float("nan"), info.get("coef", 0.0), info
    t = np.arange(skew.size, dtype=np.float64) / fr        # seconds within the capture
    A = np.polyfit(t, skew, 1)
    resid = skew - np.polyval(A, t)
    good = np.abs(resid) < 4 * (np.median(np.abs(resid)) + 1e-9)
    if good.sum() > MIN_FRAMES:
        A = np.polyfit(t[good], skew[good], 1)
    info["scatter_ns"] = float(np.median(np.abs(skew - np.polyval(A, t))))
    # A[0] is ns of skew per second of elapsed time. A dimensionless rate error is that
    # over 1e9, and ppm multiplies by 1e6 -- so the conversion is /1000, not *1e6.
    return (float(np.polyval(A, float(np.mean(t)))), float(A[0] * 1e-3),
            info["coef"], info)


def write_wavs(prefix, la, lb, fs):
    """Dump both boards' decoded left channels as WAV, for listening or eyeballing.

    Not part of the measurement -- the correlation runs on the arrays directly, so no
    WAV round trip is needed -- but it is the quickest way to confirm by ear that the
    decode is real audio and that both boards carry the same content.
    """
    import wave
    for name, v in (("a", la), ("b", lb)):
        if v.size == 0:
            continue
        pk = float(np.max(np.abs(v))) or 1.0
        pcm = (v / pk * 32000).astype("<i2")
        path = f"{prefix}-{name}.wav"
        with wave.open(path, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(int(fs) if np.isfinite(fs) and fs > 1000 else 44100)
            w.writeframes(pcm.tobytes())
        print(f"  wrote {path} ({v.size} frames)")


def args_rate_hz(args):
    mult = {"K": 1e3, "M": 1e6, "G": 1e9}
    s = str(args.samplerate).strip()
    return float(s[:-1]) * mult[s[-1].upper()] if s[-1].upper() in mult else float(s)


def make_simulator(args):
    """Rasterise two synthetic I2S buses with a known offset and known clock error.

    Real captures give no ground truth, so this is the only check that the sign
    convention and the magnitude are right, not merely self-consistent.
    """
    fs = args_rate_hz(args)
    bits = args.bits or 16
    chan = {"BCLK_ONE": 2, "LRC_ONE": 4, "DIN_ONE": 1,
            "BCLK_TWO": 5, "LRC_TWO": 3, "DIN_TWO": 7}
    state = {"i": 0}

    def content(stream_idx):
        """Band-limited, non-repeating enough to correlate: audio-like, not noise."""
        t = stream_idx / args.sim_frame_rate
        v = (0.45 * np.sin(2 * np.pi * 437.0 * t) +
             0.28 * np.sin(2 * np.pi * 1103.0 * t + 0.7) +
             0.16 * np.sin(2 * np.pi * 2731.0 * t + 1.9))
        return np.rint(v * 12000).astype(np.int64)

    def raster(buf, n, t0, tframe, ch_bclk, ch_ws, ch_din, stream_shift):
        tbit = tframe / (2 * bits)
        u = (np.arange(n, dtype=np.float64) - t0) / tbit      # bit-time
        i = np.floor(u).astype(np.int64)
        frac = u - i
        # Nothing before this board's own start: rasterising negative bit-times gave the
        # delayed board a partial leading frame, shifting its frame indexing by one and
        # making the measured offset differ from the truth by exactly one frame period.
        live = i >= 0
        buf[(frac >= 0.5) & live] |= np.uint8(1 << ch_bclk)   # BCLK rising at frac .5
        slot = np.floor_divide(i, bits)
        buf[(slot & 1).astype(bool) & live] |= np.uint8(1 << ch_ws)  # ws high = right
        # One BCLK of delay: bit j of a slot's sample appears in bit-time slot*bits+1+j
        j = i - 1 - slot * bits
        valid = (j >= 0) & (j < bits) & (slot >= 0) & live
        frame = np.floor_divide(slot, 2)
        vals = content(np.arange(0, int(n / tframe) + 4) + stream_shift)
        fi = np.clip(frame, 0, vals.size - 1)
        v = (vals[fi] & ((1 << bits) - 1))
        bitval = (v >> (bits - 1 - np.clip(j, 0, bits - 1))) & 1
        buf[valid & (bitval == 1)] |= np.uint8(1 << ch_din)

    def capture():
        idx = state["i"]
        state["i"] += 1
        n = args.samples
        buf = np.zeros(n, dtype=np.uint8)
        tframe_a = fs / args.sim_frame_rate
        eps = args.sim_ppm * 1e-6
        tframe_b = tframe_a * (1.0 + eps)
        # Offset accumulates between captures exactly as two real clocks would.
        off = args.sim_offset_ns * 1e-9 * fs + \
            idx * args.interval * eps * fs
        # Split the offset the way real hardware does: a board that lags by more than a
        # frame is emitting content its partner emitted whole frames ago, so the delay
        # appears BOTH as a content shift and as a sub-frame timing difference. Modelling
        # it purely as timing (content-synchronous but seconds late) is impossible in
        # practice and made the frame-resolution logic look broken when it was right.
        d = int(round(off / tframe_a))
        r = off - d * tframe_a
        raster(buf, n, 0.0, tframe_a, chan["BCLK_ONE"], chan["LRC_ONE"], chan["DIN_ONE"], 0)
        raster(buf, n, r, tframe_b, chan["BCLK_TWO"], chan["LRC_TWO"], chan["DIN_TWO"], -d)
        state["true_lag"] = d
        # Truth at the capture midpoint, in the same terms measure_capture reports.
        f_mid = (n / 2) / tframe_a
        state["true_ns"] = (off + f_mid * (tframe_b - tframe_a)) / fs * 1e9
        return buf

    return capture, chan, state


# Sync report, e.g.
#   [16:50:19.830][D][snapclient.client:2070][snap_player]: Sync: avg .. median 153 us |
#   corrected -0/+1008 frames, 3 hard resyncs, .. trim +163.22 ppm (idle), ..
# Temperature is not in these logs yet, so match the shapes ESPHome actually emits
# rather than one guessed format: a named sensor state, an explicit temp= field, or a
# bare value with a degree unit. \btemp avoids matching "attempt=".
# Temperature can arrive in several shapes, so match what firmwares actually emit
# rather than betting on one. Each entry is (regex, name group, value group); a name
# group of 0 means the pattern carries no sensor name.
#   'ESP Temperature': Sending state 47.7 °C     -- the stock ESPHome sensor dump
#   DIETEMP 66.100 C                             -- a logger.log format string
#   temperature=51.2 / soc temp: 49.5            -- an explicit field
TEMP_STAMP = r"^\[(\d\d):(\d\d):(\d\d)\.(\d+)\]"
TEMP_RES = (
    (re.compile(TEMP_STAMP + r".*?'([^']+)':\s*Sending state\s*(-?\d+(?:\.\d+)?)"
                r"\s*(?:\u00b0\s*)?C\b"), 5, 6),
    # A name token containing "temp" followed by a value AND a C unit. Requiring the
    # unit is what keeps "attempt=3" out -- "attempt" does contain "temp".
    (re.compile(TEMP_STAMP + r".*?\b([A-Za-z][A-Za-z_]*temp[a-z_]*)\b[\s:=]+"
                r"(-?\d+(?:\.\d+)?)\s*(?:\u00b0\s*)?C\b", re.I), 5, 6),
    (re.compile(TEMP_STAMP + r".*?\btemp(?:erature)?[_a-z]*\s*[=:]\s*"
                r"(-?\d+(?:\.\d+)?)"), 0, 5),
    (re.compile(TEMP_STAMP + r".*?(-?\d+(?:\.\d+)?)\s*\u00b0C\b"), 0, 5),
)


def match_temperature(line):
    """(time_of_day_s, sensor_name, celsius) or None."""
    for rx, ni, vi in TEMP_RES:
        m = rx.match(line)
        if not m:
            continue
        tod = (int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3))
               + int(m.group(4)) / (10 ** len(m.group(4))))
        name = m.group(ni).strip() if ni else "temp"
        return tod, name, float(m.group(vi))
    return None


# Above this many samples the plot is decimated for drawing (the data is untouched).
MAX_PLOT_POINTS = 2500

# TSF role, e.g. "tsf=leader(peers 5)" / "tsf=follower(0.9s, depth +2267 us)". Matched on
# the first letter only: the Sync line is long and the logger truncates it mid-token, so
# "tsf=follow", "tsf=foll" and "tsf=le" all occur and must not be missed.
ROLE_RE = re.compile(r"^\[(\d\d):(\d\d):(\d\d)\.(\d+)\].*?\btsf=([lf])")


LOG_COVERAGE = {}
LOG_STATE = {}
TEMPS = {}

SYNC_RE = re.compile(
    r"^\[(\d\d):(\d\d):(\d\d)\.(\d+)\].*?"
    r"corrected -(\d+)/\+(\d+) frames,\s*(\d+) hard resyncs.*?"
    r"trim ([+-][\d.]+) ppm")


def parse_sync_events(path, board, trim_ppm, start_offset=0, span=None, state=None,
                      sync_us=200, peak_us=600, pipe_ms=25, tail_bytes=0):
    """Clock-affecting events from a device log: (time_of_day_s, kind, text).

    Three kinds, all read off the one Sync line the firmware emits every ~3.3 s:
      corrected  a nonzero frame correction -- playout physically stepped
      resync     the hard-resync counter advanced
      trim       the resampler rate moved by at least trim_ppm

    trim moves on almost every report, so annotating every change would bury the plot;
    only steps past the threshold are marked. Returns the new end-of-file offset too, so
    a long run can tail the log rather than re-reading megabytes each time.
    """
    # State must survive across polls. A live run tails the log every capture and each
    # poll sees only a line or two, so re-baselining per call would compare a value
    # against nothing and emit almost no events -- the annotations would silently dry up
    # the moment the run went live, having worked fine on a whole-file --replot.
    state = state if state is not None else {}
    ev, temps = [], []
    last_trim, last_resync = state.get("trim"), state.get("resync")
    last_pipe = state.get("pipe")
    last_role = state.get("role")
    try:
        f = open(path, errors="replace")
    except OSError:
        return [], start_offset, state, []
    if start_offset == 0 and tail_bytes:
        try:
            size = os.fstat(f.fileno()).st_size
            if size > tail_bytes:
                f.seek(size - tail_bytes)
                f.readline()          # discard the partial line at the seek point
        except OSError:
            pass
    else:
        f.seek(start_offset)
    for line in f:
        # Role is read independently of SYNC_RE: a truncated line may lose the trim
        # field that SYNC_RE requires while still carrying the role.
        rm = ROLE_RE.match(line)
        if rm:
            tod_r = (int(rm.group(1)) * 3600 + int(rm.group(2)) * 60 + int(rm.group(3))
                     + int(rm.group(4)) / (10 ** len(rm.group(4))))
            role = "leader" if rm.group(5) == "l" else "follower"
            if last_role is not None and role != last_role:
                ev.append((tod_r, "role", f"{board}: {last_role} -> {role}"))
            last_role = role
        m = SYNC_RE.match(line)
        if not m:
            t = match_temperature(line)
            if t:
                temps.append((t[0], f"{board} {t[1]}", t[2]))
            continue
        tod = (int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3))
               + int(m.group(4)) / (10 ** len(m.group(4))))
        if span is not None:
            span[0] = tod if span[0] is None else min(span[0], tod)
            span[1] = tod if span[1] is None else max(span[1], tod)
        down, up, nres, trim = (int(m.group(5)), int(m.group(6)),
                                int(m.group(7)), float(m.group(8)))
        # The sync error itself is the best marker of what actually moves the wire.
        # Measured: peak(B-A) correlates -0.76 with the wire skew, median(B-A) -0.54,
        # while trim correlates only -0.23 -- trim is reported "(idle)", i.e. computed
        # but not applied, so it marks the controller reacting, not the disturbance.
        mm = re.search(r"median (-?\d+) us", line)
        pk = re.search(r"peak (-?\d+) us", line)
        pl = re.search(r"pipeline (-?\d+) ms", line)
        med = int(mm.group(1)) if mm else 0
        peak = int(pk.group(1)) if pk else 0
        if abs(med) >= sync_us or abs(peak) >= peak_us:
            ev.append((tod, "sync", f"{board}: median {med:+d} peak {peak} us"))
        if pl:
            pipe = int(pl.group(1))
            if last_pipe is not None and abs(pipe - last_pipe) >= pipe_ms:
                ev.append((tod, "pipeline",
                           f"{board}: pipeline {last_pipe}->{pipe} ms"))
            last_pipe = pipe
        if down or up:
            ev.append((tod, "corrected", f"{board}: corrected -{down}/+{up} frames"))
        if last_resync is not None and nres > last_resync:
            ev.append((tod, "resync", f"{board}: {nres - last_resync} hard resync"))
        last_resync = nres
        if last_trim is not None and abs(trim - last_trim) >= trim_ppm:
            ev.append((tod, "trim", f"{board}: trim {last_trim:+.0f}->{trim:+.0f} ppm"))
        last_trim = trim
    end = f.tell()
    f.close()
    state["trim"], state["resync"], state["pipe"] = last_trim, last_resync, last_pipe
    state["role"] = last_role
    return ev, end, state, temps


def tod_to_unix(tod, ref_unix):
    """Log lines carry a time of day with no date; place each on the day that puts it
    nearest the run. Handles a run that crosses midnight."""
    lt = time.localtime(ref_unix)
    # mktime with isdst=-1 resolves the local midnight correctly across a DST boundary.
    midnight = time.mktime((lt.tm_year, lt.tm_mon, lt.tm_mday, 0, 0, 0, 0, 0, -1))
    best = None
    for k in (-1, 0, 1):
        cand = midnight + k * 86400.0 + tod
        if best is None or abs(cand - ref_unix) < abs(best - ref_unix):
            best = cand
    return best


def write_svg(path, ts, ys, title, ylabel, include_zero=True,
              xlabel="elapsed (s)", log_axes=False, events=(), panel2=None, stats=None,
              panel2_label="temperature (\u00b0C)"):
    """Plot without a plotting dependency -- the rest of scripts/ is stdlib too.

    A second panel is drawn only when panel2 has data, so a run without temperature
    logging looks exactly as before. Both panels share the x axis and the event bars
    span both, which is the point: it should be obvious at a glance whether a skew
    excursion lines up with a temperature move.
    """
    W, ML, M, MB = 900, 108, 70, 70
    has2 = bool(panel2) and any(v for v in panel2.values())
    top_h = 280 if has2 else 280
    top0, top1 = M, M + top_h
    bot0, bot1 = (top1 + 62, top1 + 62 + 150) if has2 else (0, 0)
    H = (bot1 if has2 else top1) + MB

    pts = [(t, y) for t, y in zip(ts, ys) if math.isfinite(y)]
    if not pts:
        open(path, "w").write(
            f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}">'
            f'<text x="{W/2}" y="{H/2}" text-anchor="middle" font-family="sans-serif">'
            f'no valid measurements yet</text></svg>')
        return
    xs = [p[0] for p in pts]
    x0, x1 = min(xs), max(xs)
    if has2:
        for series in panel2.values():
            for x, _ in series:
                x0, x1 = min(x0, x), max(x1, x)
    if x1 - x0 < 1e-9:
        x1 = x0 + 1

    vs = [p[1] for p in pts]
    y0, y1 = min(vs), max(vs)
    if log_axes:
        include_zero = False
    if include_zero:
        y0, y1 = min(y0, 0.0), max(y1, 0.0)
    pad = (y1 - y0) * 0.1 or (abs(y0) * 0.1 or 1)
    y0, y1 = y0 - pad, y1 + pad

    def px(t):
        return ML + (t - x0) / (x1 - x0) * (W - ML - 40)

    def mk_py(p0, p1, v0, v1):
        return lambda v: p1 - (v - v0) / (v1 - v0 or 1) * (p1 - p0)

    py = mk_py(top0, top1, y0, y1)
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
         f'font-family="-apple-system,sans-serif" font-size="12">',
         f'<rect width="{W}" height="{H}" fill="white"/>',
         f'<text x="{ML}" y="26" font-size="15" font-weight="600">{title}</text>']
    if stats:
        o.append(f'<text x="{ML}" y="44" font-size="11" fill="#666">{stats}</text>')

    def axis(p0, p1, v0, v1, lab, pyf, xticks):
        for k in range(6):
            v = v0 + (v1 - v0) * k / 5
            yy = pyf(v)
            o.append(f'<line x1="{ML}" y1="{yy:.1f}" x2="{W-40}" y2="{yy:.1f}" '
                     f'stroke="#e5e5e5"/>')
            vv = 10 ** v if log_axes else v
            t_lbl = f"{vv:,.0f}" if abs(vv) >= 1000 else f"{vv:.3g}"
            o.append(f'<text x="{ML-8}" y="{yy+4:.1f}" text-anchor="end" '
                     f'fill="#555">{t_lbl}</text>')
        o.append(f'<line x1="{ML}" y1="{p1}" x2="{W-40}" y2="{p1}" stroke="#333"/>')
        o.append(f'<line x1="{ML}" y1="{p0}" x2="{ML}" y2="{p1}" stroke="#333"/>')
        o.append(f'<text x="16" y="{(p0+p1)/2}" text-anchor="middle" fill="#333" '
                 f'transform="rotate(-90 16 {(p0+p1)/2})">{lab}</text>')
        if xticks:
            for k in range(6):
                t = x0 + (x1 - x0) * k / 5
                xx = px(t)
                o.append(f'<line x1="{xx:.1f}" y1="{p1}" x2="{xx:.1f}" y2="{p1+5}" '
                         f'stroke="#888"/>')
                tt = 10 ** t if log_axes else t
                if not log_axes and abs(tt) < (x1 - x0) * 1e-3:
                    tt = 0.0
                o.append(f'<text x="{xx:.1f}" y="{p1+20}" text-anchor="middle" '
                         f'fill="#555">{tt:.4g}</text>')
            o.append(f'<text x="{W/2}" y="{p1+42}" text-anchor="middle" '
                     f'fill="#333">{xlabel}</text>')

    axis(top0, top1, y0, y1, ylabel, py, not has2)
    if y0 <= 0 <= y1:
        z = py(0.0)
        o.append(f'<line x1="{ML}" y1="{z:.1f}" x2="{W-40}" y2="{z:.1f}" stroke="#888" '
                 f'stroke-width="1.2" stroke-dasharray="2 3"/>')
        o.append(f'<text x="{W-36}" y="{z+4:.1f}" fill="#888">0</text>')

    bar_bottom = bot1 if has2 else top1
    colours = {"corrected": "#d9534f", "resync": "#8e44ad", "trim": "#e0a800",
               "sync": "#1a7f37", "pipeline": "#0969da", "role": "#000000"}
    for i, (ex, kind, label) in enumerate(sorted(events)):
        if not (x0 <= ex <= x1):
            continue
        xx = px(ex)
        col = colours.get(kind, "#888")
        o.append(f'<line x1="{xx:.1f}" y1="{top0}" x2="{xx:.1f}" y2="{bar_bottom}" '
                 f'stroke="{col}" stroke-width="1.1" stroke-dasharray="4 3" '
                 f'opacity="0.75"/>')
        ly = top1 - 6 - (i % 3) * 12
        txt = label if len(label) <= 30 else label[:29] + "\u2026"
        o.append(f'<text x="{xx-3:.1f}" y="{ly:.1f}" font-size="9" fill="{col}" '
                 f'transform="rotate(-90 {xx-3:.1f} {ly:.1f})">{txt}</text>')

    # Decimate for drawing: beyond a few thousand points the extra vertices are below
    # one pixel, and a run of 100k captures would otherwise emit a multi-megabyte SVG
    # that is rewritten after every capture.
    draw = pts
    if len(pts) > MAX_PLOT_POINTS:
        step = len(pts) / MAX_PLOT_POINTS
        draw = [pts[min(int(i * step), len(pts) - 1)] for i in range(MAX_PLOT_POINTS)]
        draw.append(pts[-1])
    o.append(f'<polyline points="{" ".join(f"{px(t):.1f},{py(v):.1f}" for t, v in draw)}" '
             f'fill="none" stroke="#2b6cb0" stroke-width="1.5"/>')
    if len(draw) <= 800:
        for t, v in draw:
            o.append(f'<circle cx="{px(t):.1f}" cy="{py(v):.1f}" r="2.5" fill="#2b6cb0"/>')

    if has2:
        allv = [v for series in panel2.values() for _, v in series]
        t0_, t1_ = min(allv), max(allv)
        tp = (t1_ - t0_) * 0.15 or 1.0
        t0_, t1_ = t0_ - tp, t1_ + tp
        py2 = mk_py(bot0, bot1, t0_, t1_)
        axis(bot0, bot1, t0_, t1_, panel2_label, py2, True)
        palette = ["#c2410c", "#0f766e", "#7c3aed", "#a16207", "#be123c"]
        for i, (name, series) in enumerate(sorted(panel2.items())):
            if not series:
                continue
            col = palette[i % len(palette)]
            pl = " ".join(f"{px(x):.1f},{py2(v):.1f}" for x, v in sorted(series))
            o.append(f'<polyline points="{pl}" fill="none" stroke="{col}" '
                     f'stroke-width="1.5"/>')
            # Left-aligned inside the panel: at the right edge it was easy to miss.
            o.append(f'<text x="{ML+8}" y="{bot0+14+i*13}" font-size="10" '
                     f'fill="{col}">{name}</text>')
    o.append("</svg>")
    open(path, "w").write("\n".join(o))


SCHEMA = "elapsed_s,unix_s,offset_ns,ppm,pcm_coef,frame_lag,rival,scatter_ns,reason"


DUMP_SCHEMA = "elapsed_s,skew_ns"


def load_existing(path):
    """Rows from a previous run, but only if the file is really ours.

    Returns (elapsed, offsets_ns, anchor) where anchor is the wall-clock time that
    row 0's elapsed=0 corresponded to -- needed so an appended run continues the same
    time axis, with the real gap between runs visible rather than collapsed.
    """
    ts, ys, anchor = [], [], None
    if not os.path.exists(path):
        return ts, ys
    head = ""
    for line in open(path):
        if line.startswith("elapsed_s"):
            head = line.strip()
            break
    if head == DUMP_SCHEMA:
        # A --dump-skew file: one row per audio frame rather than one per block. Same
        # quantity, ~440x denser, so it plots through the identical path.
        for line in open(path):
            if line[:1] in "#e":
                continue
            f = line.strip().split(",")
            if len(f) >= 2:
                try:
                    ts.append(float(f[0]))
                    ys.append(float(f[1]))
                except ValueError:
                    pass
        return ts, ys, None
    if head and head != SCHEMA:
        sys.exit(f"{path} has a different column layout:\n  found  {head}\n  "
                 f"expect {SCHEMA}\nPass a different --out, or move the old file aside.")
    for line in open(path):
        if line[:1] in "#e":
            continue
        f = line.strip().split(",")
        if len(f) >= 3:
            try:
                el, un = float(f[0]), float(f[1])
                ts.append(el)
                ys.append(float(f[2]))
                if anchor is None:
                    anchor = un - el
            except ValueError:
                pass
    return ts, ys, anchor


def trim_temps(temps, lo, hi):
    """Temperature points inside the run window, dropping series with nothing in it.

    The first poll reads a whole log file, which may hold hours of history; without
    this the second panel's x range would dwarf the run and flatten it to a line.
    """
    out = {}
    for name, series in temps.items():
        keep = [(x, v) for x, v in series if lo <= x <= hi]
        if len(keep) >= 2:
            out[name] = keep
    return out


def stats_caption(ts, ys):
    """One-line summary for the plot header: spread first, slope last."""
    v = [y for y in ys if math.isfinite(y)]
    if not v:
        return ""
    parts = [f"n={len(v)}", f"mean {np.mean(v)/1000:+.3f} us",
             f"sd {np.std(v)/1000:.3f} us",
             f"p2p {(max(v)-min(v))/1000:.3f} us"]
    f = fit_slope(ts, ys)
    if f:
        parts.append(f"slope {f[0]:+,.1f} ns/s ({f[0]/1000:+.4f} ppm)")
    return "   ".join(parts)


def fit_slope(ts, ys):
    """Least-squares slope in y-units per second. Reported as a number only.

    Not drawn: across a series that wanders and resyncs, a straight line through the
    whole run describes nothing and reads as a trend that is not there. The value is
    still worth having next to the spread.
    """
    pts = [(t, y) for t, y in zip(ts, ys) if math.isfinite(y)]
    if len(pts) < 3:
        return None
    t = np.array([p[0] for p in pts])
    y = np.array([p[1] for p in pts])
    if np.ptp(t) < 1e-9:
        return None
    sl, ic = np.polyfit(t, y, 1)
    return float(sl), float(ic)


STABILITY_TAUS = (0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 2.0)


def run_stability(args, chan, capture):
    """How in-sync the boards STAY, as a function of the interval you care about.

    A single "how in-sync are we" number does not exist here: the offset wanders, so its
    spread grows with the observation window. Two metrics, both per interval tau:

      MTIE(tau)  the worst peak-to-peak skew inside any window of length tau. This is
                 the number you can quote as a guarantee -- "over any 100 ms the boards
                 stay within X".
      TDEV(tau)  the time deviation, from second differences of tau-averages. The
                 conventional stability measure; its slope against tau says whether you
                 are looking at white noise, a random walk, or a systematic rate error.

    Confidence comes from the window count, which is reported rather than assumed:
    the relative 95% interval on TDEV is about 1.96/sqrt(2*windows), so pinning a tau
    to +-10% needs a total run of roughly 200*tau. Windows only ever come from WITHIN a
    capture, never spanning the gap between two, so a tau larger than the chunk is
    simply not reported instead of being silently fabricated.
    """
    fr_nominal = 44100.0
    chunk_s = args.samples / args_rate_hz(args)
    print(f"stability: {args.duration:g} s of capture in {chunk_s:.2f} s chunks at "
          f"{args.samplerate} ({args.samples/1e6:.1f} MB each)")
    print(f"  tau is only reported where a chunk holds >=3 windows, so tau <= "
          f"{chunk_s/3:.2f} s here\n")

    acc = {tau: {"mtie": 0.0, "d2sq": 0.0, "nd2": 0, "nwin": 0} for tau in STABILITY_TAUS}
    means, prefer, pending, segs, captured, resyncs, dropped = [], None, None, 0, 0.0, 0, 0
    try:
        while captured < args.duration:
            try:
                buf = capture()
                skew, fr, info = skew_series(buf, chan, args, prefer)
            except RuntimeError as e:
                print(f"  capture failed: {e}", file=sys.stderr)
                break
            if max(info.get("gap_A", 0) or 0, info.get("gap_B", 0) or 0) > 3:
                dropped += 1
                print(f"  chunk {segs+1}: discarded, analyser dropped samples")
                continue
            if skew.size < MIN_FRAMES or not np.isfinite(fr):
                print(f"  chunk {segs+1}: discarded, no frame match "
                      f"(coef {info.get('coef',0):.2f})")
                continue
            k = info["frame_lag"]
            if prefer is not None and abs(k - prefer) > args.max_jump_frames:
                if pending is not None and abs(k - pending) <= 4:
                    prefer, pending, resyncs = k, None, resyncs + 1
                    print(f"  chunk {segs+1}: frame lag moved to {k} -- treated as a "
                          f"resync; earlier chunks stay in the statistics")
                else:
                    pending = k
                    print(f"  chunk {segs+1}: discarded, frame lag jumped "
                          f"{k - prefer:+d} frames (unconfirmed)")
                    continue
            else:
                prefer, pending = k, None

            segs += 1
            captured += skew.size / fr
            mval = float(skew.mean())
            fperiod = 1e9 / fr if fr > 0 else 0.0
            if means and fperiod > 0:
                dv = mval - means[-1]
                cand = int(np.clip(round(-dv / fperiod), -2, 2))
                if cand and abs(dv + cand * fperiod) < 0.3 * fperiod:
                    mval += cand * fperiod
            means.append(mval)
            for tau in STABILITY_TAUS:
                n = int(round(tau * fr))
                if n < 2 or n > skew.size:
                    continue
                w = skew[:(skew.size // n) * n].reshape(-1, n)
                a = acc[tau]
                a["mtie"] = max(a["mtie"], float((w.max(1) - w.min(1)).max()))
                a["nwin"] += w.shape[0]
                if w.shape[0] >= 3:
                    av = w.mean(1)
                    d2 = av[2:] - 2 * av[1:-1] + av[:-2]
                    a["d2sq"] += float(np.sum(d2 ** 2))
                    a["nd2"] += d2.size
            print(f"  chunk {segs}: {skew.size} frames, mean {skew.mean():+,.0f} ns, "
                  f"coef {info['coef']:.4f}, lag {k}   [{captured:.1f}/{args.duration:g} s]")
    except KeyboardInterrupt:
        print("\ninterrupted")

    if not segs:
        sys.exit("no usable chunks -- nothing to report")

    rows = []
    print(f"\n{'tau':>9} {'MTIE':>12} {'TDEV':>11} {'TDEV 95% CI':>13} {'windows':>9}")
    for tau in STABILITY_TAUS:
        a = acc[tau]
        if a["nwin"] == 0:
            continue
        tdev = math.sqrt(a["d2sq"] / (6 * a["nd2"])) if a["nd2"] else float("nan")
        ci = 1.96 * tdev / math.sqrt(2 * a["nd2"]) if a["nd2"] else float("nan")
        rows.append((tau, a["mtie"], tdev, ci, a["nwin"]))
        t_s = f"{tdev:10,.0f}n" if math.isfinite(tdev) else f"{'--':>11}"
        c_s = f"+-{ci:9,.0f}n" if math.isfinite(ci) else f"{'--':>13}"
        print(f"{tau:8.3f}s {a['mtie']:11,.0f}n {t_s} {c_s} {a['nwin']:9,d}")

    print(f"\n  {segs} chunks, {captured:.1f} s analysed"
          + (f", {dropped} discarded for dropped samples" if dropped else "")
          + (f", {resyncs} resync(s)" if resyncs else ""))
    print(f"  chunk mean skew: {np.mean(means):+,.0f} ns, spread across chunks "
          f"{np.std(means):,.0f} ns (min {min(means):+,.0f}, max {max(means):+,.0f})")
    print("  quote MTIE at a tau whose window count is large; +-10% on TDEV needs "
          "~200*tau of total run")
    with open(args.out, "w") as f:
        f.write("# i2s-skew.py --stability\n")
        f.write("tau_s,mtie_ns,tdev_ns,tdev_ci95_ns,windows\n")
        for tau, mtie, tdev, ci, nw in rows:
            f.write(f"{tau:g},{mtie:.1f},{tdev:.1f},{ci:.1f},{nw}\n")
    if rows:
        write_svg(args.plot, [math.log10(r[0]) for r in rows],
                  [math.log10(max(r[1], 1e-9)) for r in rows],
                  "MTIE: worst peak-to-peak skew within any window",
                  "MTIE (ns)", include_zero=False,
                  xlabel="interval tau (s)", log_axes=True)
    print(f"  data {args.out}   plot {args.plot}")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--interval", type=float, default=5.0)
    p.add_argument("--count", type=int, default=0, help="captures (0 = until Ctrl-C)")
    p.add_argument("--samples", type=int, default=DEFAULT_SAMPLES,
                   help="samples per capture (2.4M = 100 ms at 24 MS/s ~ 4400 frames)")
    p.add_argument("--samplerate", default=DEFAULT_RATE)
    p.add_argument("--pvs", default=DEFAULT_PVS,
                   help="PulseView session file supplying the channel map")
    p.add_argument("--bits", type=int, default=None,
                   help="I2S slot width; detected from LRC/BCLK spacing if omitted")
    p.add_argument("--bit-delay", type=int, default=None, choices=[0, 1],
                   help="BCLK delay before the first data bit; detected if omitted")
    p.add_argument("--maxlag-ms", type=float, default=None,
                   help="frame-offset search half-range; default is 40%% of the capture "
                        "window, which is the most that leaves usable overlap")
    p.add_argument("--driver", default="fx2lafw")
    p.add_argument("--conn", default=None)
    p.add_argument("--sigrok-cli", default="sigrok-cli")
    p.add_argument("--timeout", type=float, default=120.0)
    p.add_argument("--out", default="i2s-skew.csv")
    p.add_argument("--annotate", nargs="+", metavar="LOG", default=None,
                   help="device logs (e.g. a.log b.log) to mark on the plot as vertical "
                        "bars: frame corrections, hard resyncs, and large trim steps")
    p.add_argument("--sync-us", type=float, default=200.0,
                   help="--annotate: mark a Sync report whose |median| reaches this")
    p.add_argument("--peak-us", type=float, default=600.0,
                   help="--annotate: mark a Sync report whose peak error reaches this")
    p.add_argument("--pipeline-ms", type=float, default=25.0,
                   help="--annotate: mark a step in pipeline depth of at least this")
    p.add_argument("--log-tail-mb", type=float, default=4.0,
                   help="--annotate: how much of each log's tail to read on the first "
                        "poll. Reading whole multi-MB logs inside the capture loop "
                        "stalled it for 30 s; the tail is all that baselines need")
    p.add_argument("--trim-ppm", type=float, default=100.0,
                   help="--annotate: minimum trim step to mark (it moves every report)")
    p.add_argument("--replot", action="store_true",
                   help="redraw the plot from an existing --out CSV without capturing; "
                        "use with --annotate to overlay logs on a run already taken")
    p.add_argument("--stability", action="store_true",
                   help="characterise how in-sync the boards STAY: MTIE and TDEV per "
                        "interval, with window counts so the confidence is explicit")
    p.add_argument("--duration", type=float, default=60.0,
                   help="--stability: seconds of capture to analyse in total")
    p.add_argument("--max-jump-frames", type=int, default=50,
                   help="frame-lag change between captures treated as implausible for a "
                        "clock difference (50 frames ~ 1.1 ms at 44.1 kHz). Held, then "
                        "accepted if the next capture repeats it")
    p.add_argument("--rolling", action="store_true",
                   help="re-estimate the frame alignment in a sliding window inside each "
                        "capture, to catch a resync that happens mid-capture")
    p.add_argument("--rolling-span", type=int, default=8,
                   help="--rolling: frames either side of the capture lag to search")
    p.add_argument("--stream", action="store_true",
                   help="read contiguous blocks from one running acquisition instead of "
                        "restarting sigrok per capture: ~99%% duty and no gap between "
                        "blocks, so coverage is continuous")
    p.add_argument("--stream-seconds", type=float, default=60.0,
                   help="--stream: seconds per sigrok acquisition before restarting")
    p.add_argument("--no-prefetch", dest="prefetch", action="store_false",
                   help="do not overlap the next capture with this one's decode")
    p.add_argument("--max-frame-correct", type=int, default=0,
                   help="cap on the whole-frame ambiguity undone per row (0 = no cap). "
                        "The ambiguity is not small -- it was measured wandering over "
                        "+-10 frames -- so a cap mainly re-breaks the series")
    p.add_argument("--plot-window", type=float, default=0.0,
                   help="plot only the last N seconds (0 = the whole run). A live chart "
                        "wants a bounded window: the cost of a write grows with the "
                        "points in it, so an unbounded run slows the refresh over time")
    p.add_argument("--plot-every", type=float, default=2.0,
                   help="seconds between plot rewrites; the CSV is always written per "
                        "capture. Redrawing every capture makes a long run quadratic")
    p.add_argument("--dump-skew", default=None,
                   help="write the PER-FRAME skew series (one row per audio frame, "
                        "~44100/s) to this CSV, not just one row per capture")
    p.add_argument("--y-free", action="store_true",
                   help="scale the vertical axis to the data instead of always including "
                        "zero; use when the shape of the variation matters more than its "
                        "distance from zero")
    p.add_argument("--append", action="store_true",
                   help="continue a previous run's CSV instead of starting fresh. Only "
                        "meaningful if the capture really was uninterrupted")
    p.add_argument("--plot", default="i2s-skew.svg")
    p.add_argument("--wav-prefix", default=None,
                   help="dump each capture's decoded audio to WAV for listening")
    p.add_argument("--simulate", action="store_true",
                   help="no hardware: synthetic I2S with a known offset and ppm")
    p.add_argument("--sim-offset-ns", type=float, default=-12_000.0)
    p.add_argument("--sim-ppm", type=float, default=-5.0)
    p.add_argument("--sim-frame-rate", type=float, default=44117.647)
    args = p.parse_args()

    if args.simulate:
        capture, chan, sim = make_simulator(args)
        if args.count == 0:
            args.count = 5
    else:
        chan = parse_pvs(args.pvs)
        missing = [k for k in ("DIN_ONE", "BCLK_ONE", "LRC_ONE",
                               "DIN_TWO", "BCLK_TWO", "LRC_TWO") if k not in chan]
        if missing:
            sys.exit(f"{args.pvs} does not name these channels: {', '.join(missing)}")
        print("channel map: " + "  ".join(
            f"{k}=D{chan[k]}" for k in ("DIN_ONE", "BCLK_ONE", "LRC_ONE",
                                        "DIN_TWO", "BCLK_TWO", "LRC_TWO")))
        if args.stream:
            capture, stream_state = stream_reader(args)
            sim = None
            args.prefetch = False          # the stream is already gapless
            print(f"  streaming: {args.samples/args_rate_hz(args)*1e3:.0f} ms blocks read "
                  f"from one acquisition, restarted every {args.stream_seconds:g}s")
        else:
            capture, sim, stream_state = (lambda: capture_logic(args)), None, None

    # A run is one continuous acquisition, so the CSV starts fresh by default. Carrying
    # rows over from a previous invocation fabricates continuity: the elapsed axis
    # restarts at zero, the gap between runs leaves no trace, and the drift fit is drawn
    # straight across a discontinuity the boards may well have resynced during.
    def collect_events(t_start, offsets=None):
        """Log events as (elapsed_seconds, kind, label), relative to the run start."""
        out = []
        if not args.annotate:
            return out, offsets
        offsets = offsets or {}
        for path in args.annotate:
            board = os.path.basename(path).split(".")[0]
            span = [None, None]
            ev, end, st, tp = parse_sync_events(path, board, args.trim_ppm,
                                                offsets.get(path, 0), span,
                                                LOG_STATE.get(path), args.sync_us,
                                                args.peak_us, args.pipeline_ms,
                                                args.log_tail_mb * (1 << 20))
            offsets[path], LOG_STATE[path] = end, st
            for tod, name, val in tp:
                TEMPS.setdefault(name, []).append(
                    (tod_to_unix(tod, t_start) - t_start, val))
            if span[0] is not None:
                LOG_COVERAGE[path] = (tod_to_unix(span[0], t_start) - t_start,
                                      tod_to_unix(span[1], t_start) - t_start)
            for tod, kind, label in ev:
                out.append((tod_to_unix(tod, t_start) - t_start, kind, label))
        return out, offsets

    if args.replot:
        ts0, ys0, anchor0 = load_existing(args.out)
        if not ts0:
            sys.exit(f"{args.out} has no rows to plot")
        ev, _ = collect_events(anchor0 or time.time())
        ev = [e for e in ev if -1 <= e[0] <= ts0[-1] + 1]
        print(f"replot: {len(ts0)} rows spanning {ts0[-1]-ts0[0]:.1f} s, "
              f"{len(ev)} log event(s) in window")
        for path, (lo, hi) in sorted(LOG_COVERAGE.items()):
            miss = ""
            if hi < ts0[-1] - 5 or lo > ts0[0] + 5:
                miss = ("  <-- does not span the run; the unmarked stretch is "
                        "un-logged, not event-free")
            print(f"   {path}: log covers t={lo:+.1f}..{hi:+.1f} s{miss}")
        for x, kind, label in sorted(ev)[:40]:
            print(f"   t={x:8.2f}s  {kind:9s} {label}")
        t2 = trim_temps(TEMPS, ts0[0] - 1, ts0[-1] + 1)
        if args.annotate and not t2:
            print(f"   no temperature found in {', '.join(args.annotate)} within the "
                  f"run window -- looked for 'Sending state N C', 'NAMETEMP N C', "
                  f"and 'temp=N'")
        if t2:
            print(f"   temperature series: " + ", ".join(
                f"{k} ({len(v)} pts, {min(v_ for _, v_ in v):.1f}-"
                f"{max(v_ for _, v_ in v):.1f} C)" for k, v in sorted(t2.items())))
        write_svg(args.plot, ts0, ys0, "I2S playout skew",
                  "board B - board A (ns)   [+ = B later]",
                  stats=stats_caption(ts0, ys0),
                  include_zero=not args.y_free, events=ev, panel2=t2)
        print(f"  plot {args.plot}")
        return

    if args.stability:
        run_stability(args, chan, capture)
        return

    ts, ys, anchor = [], [], None
    if args.append:
        ts, ys, anchor = load_existing(args.out)
        if ts:
            gap = time.time() - (anchor + ts[-1]) if anchor else float("nan")
            print(f"appending to {len(ts)} existing rows in {args.out}; continuing that "
                  f"run's clock after a {gap:.0f} s gap, which the plot will show as a gap")
        new = not os.path.exists(args.out)
        log = open(args.out, "a", buffering=1)
    else:
        prev = load_existing(args.out)[0] if os.path.exists(args.out) else []
        if prev:
            print(f"replacing {args.out} ({len(prev)} rows from a previous run)")
        new = True
        log = open(args.out, "w", buffering=1)
    if new:
        log.write("# i2s-skew.py: board B minus board A, positive means B later\n")
        log.write(SCHEMA + "\n")

    t_start = anchor if (args.append and anchor is not None) else time.time()
    ppms, n, shown_cfg, prefer, pending, last_off = [], 0, False, None, None, None
    events, log_off, last_plot = [], None, 0.0
    # A window of recent values, long enough to out-vote a run of bad blocks but short
    # enough to follow a real move.
    ncorr = 0
    dumpf = None
    if args.dump_skew:
        dumpf = open(args.dump_skew, "w", buffering=1 << 20)
        dumpf.write("# per-frame skew; elapsed_s is the capture start plus frame index\n")
        dumpf.write("elapsed_s,skew_ns\n")
    print(f"{'elapsed':>9} {'offset':>13} {'ppm':>9} {'coef':>7} {'flag':>6} "
          f"{'rival':>6} {'scatter':>9}  note")
    # Acquisition is the floor -- a 1 s window costs 1 s -- so the ~0.2 s of decode
    # afterwards is pure blind time. Kick off the next capture before processing this
    # one and it overlaps instead, taking the duty cycle from ~79% to ~95%. Only ever
    # one sigrok-cli at a time: the next is submitted after the previous has returned.
    pool = pending = None
    if args.prefetch and not args.simulate:
        pool = concurrent.futures.ThreadPoolExecutor(max_workers=1)
    try:
        while args.count == 0 or n < args.count:
            wall = time.time()
            elapsed = (n * args.interval) if args.simulate else (wall - t_start)
            try:
                if pool is None:
                    buf = capture()
                else:
                    buf = pending.result() if pending is not None else capture()
                    more = (args.count == 0 or n + 1 < args.count)
                    pending = pool.submit(capture) if more else None
                off, ppm, coef, info = measure_capture(buf, chan, args, prefer,
                                                      dumpf, elapsed)
            except RuntimeError as e:
                print(f"{elapsed:9.1f}   capture failed: {e}", file=sys.stderr)
                pending = None
                n += 1
                if args.count and n >= args.count:
                    break
                if not args.simulate:
                    time.sleep(args.interval)
                continue

            if not shown_cfg:
                print(f"  decoded {info['bits'][0]}/{info['bits'][1]} bit slots, "
                      f"delay {info['delay'][0]}/{info['delay'][1]}, "
                      f"frame rate {info['fs'][0]:.3f}/{info['fs'][1]:.3f} Hz, "
                      f"BCLK {args_rate_hz(args)/info['bclk_A']/1e6:.4f}/"
                      f"{args_rate_hz(args)/info['bclk_B']/1e6:.4f} MHz")
                shown_cfg = True
            worst = max(info.get("gap_A", 0) or 0, info.get("gap_B", 0) or 0)
            if worst > 3:
                print(f"  WARNING: a BCLK interval is {worst:.1f}x the median -- the "
                      f"analyser probably dropped samples ({args.samplerate} across 8 "
                      f"channels is at the USB 2.0 limit). Retry at a lower "
                      f"--samplerate; timing from this capture is not trustworthy.")

            # Unwrap the whole-frame component. The boards' real skew moves ~2 us
            # between captures against a 22.7 us frame, so a step of more than half a
            # frame is the correlation having chosen a neighbouring lag, not the audio
            # having moved. Unwrapping recovered a smooth series from a measured one
            # whose step sd was 13.5 us: it fell to 2.3 us.
            fperiod = 1e9 / info["fs"][0] if np.isfinite(info.get("fs", (0,))[0]) \
                and info["fs"][0] > 0 else 0.0
            # Correct ONLY the frame-flip signature, and never against a running total.
            # The pairing is already absolute, so a plain continuity unwrap (snap every
            # step to within half a frame of the previous ANSWER) compounds: one wrong
            # frame is carried forever, which walked a series that belongs within +-30 us
            # of zero out to -250 us. Two guards: the correction is relative to this
            # capture's own raw value, and it applies only when the step is within 30% of
            # a whole frame -- a real 15 us movement is left alone, a 22.0 us jump is not.
            # Reference is the MEDIAN of recent accepted values, not the previous one.
            # Measured on a 60 Hz run: the lag toggled +3/-3/+3/-3 frames between blocks,
            # which a previous-value reference cannot fix (each step looks like a real
            # move away, then back) and a +-2 clamp could not reach anyway. Tonal content
            # leaves many lags near-tied -- rival ran at 0.915 -- so an excursion of a few
            # frames is expected and is exactly what this undoes. A sustained shift moves
            # the median with it, so a genuine resync is still tracked.
            # Reconstruct the whole-frame count from continuity, not from the
            # correlation. Measured on a 60 Hz run: the correlation's integer lag wandered
            # over +-10 frames at coef 0.99 -- tonal content leaves many lags near-tied
            # (rival 0.915) -- while 391 of 393 observed jumps sat within 0.03 of a whole
            # frame, i.e. pure ambiguity rather than motion. Meanwhile the TRUE skew moves
            # 0.16 ns per block at 60 Hz and 10 ppm, so the nearest whole frame to the
            # previous value is the right one by an enormous margin. Replaying that run:
            # bad steps 393 -> 2.
            #
            # No clamp, because the ambiguity is not small; but the guard stays. A real
            # move of more than 0.3 frame between blocks (>40 ppm at 60 Hz) fails the
            # guard and is left alone, so a genuine fast slew is not quietly folded away.
            nwrap = 0
            if math.isfinite(off) and fperiod > 0 and last_off is not None:
                cand = round((last_off - off) / fperiod)
                if args.max_frame_correct:
                    cand = int(np.clip(cand, -args.max_frame_correct,
                                       args.max_frame_correct))
                if cand and abs(off + cand * fperiod - last_off) < 0.3 * fperiod:
                    nwrap = int(cand)
                    off += nwrap * fperiod
                    ncorr += 1
            if math.isfinite(off):
                last_off = off
            reason = ("" if math.isfinite(off)
                      else info.get("hint", f"no frame match (coef {coef:.2f})"))
            if math.isfinite(off) and info.get("overlap", 1.0) < 0.75:
                reason = ""  # still a valid measurement, just note the reduced overlap
            k = info.get("frame_lag", 0)
            if math.isfinite(off):
                if prefer is not None and abs(k - prefer) > args.max_jump_frames:
                    if pending is not None and abs(k - pending) <= 4:
                        prefer, pending = k, None      # confirmed twice: a real resync
                    else:
                        pending = k
                        reason = (f"frame lag jumped {k - prefer:+d} frames -- ambiguous "
                                  f"match or resync; held until the next capture agrees")
                        off, ppm = float("nan"), float("nan")
                else:
                    prefer, pending = k, None
            if prefer is None and info.get("rival", 0) > RIVAL_MARGIN:
                reason = reason or f"ambiguous frame match (rival {info['rival']:.2f})"
            ts.append(elapsed)
            ys.append(off if math.isfinite(off) else float("nan"))
            if math.isfinite(ppm):
                ppms.append(ppm)
            log.write(f"{elapsed:.3f},{wall:.3f},{off:.1f},{ppm:.4f},{coef:.4f},"
                      f"{info.get('frame_lag',0)},{info.get('rival',float('nan')):.3f},"
                      f"{info.get('scatter_ns',float('nan')):.1f},{reason}\n")

            note = reason
            if info.get("lag_steps"):
                note = (note + "  " if note else "") + \
                    f"frame lag moved {info['lag_steps']} WITHIN the capture"
            if math.isfinite(off) and info.get("overlap", 1.0) < 0.75 and not nwrap:
                note = (f"offset is {abs(info['frame_lag'])/info['fs'][0]*1e3:.0f} ms of a "
                        f"{info['window_ms']:.0f} ms window -- only "
                        f"{info['overlap']*100:.0f}% overlap; a longer --samples would "
                        f"raise the coefficient")
            if nwrap and not reason:
                note = f"unwrapped {nwrap:+d} frame(s)"
            if args.simulate and math.isfinite(off):
                note = (f"true {sim['true_ns']/1000:+.4f} us, "
                        f"err {off - sim['true_ns']:+.1f} ns, ppm err "
                        f"{ppm - args.sim_ppm:+.4f}")
            o_s = f"{off/1000:+10.4f} us" if math.isfinite(off) else f"{'--':>13}"
            p_s = f"{ppm:+9.4f}" if math.isfinite(ppm) else f"{'--':>9}"
            s_s = f"{info.get('scatter_ns', float('nan')):8.1f}n" \
                if math.isfinite(info.get("scatter_ns", float("nan"))) else f"{'--':>9}"
            print(f"{elapsed:9.1f} {o_s} {p_s} {coef:7.4f} "
                  f"{info.get('frame_lag',0):6d} {info.get('rival',float('nan')):6.2f} "
                  f"{s_s}  {note}")

            if args.wav_prefix and not args.simulate:
                a_ = decode_i2s(bit(buf, chan["BCLK_ONE"]), bit(buf, chan["LRC_ONE"]),
                                bit(buf, chan["DIN_ONE"]), args.bits, args.bit_delay)
                b_ = decode_i2s(bit(buf, chan["BCLK_TWO"]), bit(buf, chan["LRC_TWO"]),
                                bit(buf, chan["DIN_TWO"]), args.bits, args.bit_delay)
                write_wavs(f"{args.wav_prefix}-{n:03d}", a_[0], b_[0], info["fs"][0])

            first_poll = log_off is None
            now = time.time()
            due = (now - last_plot >= args.plot_every) or (n + 1 >= args.count > 0)
            new_ev, log_off = collect_events(t_start, log_off)
            if first_poll:
                # The first poll reads the whole file to establish trim/resync
                # baselines; only events at or after the run start are worth drawing.
                new_ev = [e for e in new_ev if e[0] >= -1.0]
                for lp, (lo, hi) in sorted(LOG_COVERAGE.items()):
                    print(f"  annotating from {lp} (log reaches t={hi:+.1f}s; "
                          f"keep it running for the whole capture)")
            for e in new_ev:
                print(f"  event t={e[0]:7.1f}s  {e[1]:9s} {e[2]}")
            events.extend(new_ev)
            if due:
                last_plot = now
                pt, py_ = ts, ys
                if args.plot_window > 0 and ts:
                    cut = ts[-1] - args.plot_window
                    i0 = bisect.bisect_left(ts, cut)
                    pt, py_ = ts[i0:], ys[i0:]
                write_svg(args.plot, pt, py_, "I2S playout skew",
                          "board B - board A (ns)   [+ = B later]",
                          stats=stats_caption(pt, py_),
                          include_zero=not args.y_free, events=events,
                          panel2=trim_temps(TEMPS, pt[0] - 1, pt[-1] + 1))
            n += 1
            if (args.count == 0 or n < args.count) and args.interval > 0 \
                    and not args.simulate:
                time.sleep(args.interval)
    except KeyboardInterrupt:
        print("\ninterrupted")
    finally:
        if pending is not None:
            pending.cancel()
        if pool is not None:
            pool.shutdown(wait=False)

    if ts:      # always leave a current plot behind, whatever the throttle did
        write_svg(args.plot, ts, ys, "I2S playout skew",
                  "board B - board A (ns)   [+ = B later]",
                  stats=stats_caption(ts, ys),
                  include_zero=not args.y_free, events=events,
                  panel2=trim_temps(TEMPS, -1, ts[-1] + 1))
    if ncorr:
        print(f"\n  {ncorr} of {len(ys)} rows had a whole-frame ambiguity "
              f"corrected by continuity with the previous row")
    if stream_state and stream_state.get("dropped"):
        print(f"\n  WARNING: {stream_state['dropped']} stream block(s) dropped -- "
              f"processing fell behind the acquisition, so coverage has gaps")
    good = [v for v in ys if math.isfinite(v)]
    print(f"\n{len(good)}/{len(ys)} usable")
    if good:
        print(f"  offset mean {np.mean(good):+,.1f} ns  sd {np.std(good):,.1f} ns  "
              f"range {max(good)-min(good):,.1f} ns")
    if ppms:
        print(f"  rate difference (per capture) mean {np.mean(ppms):+.4f} ppm  "
              f"sd {np.std(ppms):.4f} ppm")
    f = fit_slope(ts, ys)
    if f:
        print(f"  least-squares slope {f[0]:+,.1f} ns/s ({f[0]/1000:+.4f} ppm) over "
              f"{ts[-1]-ts[0]:.0f} s  (reported only -- not drawn)")
    print(f"  data {args.out}   plot {args.plot}")
    if dumpf:
        dumpf.close()
        print(f"  per-frame skew {args.dump_skew}")
    log.close()


if __name__ == "__main__":
    main()
