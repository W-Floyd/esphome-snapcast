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

OFFSET INTEGRAL. The offset between two boards is the integral of their differential
achieved rate and nothing else -- integrating the fs_a_hz/fs_b_hz columns reproduces the
measured offset at corr -0.997..-1.000, slope -1.0, residual sd 0.14-0.38 us over runs of
92-499 s, so there is no second term and no static offset to explain. That check runs on
every replot, so the result stays testable rather than remembered.

Given --annotate logs from firmware that emits the "Trim window:" line, the same check
runs a second time against the trim the BOARDS reported, which asks whether a device can
see its own relative offset without an analyser -- the thing it would need in order to
correct one. The first two --annotate logs are taken as board A and board B in that
order; a swap shows up as slope +1 instead of -1.

    python3 scripts/i2s-skew.py --replot --annotate a.log b.log
"""

import argparse
import collections
import bisect
import concurrent.futures
import itertools
import json
import math
import queue
import threading
import os
import re
import subprocess
import sys
import tempfile
import time
import traceback
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import urllib.parse

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
# How far the best correlation peak must beat the runner-up before the row is a measurement at
# all. See the gate below for the measurement this is drawn from; it is a MARGIN rather than an
# absolute coefficient because the spurious matches carry coef 0.984 against the true match's
# 0.985 -- indistinguishable on coefficient, obvious on margin.
MIN_RIVAL_MARGIN = 0.05
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
    starved = 0
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
        nblk = 0
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
                nblk += 1
                yield np.frombuffer(b"".join(chunks), dtype=np.uint8)
        finally:
            if proc.poll() is None:
                proc.kill()
            err = (errbuf[0].decode(errors="replace").strip() if errbuf else "")
        if err and ("LIBUSB" in err or "Failed to open" in err):
            raise RuntimeError("cannot claim the analyser -- is PulseView open?\n  " + err)
        # AN ACQUISITION THAT ENDS EARLY MUST BE SAID OUT LOUD. Measured 2026-08-28: at
        # samplerate=24M this fx2 delivers a couple of MB and then simply stops sending,
        # and sigrok sits out the whole --time before reporting "Device only sent N
        # samples". Restarting that silently is a hang with no output: the loop consumed
        # its three blocks, then waited minutes per restart for a stream that never came
        # back. 12M and 8M sustain the full duration on the same rig, so this is a rate
        # ceiling, not a dead device -- which is worth saying rather than making the
        # operator bisect it.
        expect = int(args.stream_seconds * args_rate_hz(args) / block)
        if nblk < max(1, expect // 2):
            starved += 1
            print(f"  WARNING: acquisition ended after {nblk} of ~{expect} blocks"
                  + (f" -- {err}" if err else ""), file=sys.stderr)
            if starved >= 3:
                raise RuntimeError(
                    f"the analyser will not sustain samplerate={args.samplerate}: three "
                    f"acquisitions in a row stopped early. Try --samplerate 12M "
                    f"(halve --samples to keep the same block duration).")
        else:
            starved = 0


def stream_reader(args, depth=16):
    """stream_blocks in a thread, buffered, so slow processing cannot stall sigrok.

    Anything that takes longer than a block -- the first log parse reads tens of MB, and
    was measured stalling the loop for 30 s -- otherwise stops the pipe being read, the
    acquisition backs up behind it, and coverage is silently lost. Here the reader keeps
    consuming; if the queue is full the block is DROPPED and counted, so falling behind
    shows up as a reported gap instead of a stall.
    """
    q = queue.Queue(maxsize=depth)
    state = {"dropped": 0, "error": None, "done": False, "restarts": 0}

    # THE ACQUISITION MUST BE REBUILDABLE, or one transient ends the run.
    #
    # Every board reboot -- an OTA, in practice -- stops both I2S buses for a few seconds. sigrok
    # then delivers a short acquisition, stream_blocks counts three of those in a row and raises
    # "will not sustain samplerate", and before this change that killed the reader THREAD: the
    # generator was gone, state["error"] was latched, and take() raised the same message for ever
    # while the boards played on happily. Twice tonight (2026-09-02, both after an OTA) the
    # analyser sat at "no BCLK edges" until it was restarted by hand, with the CSV frozen and
    # nothing but the operator to notice.
    #
    # So a failed acquisition is retried with backoff, and only a run of them is fatal. The error
    # is latched only after MAX_RESTARTS, which keeps the CaptureFailures contract intact: the
    # process still exits rather than looping for ever, just not on the first transient.
    MAX_RESTARTS = 6

    def run():
        while True:
            try:
                for blk in stream_blocks(args):
                    if state["restarts"]:
                        # A block got through: the acquisition is healthy again.
                        print(f"  acquisition recovered after {state['restarts']} restart(s)",
                              file=sys.stderr, flush=True)
                        state["restarts"] = 0
                    try:
                        q.put_nowait(blk)
                    except queue.Full:
                        state["dropped"] += 1
                # Generator returned without raising: stream_blocks loops for ever, so this is
                # not expected -- treat it as a restartable end rather than a silent stop.
                raise RuntimeError("acquisition generator ended")
            except Exception as e:
                state["restarts"] += 1
                if state["restarts"] > MAX_RESTARTS:
                    state["error"] = e              # surfaced on the consumer side
                    break
                print(f"  acquisition failed ({e}); restart "
                      f"{state['restarts']}/{MAX_RESTARTS} in {2 * state['restarts']}s",
                      file=sys.stderr, flush=True)
                time.sleep(2 * state["restarts"])
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
                # Prefer the latched cause: "stream ended" says nothing, and the reason the
                # acquisition gave up is exactly what the operator needs to read.
                raise RuntimeError(str(state["error"]) if state["error"] else "stream ended")
            return blk

    return take, state


class CaptureFailures:
    """Bound what a capture that keeps failing is allowed to cost.

    The dangerous failure is not one bad capture, it is a STUCK one. With --stream the loop
    runs at --interval 0, so a device this process cannot claim -- PulseView holding the USB
    interface, LIBUSB_ERROR_ACCESS -- produced an unthrottled print loop rather than an exit.
    Redirected to a file (2026-08-28) that wrote 143 GB and filled the disk, which killed both
    live analyser instances mid-window and froze test.csv. Nothing warned: the boards kept
    logging normally and the CSV simply stopped.

    So a repeated failure is made cheap in both directions:

      * OUTPUT is bounded. An identical message is printed on a doubling schedule (1st, 2nd,
        4th, 8th ...) carrying its own tally, so a wedge costs a few dozen lines however long
        it lasts, and a genuinely varying failure still shows every distinct message.
      * TIME is bounded. Retries back off to MAX_BACKOFF regardless of --interval, so the
        loop cannot spin, and after HARD_LIMIT consecutive failures the process EXITS
        nonzero. A wedged analyser that exits is visible -- a dead tmux pane, a non-zero
        status -- where one that retries for ever looks exactly like a quiet bench.

    The tally is the point of the summary line: "gave up after N consecutive failures" says
    the run ended, which "capture failed" repeated for six hours does not.
    """

    HARD_LIMIT = 40          # consecutive failures before giving up entirely
    MAX_BACKOFF = 30.0       # seconds; the floor on retry spacing, ignoring --interval

    def __init__(self, limit=None):
        self.limit = limit or self.HARD_LIMIT
        self.reset()

    def reset(self):
        self.n = 0
        self.last = None
        self.first_at = None

    ok = reset          # a successful capture clears the run

    def failed(self, msg, elapsed):
        """Report one failure. Returns the seconds to wait; exits if it has gone on too long."""
        now = time.time()
        if msg != self.last:
            self.last, self.n, self.first_at = msg, 0, now
        self.n += 1
        if self.first_at is None:
            self.first_at = now
        # Doubling schedule: 1, 2, 4, 8, ... Bounded output for an unbounded wedge.
        if self.n & (self.n - 1) == 0:
            tail = f" [{self.n} consecutive, {now - self.first_at:.0f}s]" if self.n > 1 else ""
            print(f"{elapsed:9.1f}   capture failed: {msg}{tail}", file=sys.stderr, flush=True)
        if self.n >= self.limit:
            print(f"\nGIVING UP: {self.n} consecutive capture failures over "
                  f"{now - self.first_at:.0f}s, last was: {msg}\n"
                  f"  Nothing has been measured since. Fix the analyser and restart; the "
                  f"CSV written so far is intact.", file=sys.stderr, flush=True)
            sys.exit(3)
        return min(2.0 ** min(self.n - 1, 16), self.MAX_BACKOFF)


FAILURES = CaptureFailures()


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
# The width the .svg on disk is always laid out at. The live view overrides it per client;
# the file does not follow the browser, or every capture would rewrite it a different shape.
PLOT_WIDTH = 900

# Panel series colours, by position in the panel's SORTED key order. Module-level because
# the legend has to name the same colour the drawing will use, and the two are computed in
# different places -- a second literal list would drift and the swatch would then be a
# confident lie about which trace a checkbox controls.
PANEL_PALETTE = ["#c2410c", "#0f766e", "#7c3aed", "#a16207", "#be123c"]

# TSF group state, e.g. "tsf=consensus(n2, 1.0s, depth +2267 render +12 us)" / "tsf=solo(...)".
# Matched on the first letter only: the Sync line is long and the logger truncates it mid-token,
# so "tsf=consen", "tsf=cons" and "tsf=so" all occur and must not be missed.
#
# l/f are the RETIRED leader/follower states, kept because a.log and b.log span days and still
# carry them; the firmware is leaderless as of 2026-08-28 and emits only c/s/i.
STATE_NAMES = {"c": "consensus", "s": "solo", "i": "inactive", "l": "leader", "f": "follower"}
ROLE_RE = re.compile(r"^\[(\d\d):(\d\d):(\d\d)\.(\d+)\].*?\btsf=([lfcsi])")


LOG_COVERAGE = {}
LOG_STATE = {}
TEMPS = {}
# board -> [(elapsed_s, mean_ppm | None, audio_s)] from the firmware's "Trim window:" line.
# None marks a window with no trim programmed at all, which is a hole in the integral and
# not a zero; see integral_fit.
TRIMS = {}
# [(elapsed_s, dl_err_a - dl_err_b in us)], built once where the CSV row is written so the
# overlay and the column are literally the same numbers. A second derivation would be a
# second thing to keep in step, and this plot exists to compare belief with measurement --
# the comparison must not itself be two different computations.
DL_DIFF = []

# (board, key) -> [(elapsed_s, value)] for what the firmware BELIEVES, so it can be shown
# against what the wire measures. Sampled at the firmware's own cadence (render phase every
# ~24 s over these logs), not at the capture rate.
FIRMWARE = {}
# board -> (offset_s, jitter_ms, n) from device_anchor: how this board's esp_timer clock maps
# onto the host axis, and how much host-side delay had to be averaged out to get there.
DEV_ANCHOR = {}
# board -> [(elapsed_s, n, err_us, med_us, ring_ms, drops)] from the armed per-chunk burst.
RSYNCS = {}
# board -> [(elapsed_s, latency_us, age_us, frames)] from each re-baseline anchor.
SEEDS = {}
# board -> [(elapsed_s, repaired_us)] from each accounting-split repair.
REPAIRS = {}
# board -> [(elapsed_s, injected_us)] from each deliberate accounting-split injection.
INJECTS = {}

# `trim` is OPTIONAL and must stay that way. It used to be required, and the firmware's Sync line
# hit the 256-byte formatting ceiling on 140 of 144 reports -- so the trim field was cut off, the
# regex failed, and the WHOLE line was discarded along with its frame corrections, hard resyncs and
# pipeline steps. A parser that drops a record because one trailing field is missing turns a
# formatting limit into silent data loss, which is the worst kind: the plot simply showed fewer
# events and looked fine. Firmware now splits the report across `Sync:` and `SYNCX`, but old logs
# still have the truncated form and must remain readable.
SYNC_RE = re.compile(
    r"^\[(\d\d):(\d\d):(\d\d)\.(\d+)\].*?"
    r"corrected -(\d+)/\+(\d+) frames,\s*(\d+) hard resyncs"
    r"(?:.*?trim ([+-][\d.]+) ppm)?")

# DEVICE TIMESTAMP, esp_timer microseconds since boot, appended to every series line the
# firmware emits for plotting. Preferred over the "[HH:MM:SS]" prefix for placing points,
# because the prefix is the HOST's receive time and so is exposed to log-delivery delay in a
# way a device clock is not.
#
# How much delay, measured on hardware once this field made the comparison possible: p50 0 ms,
# p90 7.7 ms, p99 28 ms over 139 lines -- under 3% of a 1 s interval. An earlier claim of
# "200 ms typical, up to 1 s" was WRONG; it came from one truncated and interleaved line, n=1.
# So this field is cheap insurance and a continuous self-check, not a fix for a measured fault,
# and device_anchor prints the residual so a future regression in the host path is visible
# rather than assumed. Optional: a log from older firmware still plots off the prefix.
DEV_T_RE = re.compile(r"\bt=(\d+)")

# "Rate ref: tsf-local +42.237 ppm (raw +42.100) t=..." -- each board's local clock against
# the RADIO timebase, so it is measured outside the audio servo loop. Its DIFFERENCE between
# two boards is their crystal difference: measured at -5.347 ppm, which accounts for the whole
# of the differential trim's constant offset from the true rate and takes the integrated error
# from 505 to 17 us per 100 s. Updates every RATE_WINDOW_US (4 s) on the device, so there is
# nothing to gain by looking for it faster.
# "Crystal: mine +42.169 leader +37.001 delta +5.168 ppm t=..." -- the DEVICE's own answer to
# the question this script needed an analyser for. Both sides measure themselves against the
# same AP TSF, so the AP's crystal cancels and the delta is the two devices' crystal difference,
# measured outside the audio servo loop. That difference is the whole of the differential trim's
# constant offset from the true achieved rate (-5.25..-5.40 ppm measured on the wire), so this
# line is what lets a speaker compute the correction with no analyser present. Needs at least one
# peer to difference against, so a device alone in its group emits nothing.
#
# "group" is the current wording and "leader" the retired one; both are accepted because a.log and
# b.log span days across the leaderless change.
CRYSTAL_RE = re.compile(
    r"^\[(\d\d):(\d\d):(\d\d)\.(\d+)\].*?Crystal: mine ([+-][\d.]+) (?:group|leader) ([+-][\d.]+) "
    r"delta ([+-][\d.]+) ppm")

# "RSYNC[7] t=... err=52123 med=-1914 ring=1697 drops=3" -- the armed per-chunk burst around
# a resync excursion. The discriminator it exists for: if lateness is real, err falls ~1:1
# with the audio discarded, so err should drop by about one chunk (~26 ms) per drops++. If err
# is flat or rising across drops, the trigger is a bad prediction or a bad deadline and
# discarding cannot close it. Those two want opposite fixes and look identical at report
# resolution, which is how a discard cap got flashed and reverted.
# "SEEDANCHOR latency=217755 age=1234 frames=9602 t=..." -- a re-baseline anchoring the playout
# accounting to this device's OWN measured pipeline latency. Under test: the per-device error in
# that latency becomes a permanent static offset, because the servo then measures against the
# prediction it anchors and reads ~0 while the audio sits that far off. Unobservable on-device by
# construction, so the wire has to arbitrate -- report_seed_steps pairs each seed against the
# offset step at that instant.
# "SEEDDRAIN anchored=220000 actual=237412 err=+17412 frames=9702 t=..." -- the anchor error,
# measured ON-DEVICE and without any prediction in the path. The seed asserts the resident audio
# will take `anchored` us to drain; the playout FEEDBACK (ground truth from the speaker callback,
# not from `pushed`) says it took `actual`. err is the difference, and it is exactly the term that
# is invisible to every other on-device metric: the servo measures against the prediction this
# error corrupts, so it reads ~0 while the audio sits err away from where it belongs.
#
# Pair err against the wire step at the same seed. The hypothesis predicts they match.
# "Accounting split repaired: accounted queue ran -51747 us against measured latency for 3 s"
# -- the self-repair correcting a sustained accounted-vs-measured split. Instrumented because each
# firing means the accounting was off by that much for DRIFT_REPAIR_HOLD_US (3 s) while the servo
# steered real audio against a prediction wrong by the same amount, and then the repair fixes the
# ACCOUNTING, which makes any resulting audio displacement invisible to every on-device metric.
# Measured 23 times across two logs on splits from 4.7 to 57 ms, i.e. far more often than seeds.
# "SPLITINJECT +20000 us (+882 frames) t=..." -- a deliberately injected accounting split, so a
# repair can be provoked in QUIET conditions with a KNOWN magnitude. Paired with the repair that
# follows it, this turns the step-versus-split relationship from one natural data point into a
# curve.
# Matches the ramped form, "SPLITINJECT request +2500 us, ramping at 100 us/s (remaining ...)".
# The `request` word was added when the hook became a ramp and this pattern was not, so every
# provoked repair silently lost its tag and became indistinguishable from a natural one -- which is
# the single thing the tag exists to prevent. Kept tolerant of the word being absent.
SPLITINJECT_RE = re.compile(
    r"^\[(\d\d):(\d\d):(\d\d)\.(\d+)\].*?SPLITINJECT (?:request )?([+-]?\d+) us")

REPAIR_RE = re.compile(
    r"^\[(\d\d):(\d\d):(\d\d)\.(\d+)\].*?Accounting split repaired: accounted queue ran "
    r"([+-]?\d+) us")

SEEDDRAIN_RE = re.compile(
    r"^\[(\d\d):(\d\d):(\d\d)\.(\d+)\].*?SEEDDRAIN anchored=(-?\d+) actual=(-?\d+) err=(-?\d+)")

SEEDANCHOR_RE = re.compile(
    r"^\[(\d\d):(\d\d):(\d\d)\.(\d+)\].*?SEEDANCHOR latency=(\d+) age=(-?\d+) frames=(-?\d+)")

# "RENDERTAG measured=-1035.. inferred=-1035.. tags=335 age=9705 frames=441 off=1908 sup=1" --
# the captured render phase beside the ledger-inferred one. Two things are worth a mark: the
# measured value going `unknown` (the signal REFUSING, which is the healthy response to a stalled
# pipeline and the moment the old signal would have published a wrong number instead), and the two
# disagreeing, which is a ledger bias the inferred form cannot see.
RENDERTAG_RE = re.compile(
    r"^\[(\d\d):(\d\d):(\d\d)\.(\d+)\].*?RENDERTAG measured=(unknown|-?\d+) "
    r"inferred=(unknown|-?\d+) tags=(\d+) ")

# "PHASEIN mine=-1035.. | 4D74 d=+85 age=484ms | 49C8 d=+33 age=81ms | .. | group=-17" -- the
# GROUP CONSENSUS INPUTS, emitted only by a board with tsf_observer set. This is the one line that
# names WHICH peer moved: a single peer publishing a wild phase drags the group statistic, and with
# the pairing window admitting 0-2 peers 94% of the time there is often no other contributor to
# outvote it. Marking the offending peer by name is the whole point.
PHASEIN_RE = re.compile(
    r"^\[(\d\d):(\d\d):(\d\d)\.(\d+)\].*?PHASEIN mine=(unknown|-?\d+)(.*?)\| group=(-?\d+)")
PHASEIN_PEER_RE = re.compile(r"\| ([0-9A-F]{4}) d=([+-]\d+) age=(\d+)ms")

# "TRIMDBG applied=+18.75 ppm samples=128 railed=0 span=+16..+22 splithold=0 gate=0 lock=1 ..."
# The steering attribution, on its own line precisely because it used to live at the END of the
# Sync report and was truncated away on ~99% of reports. Every count taken from that field was
# really a count of "did the line happen to fit".
TRIMDBG_RE = re.compile(
    r"^\[(\d\d):(\d\d):(\d\d)\.(\d+)\].*?TRIMDBG applied=([+-][\d.]+) ppm samples=(\d+) "
    r"railed=(\d+) span=([+-][\d.]+)\.\.([+-][\d.]+) splithold=(\d+) gate=(\d)")
TRIMDBG_OFF_RE = re.compile(
    r"^\[(\d\d):(\d\d):(\d\d)\.(\d+)\].*?TRIMDBG rate_lock_ok=0")

# "SYNCX feedback ... buffered ... pipeline ... , tsf=consensus(n5, 1.2s, depth .. render .. us)"
# The tail of the old Sync line, moved so it survives. `pipeline` markers are read from here now.
SYNCX_RE = re.compile(
    r"^\[(\d\d):(\d\d):(\d\d)\.(\d+)\].*?SYNCX .*?pipeline (-?\d+) ms")
# Consensus size and mapping age, when the tsf= field is present on SYNCX.
SYNCX_TSF_RE = re.compile(r"tsf=(\w+)\(n(\d+), ([\d.]+)s")

RSYNC_RE = re.compile(
    r"^\[(\d\d):(\d\d):(\d\d)\.(\d+)\].*?RSYNC\[(\d+)\] t=(\d+) err=(-?\d+) med=(-?\d+) "
    r"ring=(\d+) drops=(\d+)")

# "RPRE[-80..-75] t=... dt=26100,26099,... err=... med=... ring=... drops=..." -- the PRE-TRIGGER
# history replayed when that burst arms. The burst above is armed BY the threshold crossing, so
# its first line already reads ring=26 and it structurally cannot show the ring EMPTYING, which is
# the question that replaced "did discarding close the error?". These lines are the chunks before
# the arm, six per line, replayed one line per chunk so the replay costs a fraction of the live
# burst's log rate.
#
# Expanded into the SAME per-chunk rows as RSYNC, with NEGATIVE sequence numbers (-80 = 80 chunks
# before the arm, -1 = the chunk immediately before it), so an episode reads as one continuous
# record from -80 to +79 and the burst report needs no special case. The replay is emitted AFTER
# the arm, so rows are sorted by device time before bursts are split.
#
# dt is each chunk's gap from its predecessor: the firmware stores deltas (4 bytes, and the
# arrival cadence is itself part of "why does the ring empty?"), and only the first sample on
# each line carries an absolute t. The rest are reconstructed by accumulating dt.
RPRE_RE = re.compile(
    r"^\[(\d\d):(\d\d):(\d\d)\.(\d+)\].*?RPRE\[-(\d+)\.\.-(\d+)\] t=(\d+) dt=([\d,]+) "
    r"err=([-\d,]+) med=([-\d,]+) ring=([\d,]+) drops=([\d,]+)")

# "Trim window: mean +12.345 ppm over 3.31 s audio (covered 100%)" -- the window's
# TIME-MEAN applied trim, on its own line because the Sync line above is at the 256-byte
# formatting ceiling. This is the quantity the offset integral needs; the end-of-window
# snapshot on the Sync line is one sample per 3.3 s of a continuously moving value, and
# integrating those explained only 13-19% of the measured offset where the analyser's own
# rate columns explained 99-100%.
TRIM_WINDOW_RE = re.compile(
    r"^\[(\d\d):(\d\d):(\d\d)\.(\d+)\].*?Trim window: mean ([+-][\d.]+) ppm "
    r"over ([\d.]+) s audio \(covered (\d+)%\)")
# The no-trim variant of the same line. Parsed rather than ignored precisely because it
# marks real audio time the integral cannot account for.
TRIM_NONE_RE = re.compile(
    r"^\[(\d\d):(\d\d):(\d\d)\.(\d+)\].*?Trim window: no trim programmed over ([\d.]+) s audio")

# The firmware's OWN estimate of the quantity this script measures on the wire, so it can
# be plotted against the measurement rather than trusted. Two independent forms exist and
# they disagree by orders of magnitude over these logs -- render phase sits at a median of
# -41 us while playout-depth reports medians of +12 ms and -6.6 ms -- which is exactly the
# disagreement the wire can arbitrate.
# The sign class must accept '+': the firmware prints this with %+PRId64, so every POSITIVE
# delta carries a literal plus. A (-?\d+) here silently dropped 1112 of 2713 real points --
# 41%, all of one sign -- which does not merely thin the series, it BIASES it: the surviving
# median is a median of negatives only.
PHASE_RE = re.compile(
    r"^\[(\d\d):(\d\d):(\d\d)\.(\d+)\].*?Render phase .*?delta ([+-]?\d+) us")
# "vs group" is the current wording, "vs leader" the retired one; both are accepted, see CRYSTAL_RE.
# RPHASE (firmware 2026-08-31, from the PLAYER task at DEBUG): carries BOTH phase deltas because
# they are different quantities -- dgate is pairing-window gated and is what the servo acts on;
# dplot is the ungated quantity the VERBOSE snap_net "Render phase" line prints, which can
# difference phases sampled seconds apart and report drift as skew. PHASE_RE keeps its historical
# meaning (the ungated one) so `phase_a/b_us` stays comparable across the change; dgate gets NEW
# columns rather than being folded into an existing name (R7.3: un-holding phase_* already changed
# that column's population once, and reusing a name for a different quantity is the same defect).
# Sentinels arrive as the literal `unknown` and must stay unparsed, never coerced to a number.
# dplot is OPTIONAL in the pattern: never require a trailing field (CLAUDE.md). SYNC_RE once
# required `trim ... ppm` and a truncated line failed to match and was dropped WHOLE, taking that
# report's other fields with it. The line is ~119 bytes so truncation is unlikely here -- but
# "unlikely" is what the 256-byte ceiling was assumed to be too, and a cheap optional group means
# a clipped line still yields dgate instead of nothing.
RPHASE_RE = re.compile(
    r"^\[(\d\d):(\d\d):(\d\d)\.(\d+)\].*?RPHASE mine=(\S+) dgate=(\S+)(?:\s+dplot=(\S+))?")
DEPTH_RE = re.compile(
    r"^\[(\d\d):(\d\d):(\d\d)\.(\d+)\].*?Playout depth ([+-]?\d+) us vs (?:group|leader)")
# Comparable to the ppm this script measures per capture from the LRC edges.
RAMP_RE = re.compile(
    r"^\[(\d\d):(\d\d):(\d\d)\.(\d+)\].*?Offset ramp ([+-][\d.]+) ppm")

# "DLLOOP err=-41 us trim=+49.68 int=+53.82 ppm kp=0.100 n=36 dt=0.35 t=438623050"
#
# This is the delay loop's OWN error against its target, and the difference between the two
# boards' errors is the closest thing the firmware has to the quantity this script measures
# on the wire. Correlated at r = 0.88 against the wire with ~2.5 us of bias and 13 us/s of
# noise, where phase_b - phase_a is biased by tens of microseconds, 3-4x noisier, and
# carries stall-stamp spikes. So this, not Render phase, is the device-vs-truth comparison.
#
# Requiring trim= is deliberate and is NOT the trailing-field mistake this project has made
# before: the OUT OF RANGE form below is a DIFFERENT line that genuinely has no trim, and
# the two forms partition the DLLOOP lines exactly (measured: 8716 + 1382 = 10098, no
# remainder). Length checked too -- 139-140 bytes on 842 of 846 sampled lines, far short of
# the 256-byte formatting ceiling, so nothing here is at risk of being cut mid-token.
# int= is OPTIONAL even though every real line has it. The project's own rule: a regex that
# requires a trailing field turns a formatting limit into silent whole-line data loss, and
# SYNC_RE has already made exactly that mistake once. err is the load-bearing field and it
# comes first, so a line cut anywhere after it still yields the measurement.
DLLOOP_RE = re.compile(
    r"^\[(\d\d):(\d\d):(\d\d)\.(\d+)\].*?DLLOOP err=([-+]\d+) us"
    r"(?: trim=([-+\d.]+))?(?: int=([-+\d.]+))?")

# "DLLOOP err=-1532 us OUT OF RANGE (>=1000), holding integral +55.28 ppm t=348479916"
# The error is real but was not acted on, so it is an ANNOTATION rather than a sample: mixing
# it into the series would plot a value the loop explicitly refused to use.
DL_OOR_RE = re.compile(
    r"^\[(\d\d):(\d\d):(\d\d)\.(\d+)\].*?DLLOOP err=([-+]\d+) us OUT OF RANGE "
    r"\(>=(\d+)\)")

# "Delay loop: engaged, integral +55.36 ppm (err -186 us) t=..." and
# "Delay loop: deadline on local fallback, holding integral +55.36 ppm + P -18.19 ... tau t=..."
DL_STATE_RE = re.compile(
    r"^\[(\d\d):(\d\d):(\d\d)\.(\d+)\].*?Delay loop: (.+?)(?:\s+t=\d+)?\s*$")

# "SERVOPARAM tau_s=10.000 t=358621434" -- note this one is logged WITHOUT the [snap_player]
# tag the other lines carry, so nothing here may require it.
SERVOPARAM_RE = re.compile(
    r"^\[(\d\d):(\d\d):(\d\d)\.(\d+)\].*?SERVOPARAM (\S+?)=([-+\d.]+)")

# "SERVOTUNE hold tau=10.0 s (r1=+0.99 mean=-146 sd=107) t=..." -- and a "sluggish-looking
# window ... not acted on" form with no tau at all, which must not be mistaken for a change.
SERVOTUNE_RE = re.compile(
    r"^\[(\d\d):(\d\d):(\d\d)\.(\d+)\].*?SERVOTUNE .*?\btau=([\d.]+)")

DL_INTEGRAL_RE = re.compile(r"integral ([+-][\d.]+)")


def parse_sync_events(path, board, trim_ppm, start_offset=0, span=None, state=None,
                      sync_us=200, peak_us=600, pipe_ms=25, tail_bytes=0,
                      rendertag_us=500, phasein_us=1000, dl_event_s=5.0):
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
    ev, temps, trims = [], [], []
    # name -> [(time_of_day, value)], for series the caller plots and logs rather than
    # turning into bars. Keyed by name so more can be added without changing the arity.
    extras = {}
    # Per-chunk resync bursts: (tod, n, dev_us, err_us, med_us, ring_ms, drops)
    rsyncs = []
    # (tod, dev_us, latency_us, age_us, frames) per re-baseline anchor
    seeds = []
    # (tod, dev_us, repaired_us) per accounting-split repair; dev_us is None (no t= on that line)
    repairs = []
    # (tod, dev_us, injected_us) per deliberate accounting-split injection
    injects = []
    last_trim, last_resync = state.get("trim"), state.get("resync")
    last_pipe = state.get("pipe")
    # Boxed so the SYNCX branch can update it without a nonlocal; carried across incremental
    # reads like the other running state, so a membership change is not re-reported every poll.
    last_consensus = [state.get("consensus")]
    last_role = state.get("role")
    try:
        f = open(path, errors="replace")
    except OSError:
        # Same arity as the normal return: it was short by one, so an unreadable log
        # would have crashed the caller's unpacking rather than being skipped.
        return [], start_offset, state, [], [], {}, [], [], [], []
    # ROTATION / TRUNCATION GUARD: a logger restart that recreates the file leaves the carried
    # offset pointing into unrelated content (or beyond EOF, which reads nothing forever). The
    # size check alone only catches shrinkage (R7.3): a recreated file already refilled past the
    # offset resumes mid-stream with no gap indication -- a WRONG read, not a deaf one. The
    # (st_dev, st_ino) identity is the complete test; both trigger a re-prime from scratch.
    try:
        fst = os.fstat(f.fileno())
        ident = (fst.st_dev, fst.st_ino)
        if state.get("_ident") not in (None, ident) or start_offset > fst.st_size:
            start_offset = 0
        state["_ident"] = ident
    except OSError:
        pass
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
            role = STATE_NAMES.get(rm.group(5), rm.group(5))
            if last_role is not None and role != last_role:
                ev.append((tod_r, "role", f"{board}: {last_role} -> {role}"))
            last_role = role
        # The trim window is its own log line, so it is matched before SYNC_RE's continue
        # rather than alongside the fields on the Sync line.
        tw = TRIM_WINDOW_RE.match(line)
        if tw:
            tod_w = (int(tw.group(1)) * 3600 + int(tw.group(2)) * 60 + int(tw.group(3))
                     + int(tw.group(4)) / (10 ** len(tw.group(4))))
            dv = DEV_T_RE.search(line)
            trims.append((tod_w, float(tw.group(5)), float(tw.group(6)),
                          int(dv.group(1)) if dv else None))
            continue
        tn = TRIM_NONE_RE.match(line)
        if tn:
            tod_w = (int(tn.group(1)) * 3600 + int(tn.group(2)) * 60 + int(tn.group(3))
                     + int(tn.group(4)) / (10 ** len(tn.group(4))))
            dv = DEV_T_RE.search(line)
            trims.append((tod_w, None, float(tn.group(5)),
                          int(dv.group(1)) if dv else None))
            continue
        # --- delay loop ------------------------------------------------------------
        # OUT OF RANGE is tested FIRST because it is a DLLOOP line too: err is present but
        # the loop explicitly refused to act on it, so it becomes an annotation and never a
        # sample. Mixing it into the series would plot a value the firmware discarded.
        oor = DL_OOR_RE.match(line)
        if oor:
            tod_o = (int(oor.group(1)) * 3600 + int(oor.group(2)) * 60 + int(oor.group(3))
                     + int(oor.group(4)) / (10 ** len(oor.group(4))))
            # First line of a run only. These repeat every loop tick for as long as the
            # condition lasts -- 1382 of them in one log against 8716 normal lines -- and
            # marking each would paint the panel solid and hide everything else on it.
            # Same rewind hazard as the delay-loop gate: a re-read must be able to mark
            # the first line of a run it has already seen once.
            if tod_o < state.get("oor_t", tod_o):
                state["oor"] = False
            state["oor_t"] = tod_o
            if not state.get("oor"):
                ev.append((tod_o, "oor",
                           f"{board}: dl out of range {oor.group(5)} us"))
            state["oor"] = True
            continue
        dl = DLLOOP_RE.match(line)
        if dl:
            state["oor"] = False          # a normal tick ends the run
            tod_d = (int(dl.group(1)) * 3600 + int(dl.group(2)) * 60 + int(dl.group(3))
                     + int(dl.group(4)) / (10 ** len(dl.group(4))))
            dv = DEV_T_RE.search(line)
            dev = int(dv.group(1)) if dv else None
            extras.setdefault("dl_err_us", []).append((tod_d, dev, float(dl.group(5))))
            if dl.group(6) is not None:
                extras.setdefault("dl_trim_ppm", []).append(
                    (tod_d, dev, float(dl.group(6))))
            if dl.group(7) is not None:
                extras.setdefault("dl_int_ppm", []).append(
                    (tod_d, dev, float(dl.group(7))))
            continue
        ds = DL_STATE_RE.match(line)
        if ds:
            tod_s = (int(ds.group(1)) * 3600 + int(ds.group(2)) * 60 + int(ds.group(3))
                     + int(ds.group(4)) / (10 ** len(ds.group(4))))
            # The spacing gate below compares against a time carried in `state`, and that
            # state survives a REWIND: priming walks the whole log and leaves dlmode_t at
            # the end of it, then the replot pass re-reads the same file from byte zero.
            # Every event then failed "now - last >= dl_event_s" on a NEGATIVE difference,
            # and --replot silently drew no delay-loop marks at all -- measured 39 events
            # on the first pass, 0 on the second. Time-of-day also wraps at midnight, which
            # goes backwards for the same reason and must reset the gate just as much.
            if tod_s < state.get("dlmode_t", tod_s):
                state.pop("dlmode_t", None)
                state["dlmode"] = None
            txt = ds.group(5)
            # Six forms in one log, not the two the loop's name suggests: engaged (149),
            # deadline on local fallback (135), setpoint changed (14), integral (10), tags
            # stale (9), integral restored (5). Splitting on the comma left labels like
            # "setpoint changed (" -- a truncation that reads as a parse failure. Take
            # whole words up to the first clause break instead.
            mode = ("engaged" if "engaged" in txt
                    else "holding" if "holding" in txt
                    else " ".join(re.split(r"[,(]", txt, maxsplit=1)[0].split())[:22].strip())
            gi = DL_INTEGRAL_RE.search(txt)
            # On CHANGE, and no more often than dl_event_s. The loop alternates
            # holding->engaged roughly every ten seconds, so every line is a change and
            # marking them all would bury the plot -- the same reason trim is thresholded.
            if (mode != state.get("dlmode")
                    and tod_s - state.get("dlmode_t", -1e9) >= dl_event_s):
                ev.append((tod_s, "dlloop", f"{board}: dl {mode}"
                           + (f" {gi.group(1)} ppm" if gi else "")))
                state["dlmode_t"] = tod_s
            state["dlmode"] = mode
            continue
        spm = SERVOPARAM_RE.match(line)
        if spm:
            tod_p = (int(spm.group(1)) * 3600 + int(spm.group(2)) * 60 + int(spm.group(3))
                     + int(spm.group(4)) / (10 ** len(spm.group(4))))
            # Ungated: a whole log holds 8-11 of these, and each one is a deliberate change
            # to the loop's constants, which is exactly what a reader wants marked.
            ev.append((tod_p, "servoparam", f"{board}: {spm.group(5)}={spm.group(6)}"))
            continue
        stn = SERVOTUNE_RE.match(line)
        if stn:
            tod_t = (int(stn.group(1)) * 3600 + int(stn.group(2)) * 60 + int(stn.group(3))
                     + int(stn.group(4)) / (10 ** len(stn.group(4))))
            if tod_t < state.get("tau_t", tod_t):
                state["tau"] = None
            state["tau_t"] = tod_t
            tau = stn.group(5)
            # Only when tau MOVES. Most SERVOTUNE lines report a hold at the current value,
            # and the "sluggish-looking window ... not acted on" form carries no tau at all
            # and must not be read as a change -- which is why the pattern requires tau=.
            # The first observation is marked too, not just changes: it states what tau the
            # run is actually using. Measured on these logs, tau never moves -- 64 lines,
            # all tau=10.0 -- so a change-only rule would annotate nothing at all and leave
            # the reader unable to tell "unchanged" from "not parsed".
            if state.get("tau") is None:
                ev.append((tod_t, "servotune", f"{board}: tau {tau} s"))
            elif tau != state["tau"]:
                ev.append((tod_t, "servotune",
                           f"{board}: tau {state['tau']} -> {tau} s"))
            state["tau"] = tau
            continue

        # The firmware's own view, on its own log lines: matched before SYNC_RE's continue.
        # Each carries the device timestamp when the firmware is new enough; None falls back
        # to the host prefix so an older log still plots.
        # The value's capture group is carried per pattern rather than assumed to be 5: the
        # Crystal line reports mine, the group AND the delta, and the delta is the useful one.
        rp = RPHASE_RE.match(line)
        if rp:
            tod_r = (int(rp.group(1)) * 3600 + int(rp.group(2)) * 60
                     + int(rp.group(3)) + int(rp.group(4)) / (10 ** len(rp.group(4))))
            dev_r = DEV_T_RE.search(line)
            for name, gi in (("rphase_gate_us", 6), ("rphase_plot_us", 7)):
                tok = rp.group(gi)
                if tok is None or tok == "unknown":
                    continue          # sentinel: absent, NOT zero (the INT64_MIN rule)
                try:
                    val = float(tok)
                except ValueError:
                    continue
                extras.setdefault(name, []).append(
                    (tod_r, val, int(dev_r.group(1)) if dev_r else None))
            continue
        for key, rx, grp in (("phase_us", PHASE_RE, 5),
                             ("depth_us", DEPTH_RE, 5),
                             ("ramp_ppm", RAMP_RE, 5),
                             ("crystal_ppm", CRYSTAL_RE, 7)):
            mx = rx.match(line)
            if mx:
                tod_x = (int(mx.group(1)) * 3600 + int(mx.group(2)) * 60
                         + int(mx.group(3)) + int(mx.group(4)) / (10 ** len(mx.group(4))))
                dv = DEV_T_RE.search(line)
                extras.setdefault(key, []).append(
                    (tod_x, int(dv.group(1)) if dv else None, float(mx.group(grp))))
                break
        si = SPLITINJECT_RE.match(line)
        if si:
            tod_i = (int(si.group(1)) * 3600 + int(si.group(2)) * 60 + int(si.group(3))
                     + int(si.group(4)) / (10 ** len(si.group(4))))
            dv = DEV_T_RE.search(line)
            injects.append((tod_i, int(dv.group(1)) if dv else None, int(si.group(5))))
            continue
        rp = REPAIR_RE.match(line)
        if rp:
            tod_p = (int(rp.group(1)) * 3600 + int(rp.group(2)) * 60 + int(rp.group(3))
                     + int(rp.group(4)) / (10 ** len(rp.group(4))))
            repairs.append((tod_p, None, int(rp.group(5))))
            continue
        dr = SEEDDRAIN_RE.match(line)
        if dr:
            # Attached to the most recent seed: the measurement completes one drain interval after
            # the anchor it belongs to, and only one can be armed at a time.
            if seeds:
                seeds[-1] = seeds[-1][:5] + (int(dr.group(7)),)
            continue
        sa = SEEDANCHOR_RE.match(line)
        if sa:
            tod_s = (int(sa.group(1)) * 3600 + int(sa.group(2)) * 60 + int(sa.group(3))
                     + int(sa.group(4)) / (10 ** len(sa.group(4))))
            dv = DEV_T_RE.search(line)
            seeds.append((tod_s, int(dv.group(1)) if dv else None, int(sa.group(5)),
                          int(sa.group(6)), int(sa.group(7)), None))
            continue
        rs = RSYNC_RE.match(line)
        if rs:
            tod_r = (int(rs.group(1)) * 3600 + int(rs.group(2)) * 60 + int(rs.group(3))
                     + int(rs.group(4)) / (10 ** len(rs.group(4))))
            rsyncs.append((tod_r, int(rs.group(5)), int(rs.group(6)), int(rs.group(7)),
                           int(rs.group(8)), int(rs.group(9)), int(rs.group(10))))
            continue
        rp = RPRE_RE.match(line)
        if rp:
            tod_p = (int(rp.group(1)) * 3600 + int(rp.group(2)) * 60 + int(rp.group(3))
                     + int(rp.group(4)) / (10 ** len(rp.group(4))))
            first_seq, t0 = int(rp.group(5)), int(rp.group(7))
            # THE SHAPE GUARD BELOW CANNOT RUN IF THE PARSE THROWS FIRST. A line truncated
            # mid-number leaves an empty or partial element, int() raises ValueError, and the
            # exception escapes parse_sync_events and kills the whole analyser -- which it did
            # on 2026-09-02, taking the run down with test.csv frozen. The comment below always
            # said truncation was expected here; only the length mismatch was being handled.
            try:
                cols = [[int(v) for v in rp.group(g).split(",")] for g in (8, 9, 10, 11, 12)]
            except ValueError:
                continue          # truncated mid-number: refuse the line, as the shape guard does
            # A truncated line (the formatting ceiling is 256 bytes and six saturated samples
            # can approach it) would zip short and silently drop the tail; refuse it instead.
            if len({len(c) for c in cols}) == 1:
                dts, errs, meds, rings, drops = cols
                t = t0
                for i in range(len(dts)):
                    if i:
                        t += dts[i]  # dt[0] is the gap to the PREVIOUS line, not within this one
                    rsyncs.append((tod_p, -(first_seq - i), t, errs[i], meds[i],
                                   rings[i], drops[i]))
            continue
        td = TRIMDBG_RE.match(line)
        if td:
            tod_d = (int(td.group(1)) * 3600 + int(td.group(2)) * 60 + int(td.group(3))
                     + int(td.group(4)) / (10 ** len(td.group(4))))
            applied, samples = float(td.group(5)), int(td.group(6))
            railed, splithold, gated = int(td.group(7)), int(td.group(10)), int(td.group(11))
            # The three ways steering can fail to happen, each previously invisible because the
            # field carrying them sat past the truncation point.
            if samples == 0:
                why = "split-hold" if splithold else ("gated" if gated else "no chunks")
                ev.append((tod_d, "trimstop", f"{board}: no steering ({why})"))
            elif railed:
                ev.append((tod_d, "trimrail", f"{board}: trim railed {railed}/{samples}"))
            if last_trim is not None and abs(applied - last_trim) >= trim_ppm:
                ev.append((tod_d, "trim", f"{board}: trim {last_trim:+.0f}->{applied:+.0f} ppm"))
            last_trim = applied
            continue
        to = TRIMDBG_OFF_RE.match(line)
        if to:
            tod_o = (int(to.group(1)) * 3600 + int(to.group(2)) * 60 + int(to.group(3))
                     + int(to.group(4)) / (10 ** len(to.group(4))))
            ev.append((tod_o, "trimstop", f"{board}: rate lock unavailable"))
            continue
        sx = SYNCX_RE.match(line)
        if sx:
            tod_x = (int(sx.group(1)) * 3600 + int(sx.group(2)) * 60 + int(sx.group(3))
                     + int(sx.group(4)) / (10 ** len(sx.group(4))))
            pipe = int(sx.group(5))
            if last_pipe is not None and abs(pipe - last_pipe) >= pipe_ms:
                ev.append((tod_x, "pipeline", f"{board}: pipeline {last_pipe}->{pipe} ms"))
            last_pipe = pipe
            tm = SYNCX_TSF_RE.search(line)
            if tm and last_consensus[0] is not None and int(tm.group(2)) != last_consensus[0]:
                # A consensus MEMBERSHIP change: measured 2026-08-28 to raise |median error| from
                # 93 us to 154 us (p90 286 -> 674) within 15 s of one, so it belongs on the plot.
                ev.append((tod_x, "consensus",
                           f"{board}: consensus n{last_consensus[0]}->{tm.group(2)}"))
            if tm:
                last_consensus[0] = int(tm.group(2))
            continue
        rt = RENDERTAG_RE.match(line)
        if rt:
            tod_t = (int(rt.group(1)) * 3600 + int(rt.group(2)) * 60 + int(rt.group(3))
                     + int(rt.group(4)) / (10 ** len(rt.group(4))))
            meas, inf = rt.group(5), rt.group(6)
            if meas == "unknown":
                ev.append((tod_t, "rendertag",
                           f"{board}: render phase unknown (tags {rt.group(7)})"))
            elif inf != "unknown":
                gap = int(meas) - int(inf)
                if abs(gap) >= rendertag_us:
                    ev.append((tod_t, "rendertag", f"{board}: ledger bias {gap:+d} us"))
            continue
        pi = PHASEIN_RE.match(line)
        if pi:
            tod_i2 = (int(pi.group(1)) * 3600 + int(pi.group(2)) * 60 + int(pi.group(3))
                      + int(pi.group(4)) / (10 ** len(pi.group(4))))
            # Name the worst peer rather than reporting that "something" moved: the whole reason
            # this line exists is that the group's OUTPUT could not say which input caused it.
            worst, worst_d = None, 0
            for mac, d, _age in PHASEIN_PEER_RE.findall(pi.group(6)):
                if abs(int(d)) > abs(worst_d):
                    worst, worst_d = mac, int(d)
            if worst is not None and abs(worst_d) >= phasein_us:
                ev.append((tod_i2, "phasein",
                           f"peer {worst} phase {worst_d:+d} us (group {pi.group(7)})"))
            continue
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
        down, up, nres = int(m.group(5)), int(m.group(6)), int(m.group(7))
        # None when the line was truncated before the trim field, or when the firmware puts trim
        # on its own TRIMDBG line. Never crash on it: the corrections and resyncs above are the
        # load-bearing part of this record and must survive a missing trailing field.
        trim = float(m.group(8)) if m.group(8) is not None else None
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
        if trim is not None:
            if last_trim is not None and abs(trim - last_trim) >= trim_ppm:
                ev.append((tod, "trim", f"{board}: trim {last_trim:+.0f}->{trim:+.0f} ppm"))
            last_trim = trim
    end = f.tell()
    f.close()
    state["trim"], state["resync"], state["pipe"] = last_trim, last_resync, last_pipe
    state["consensus"] = last_consensus[0]
    state["role"] = last_role
    return ev, end, state, temps, trims, extras, rsyncs, seeds, repairs, injects


def place_device_times(rows, host_of, dev_of):
    """Map each row's device timestamp onto the host axis, ANCHORED PER BOOT.

    Returns (placed_times, jitter_ms, n_stamped). placed_times[i] is None where the row has no
    device stamp, so the caller falls back to the host prefix.

    The anchor must be fitted per boot epoch and this is the whole subtlety. esp_timer counts
    from boot, so every reboot sends the device clock back to ~0 while host time keeps running:
    a single median of (host - device) over a log spanning reboots is fitted across a
    discontinuity and lands nowhere. Measured when it was: the placed axis came out at
    -1941..-836 s against a capture at 0..272 s, no overlap at all, so every window silently
    found no analyser samples and the comparison reported nothing rather than failing loudly.
    That is the project's own "never read a slope across a gap" in a new costume -- here it was
    an OFFSET across a gap.

    Rows must be in log order, since epochs are detected by the device clock going backwards and
    device values from different epochs overlap, so order is the only thing that separates them.
    """
    epochs, cur, prev = [], [], None
    for i, r in enumerate(rows):
        d = dev_of(r)
        if d is not None:
            if prev is not None and d < prev:
                epochs.append(cur)
                cur = []
            prev = d
        cur.append(i)
    epochs.append(cur)
    placed = [None] * len(rows)
    resid, n = [], 0
    for idx in epochs:
        pairs = [(host_of(rows[i]), dev_of(rows[i]) / 1e6) for i in idx if dev_of(rows[i]) is not None]
        if not pairs:
            continue
        diffs = sorted(h - d for h, d in pairs)
        off = diffs[len(diffs) // 2]
        resid += [abs(x - off) for x in diffs]
        n += len(pairs)
        for i in idx:
            d = dev_of(rows[i])
            if d is not None:
                placed[i] = d / 1e6 + off
    jitter = sorted(resid)[len(resid) // 2] * 1000.0 if resid else None
    return placed, jitter, n


def device_anchor(pairs):
    """(offset_s, jitter_ms, n) mapping one board's esp_timer clock onto the host axis.

    Each board's device clock counts from its own boot, so it needs an anchor before it can
    share an axis -- but the anchor should be fitted from MANY lines rather than taken from
    one, because the host prefix each pair uses is receive time and carries the delay this
    whole change exists to route around. The median of (host - device) averages that out;
    the residual MAD is then a direct measurement of the host-side jitter, which is worth
    printing since it is the number that justifies the device stamp in the first place.

    Drift between the two clocks is ppm-scale, so over a plotted window of minutes it is
    sub-millisecond and a constant offset is enough. Over hours it would not be, which is
    why this is fitted from the lines inside the run rather than the whole log.
    """
    if not pairs:
        return None, None, 0
    diffs = sorted(t - d for t, d in pairs)
    off = diffs[len(diffs) // 2]
    mad = sorted(abs(x - off) for x in diffs)[len(diffs) // 2]
    return off, mad * 1000.0, len(diffs)


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


def _as_xy(pts):
    """(n, 2) float array from a series, sorted by x with non-finite rows dropped.

    fromiter over a flattened chain, not np.asarray(list_of_tuples): measured 13.1 ms
    against 38.5 ms for 400k points, because asarray has to probe each tuple for shape and
    type. An array that arrives already an array costs nothing, so a caller that keeps its
    series in numpy pays no conversion at all.
    """
    if isinstance(pts, np.ndarray):
        arr = pts.astype(float, copy=False).reshape(-1, 2)
    else:
        n = len(pts)
        if not n:
            return np.empty((0, 2))
        arr = np.fromiter(itertools.chain.from_iterable(pts), dtype=float,
                          count=2 * n).reshape(-1, 2)
    arr = arr[np.isfinite(arr[:, 0]) & np.isfinite(arr[:, 1])]
    if arr.shape[0] > 1 and not np.all(np.diff(arr[:, 0]) >= 0):
        arr = arr[np.argsort(arr[:, 0], kind="stable")]
    return arr


def _envelope(xs, vs, x_lo, x_hi, buckets):
    """Aggregate a dense series into at most ``buckets`` columns of (x, lo, hi, mean).

    This is what a wide time window actually wants. Stride sampling keeps 1 point in n and
    throws the other n-1 away, so an excursion narrower than the stride is simply not on
    the plot -- and on this bench the excursions ARE the signal. Bucketing by PIXEL COLUMN
    keeps the extremes of every column, which is the same picture the eye would get from
    the undecimated trace, at a vertex count bounded by the width of the panel rather than
    by the length of the run.

    It is also the cheaper path at size: the work is four numpy reductions over the raw
    arrays instead of a Python loop, and the drawn vertex count stops growing with n.

    ``xs`` must be non-decreasing, which makes the bucket index non-decreasing too, so the
    reductions are reduceat over runs rather than the much slower ufunc.at scatter.
    """
    span = (x_hi - x_lo) or 1.0
    idx = ((xs - x_lo) / span * buckets).astype(np.int64)
    np.clip(idx, 0, buckets - 1, out=idx)
    starts = np.flatnonzero(np.diff(idx, prepend=idx[0] - 1))
    counts = np.diff(np.append(starts, xs.size)).astype(np.float64)
    return (np.add.reduceat(xs, starts) / counts,
            np.minimum.reduceat(vs, starts),
            np.maximum.reduceat(vs, starts),
            np.add.reduceat(vs, starts) / counts)


def _split_runs(xs):
    """Index bounds of contiguous runs, split on gaps -- split_gaps over numpy arrays.

    Same rule (ten times the median spacing, floored at half a second) but computed on the
    RAW series. split_gaps ran on the already-decimated points, so its median spacing was
    the stride times the true spacing and it could not see a gap shorter than ten strides:
    at 20k captures that is a blind spot of ~80 samples. Segmenting the raw series first
    also lets each segment be enveloped on its own, so a band never spans a dropout.
    """
    if xs.size < 3:
        return [(0, xs.size)] if xs.size else []
    d = np.diff(xs)
    med = float(np.median(d))
    limit = max(10 * med, 0.5) if med > 0 else 0.5
    cuts = np.flatnonzero(d > limit) + 1
    bounds = np.concatenate(([0], cuts, [xs.size]))
    return [(int(a), int(b)) for a, b in zip(bounds, bounds[1:]) if b > a]


def _write_atomic(path, text):
    """Write via a temp file and rename.

    The live view no longer reads this file -- it is served the display list directly --
    but anything else watching the path still can, and a plain truncate-and-write is
    visible half-finished to whatever reads it next. The rename is atomic, so a reader
    sees either the old plot or the new one and never a partial document.
    """
    # A unique temp per WRITE, not per process. `{path}.tmp{pid}` collided whenever two
    # writes overlapped -- the final plot in main() is not under plot_busy, so it races a
    # live draw thread -- and the loser's os.replace raised FileNotFoundError after the
    # winner had already renamed the shared name away. Same directory, so the replace
    # stays on one filesystem and stays atomic.
    d, base = os.path.dirname(path) or ".", os.path.basename(path)
    fd, tmp = tempfile.mkstemp(dir=d, prefix=base + ".", suffix=".tmp")
    try:
        with os.fdopen(fd, "w") as f:
            f.write(text)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


# Drawing ops. The layout below emits these, and two back ends consume them: an SVG
# writer for the file on disk, and a JSON feed for the live canvas in --serve.
#
# One layout, two renderers. The alternative -- porting the axis ranging, the robust
# overlay scaling, the gap splitting and the tick-decimal rule into JavaScript -- would
# have put the plot's actual judgement calls in two places, and the browser copy would
# have drifted from the file copy silently, which is the exact failure this project
# keeps re-learning. The canvas gets device coordinates and paints them; it decides
# nothing.
#
#   ["rect",   x, y, w, h, fill, opacity]
#   ["line",   x1, y1, x2, y2, stroke, width, dash, opacity]
#   ["poly",   [x0,y0,x1,y1,...], stroke, width, dash, opacity, clip]
#   ["area",   [x0,y0,x1,y1,...], fill, opacity, clip]        (closed, filled)
#   ["circle", cx, cy, r, fill]
#   ["text",   x, y, s, anchor, fill, size, weight, rot]
#
# dash is None or [on, off]; clip is None or "top"; rot is None or [deg, cx, cy].


# Chrome colours are named, not literal, so the two back ends can disagree about what
# "axis" looks like without disagreeing about the plot. The SVG resolves every token to
# the value it has always used -- the file is unchanged -- and the canvas resolves the
# same token against the viewer's colour scheme.
#
# Only the CHROME is tokenised. The data colours stay literal hex, because a categorical
# palette is the one thing that must not be re-chosen per theme: the reader identifies a
# series by hue, and a dark-mode palette that reassigned hues would make two screenshots
# of the same run disagree about which trace is which. The canvas lifts their lightness
# for contrast and leaves the hue alone.
THEME_LIGHT = {
    "bg": "white", "fg": "#000", "grid": "#e5e5e5", "axis": "#333",
    "tick": "#555", "tickmark": "#888", "stats": "#666", "zero": "#888",
    "zero2": "#bbb", "shade": "#000",
}


def _svg_colour(c):
    return THEME_LIGHT.get(c, c)


def _xml_escape(s):
    return (str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
            .replace('"', "&quot;"))


def _ops_to_svg(ops, W, H, clip=None):
    """Render a display list as an SVG document."""
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
         f'font-family="-apple-system,sans-serif" font-size="12">']
    if clip:
        cx, cy, cw, ch = clip
        o.append(f'<defs><clipPath id="toppanel"><rect x="{cx}" y="{cy}" '
                 f'width="{cw}" height="{ch}"/></clipPath></defs>')
    open_clip = False
    for op in ops:
        kind = op[0]
        want_clip = ((kind == "poly" and op[5] is not None)
                     or (kind == "area" and op[4] is not None))
        if want_clip and not open_clip:
            o.append('<g clip-path="url(#toppanel)">')
            open_clip = True
        elif open_clip and not want_clip:
            o.append('</g>')
            open_clip = False
        if kind == "rect":
            _, x, y, w, h, fill, opac = op
            a = f' opacity="{opac}"' if opac is not None else ""
            o.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="{_svg_colour(fill)}"{a}/>')
        elif kind == "line":
            _, x1, y1, x2, y2, st, wd, dash, opac = op
            a = f' stroke-width="{wd}"' if wd is not None else ""
            if dash:
                a += f' stroke-dasharray="{dash[0]} {dash[1]}"'
            if opac is not None:
                a += f' opacity="{opac}"'
            o.append(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{_svg_colour(st)}"{a}/>')
        elif kind == "poly":
            _, flat, st, wd, dash, opac, _clip = op
            pl = " ".join(f"{flat[i]},{flat[i+1]}" for i in range(0, len(flat), 2))
            a = f' stroke-width="{wd}"' if wd is not None else ""
            if dash:
                a += f' stroke-dasharray="{dash[0]} {dash[1]}"'
            if opac is not None:
                a += f' opacity="{opac}"'
            o.append(f'<polyline points="{pl}" fill="none" stroke="{_svg_colour(st)}"{a}/>')
        elif kind == "area":
            _, flat, fill, opac, _clip = op
            pl = " ".join(f"{flat[i]},{flat[i+1]}" for i in range(0, len(flat), 2))
            a = f' opacity="{opac}"' if opac is not None else ""
            o.append(f'<polygon points="{pl}" fill="{_svg_colour(fill)}" '
                     f'stroke="none"{a}/>')
        elif kind == "circle":
            _, cx, cy, r, fill = op
            o.append(f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="{_svg_colour(fill)}"/>')
        elif kind == "text":
            _, x, y, s, anchor, fill, size, weight, rot = op
            a = ""
            if anchor:
                a += f' text-anchor="{anchor}"'
            if fill:
                a += f' fill="{_svg_colour(fill)}"'
            if size is not None:
                a += f' font-size="{size}"'
            if weight:
                a += f' font-weight="{weight}"'
            if rot:
                a += f' transform="rotate({rot[0]} {rot[1]} {rot[2]})"'
            o.append(f'<text x="{x}" y="{y}"{a}>{_xml_escape(s)}</text>')
    if open_clip:
        o.append('</g>')
    o.append("</svg>")
    return "\n".join(o)


def build_plot(ts, ys, title, ylabel, include_zero=True,
               xlabel="elapsed (s)", log_axes=False, events=(), panels=(), stats=None,
               overlays=None, expand_for_overlays=False, width=PLOT_WIDTH, hidden=()):
    """Lay the plot out and return (ops, W, H, clip) -- no plotting dependency.

    ``hidden`` is a set of group ids not to draw. Hiding happens HERE, in the layout, not
    in the renderer: a hidden panel has to give its band back, and a hidden series has to
    stop stretching its panel's axis. A renderer that merely skipped the ops would leave a
    blank band and an axis scaled to something invisible -- the plot would be lying about
    its own range. The returned ``legend`` lists every toggleable group so the page can
    build its controls from the frame instead of hard-coding a list that goes stale.

    ``width`` exists so the live view can be laid out at the browser's width instead of
    being a 900 px picture stretched to fit. Stretching would scale the type with the
    plot; relaying out spends the extra pixels on TIME, which is the axis a wider window
    is being given to. The margins are fixed, so only the plotting area grows.

    Extra panels are drawn only where they have data, so a run without temperature
    logging looks exactly as before. Both panels share the x axis and the event bars
    span both, which is the point: it should be obvious at a glance whether a skew
    excursion lines up with a temperature move.
    """
    W, ML, M, MB = max(420, int(width)), 108, 70, 70
    hid = set(hidden or ())
    legend = []          # {id, label, kind, colour} for every group that COULD be drawn
    def grp(gid, label, kind, colour=None):
        legend.append({"id": gid, "label": label, "kind": kind, "colour": colour})
        return gid not in hid
    # Only panels with something in them take space, so a run without temperature (or
    # without rate columns, replotting an older CSV) looks exactly as it did before.
    # Non-finite values are dropped here, not drawn: a NaN in a panel series poisoned the
    # panel's whole axis range (min/max of anything containing NaN is NaN) and then every
    # coordinate computed from it. The trace and the overlays have always filtered; the
    # panels did not. Empty series are KEPT so a colour and legend slot are not silently
    # reassigned, which is what dropping them would do.
    extra = []
    for item in panels:
        # (label, series[, pid[, sid_map]]). The ID is separate from the LABEL because the
        # label is not stable: "d(rate)/dt (Hz/s), 1s fit" carries a fit window that
        # rate_derivative widens to at least five sample spacings, so it reads 5s early in
        # a run and 1.2s once rows arrive faster. Keyed on the label, a toggle stopped
        # matching the moment the window moved and the panel silently came back.
        lab, series = item[0], item[1]
        pid = (item[2] if len(item) > 2 and item[2] else lab)
        sid_map = (item[3] if len(item) > 3 and item[3] else {})
        if not series:
            continue
        # (n, 2) float arrays, filtered and sorted ONCE here. The scalar version filtered
        # with a comprehension, then sorted a list of tuples, then rebuilt arrays for
        # drawing -- three Python passes over every panel point on every frame.
        clean = {k: _as_xy(pts_) for k, pts_ in series.items()}
        if not any(a.size for a in clean.values()):
            continue
        panel_on = grp(f"panel:{pid}", lab, "panel")
        # A hidden SERIES keeps its key with an empty list, so the palette index and legend
        # row of the series after it do not shift -- the same reason empty series are kept
        # above. A panel with nothing left to draw gives its band back entirely.
        kept = {}
        order = sorted(clean)
        for k, arr in clean.items():
            # Same index the drawing loop will use -- it enumerates sorted(series.items())
            # over these same keys -- so the swatch matches the line.
            col_k = PANEL_PALETTE[order.index(k) % len(PANEL_PALETTE)]
            on = grp(f"series:{pid}|{sid_map.get(k, k)}", k, "series", col_k) and panel_on
            kept[k] = arr if on else np.empty((0, 2))
        if panel_on and any(a.size for a in kept.values()):
            extra.append((lab, kept))
    top0, top1 = M, M + 280
    bands, y = [], top1
    for _ in extra:
        y += 62
        bands.append((y, y + 150))
        y += 150
    H = y + MB

    # Vectorised deliberately. The scalar version -- a comprehension over zip(ts, ys) with
    # math.isfinite, then min/max over Python lists -- was 2.4M isfinite calls and 1.6M
    # min/max calls at 400k captures, and it, not the drawing, was the whole cost of a
    # wide window: 550 ms of which the aggregation itself was 8.
    xs_a = np.asarray(ts, dtype=float)
    vs_a = np.asarray(ys, dtype=float)
    if xs_a.size != vs_a.size:
        n_ = min(xs_a.size, vs_a.size)
        xs_a, vs_a = xs_a[:n_], vs_a[:n_]
    keep = np.isfinite(xs_a) & np.isfinite(vs_a)
    xs_a, vs_a = xs_a[keep], vs_a[keep]
    pts = xs_a          # truthiness/len only; the tuple list is never built
    if not xs_a.size:
        return ([["text", W / 2, H / 2, "no valid measurements yet", "middle",
                  "fg", None, None, None]], W, H, None, legend)
    x0, x1 = float(xs_a.min()), float(xs_a.max())
    for _, series in extra:
        for arr in series.values():
            if arr.size:
                x0 = min(x0, float(arr[0, 0]))
                x1 = max(x1, float(arr[-1, 0]))
    if x1 - x0 < 1e-9:
        x1 = x0 + 1

    y0, y1 = float(vs_a.min()), float(vs_a.max())
    # Overlays set the scale too -- confining them to the measured range showed only the
    # part that happened to fall inside it, which reads as agreement where there may be
    # none. But follow their ROBUST range, not their extremes: 2 of 1572 render-phase
    # values sit near -1.8 s against a median of -41 us, and honouring those stretched the
    # axis a thousandfold and flattened everything real. The outliers are still drawn,
    # clipped to the panel, and counted in the legend.
    # The axis follows the MEASUREMENT, not the overlays. Scaling to fit the firmware's
    # estimates -- even robustly -- costs an order of magnitude of range, and the thing
    # actually being read on this panel is the measured trace converging, which then
    # collapses to a flat line. Overlays are still drawn, clipped to the panel, with the
    # off-scale count in the legend so nothing looks like agreement by omission.
    # --overlay-expand restores the fit-everything behaviour.
    if expand_for_overlays:
        for pts_ in (overlays or {}).values():
            vals = [v for _x, v in pts_ if math.isfinite(v)]
            if not vals:
                continue
            # Median +- 6 MAD, not min/max: a single -1.8 s excursion against a -41 us
            # median would stretch the axis a thousandfold.
            arr = np.asarray(vals, dtype=float)
            med = float(np.median(arr))
            mad = float(np.median(np.abs(arr - med)))
            if mad > 0:
                y0 = min(y0, med - 6 * 1.4826 * mad)
                y1 = max(y1, med + 6 * 1.4826 * mad)
            else:
                y0, y1 = min(y0, float(arr.min())), max(y1, float(arr.max()))
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

    def r1(v):
        return round(v, 1)

    py = mk_py(top0, top1, y0, y1)
    clip = (ML, top0, W - 40 - ML, top1 - top0)
    o = [["rect", 0, 0, W, H, "bg", None],
         ["text", ML, 26, title, None, "fg", 15, "600", None]]
    if stats:
        o.append(["text", ML, 44, stats, None, "stats", 11, None, None])

    def tick(vv, span):
        """Decimals from the axis SPAN, not the value. A rate axis spanning 0.3 Hz around
        44100 needs three decimals or every label reads the same integer; a derivative
        axis spanning 1e-6 needs exponent form instead of ten zeros."""
        if not (span > 0):
            return f"{vv:.4g}"
        if span < 1e-3:
            return f"{vv:.2e}"
        d = max(0, min(9, int(math.ceil(-math.log10(span / 5.0))) + 1))
        return f"{vv:,.{d}f}"

    def axis(p0, p1, v0, v1, lab, pyf, xticks):
        for k in range(6):
            v = v0 + (v1 - v0) * k / 5
            yy = pyf(v)
            o.append(["line", ML, r1(yy), W - 40, r1(yy), "grid", None, None, None])
            vv = 10 ** v if log_axes else v
            t_lbl = f"{vv:,.0f}" if log_axes else tick(vv, v1 - v0)
            o.append(["text", ML - 8, r1(yy + 4), t_lbl, "end", "tick", None, None, None])
        o.append(["line", ML, p1, W - 40, p1, "axis", None, None, None])
        o.append(["line", ML, p0, ML, p1, "axis", None, None, None])
        o.append(["text", 16, (p0 + p1) / 2, lab, "middle", "axis", None, None,
                  [-90, 16, (p0 + p1) / 2]])
        if xticks:
            for k in range(6):
                t = x0 + (x1 - x0) * k / 5
                xx = px(t)
                o.append(["line", r1(xx), p1, r1(xx), p1 + 5, "tickmark", None, None, None])
                tt = 10 ** t if log_axes else t
                if not log_axes and abs(tt) < (x1 - x0) * 1e-3:
                    tt = 0.0
                o.append(["text", r1(xx), p1 + 20, f"{tt:.4g}", "middle", "tick",
                          None, None, None])
            o.append(["text", W / 2, p1 + 42, xlabel, "middle", "axis", None, None, None])

    axis(top0, top1, y0, y1, ylabel, py, not extra)
    if y0 <= 0 <= y1:
        z = py(0.0)
        o.append(["line", ML, r1(z), W - 40, r1(z), "zero", 1.2, [2, 3], None])
        o.append(["text", W - 36, r1(z + 4), "0", None, "zero", None, None, None])

    bar_bottom = bands[-1][1] if bands else top1
    colours = {"corrected": "#d9534f", "resync": "#8e44ad", "trim": "#e0a800",
               "sync": "#1a7f37", "pipeline": "#0969da", "role": "#000000",
               "rendertag": "#c2410c", "phasein": "#0e7490",
               "trimstop": "#b91c1c", "trimrail": "#a16207", "consensus": "#7c3aed",
               "dlloop": "#0d9488", "servoparam": "#65a30d", "servotune": "#7e22ce",
               "oor": "#e11d48"}
    # Registered per KIND, not per event: there can be hundreds of marks and the useful
    # control is "stop showing trims", never "stop showing this one trim".
    ev_on = {}
    for kind in sorted({k for _x, k, _l in events}):
        ev_on[kind] = grp(f"event:{kind}", kind, "event", colours.get(kind, "#888"))
    for i, (ex, kind, label) in enumerate(sorted(events)):
        if not (x0 <= ex <= x1) or not ev_on.get(kind, True):
            continue
        xx = px(ex)
        col = colours.get(kind, "#888")
        o.append(["line", r1(xx), top0, r1(xx), bar_bottom, col, 1.1, [4, 3], 0.75])
        ly = top1 - 6 - (i % 3) * 12
        txt = label if len(label) <= 30 else label[:29] + "…"
        o.append(["text", r1(xx - 3), r1(ly), txt, None, col, 9, None,
                  [-90, r1(xx - 3), r1(ly)]])

    # Decimate for drawing: beyond a few thousand points the extra vertices are below
    # one pixel, and a run of 100k captures would otherwise emit a multi-megabyte SVG
    # that is rewritten after every capture.
    trace_on = grp("trace", "measured skew", "trace", "#2b6cb0")
    band_on = grp("band", "min/max band", "trace", "#2b6cb0")
    # One bucket per pixel column: past that the extra vertices are sub-pixel, and this
    # bounds the drawn size by the PANEL rather than by the length of the run, which is
    # the whole point at a wide window.
    ncol = max(1, min(int(W - ML - 40), MAX_PLOT_POINTS))
    # Segment the RAW series, then aggregate each segment, so a band never spans a dropout.
    runs = _split_runs(xs_a)
    ndrawn = 0
    for (a, b) in (runs if trace_on else []):
        n = b - a
        if n == 1:
            o.append(["circle", r1(px(xs_a[a])), r1(py(vs_a[a])), 2, "#2b6cb0"])
            continue
        if n <= ncol:
            # Sparse enough to draw every point: no aggregation, no band, nothing hidden.
            flat = []
            for i in range(a, b):
                flat.append(r1(px(xs_a[i])))
                flat.append(r1(py(vs_a[i])))
            o.append(["poly", flat, "#2b6cb0", 1.5, None, None, None])
            ndrawn += n
            continue
        cx, lo_, hi_, mean_ = _envelope(xs_a[a:b], vs_a[a:b], x0, x1, ncol)
        pxs = np.round(ML + (cx - x0) / ((x1 - x0) or 1) * (W - ML - 40), 1)
        yl = np.round(top1 - (lo_ - y0) / ((y1 - y0) or 1) * (top1 - top0), 1)
        yh = np.round(top1 - (hi_ - y0) / ((y1 - y0) or 1) * (top1 - top0), 1)
        ym = np.round(top1 - (mean_ - y0) / ((y1 - y0) or 1) * (top1 - top0), 1)
        if band_on:
            # Forward along the max edge, back along the min edge: one closed polygon per
            # segment. The band is min/max, NOT a confidence interval -- it is the actual
            # extremes of the samples in that column, so a spike one sample wide is still
            # on the plot at full height.
            band = np.empty(4 * pxs.size)
            band[0::2] = np.concatenate((pxs, pxs[::-1]))
            band[1::2] = np.concatenate((yh, yl[::-1]))
            o.append(["area", band.tolist(), "#2b6cb0", 0.25, None])
        flat = np.empty(2 * pxs.size)
        flat[0::2] = pxs
        flat[1::2] = ym
        o.append(["poly", flat.tolist(), "#2b6cb0", 1.2, None, None, None])
        ndrawn += int(pxs.size)
    # The firmware's own estimates, on the same axis as the measurement. Thin and dashed so
    # the measured trace stays dominant: these are claims, not observations.
    if overlays:
        opal = ["#7c3aed", "#be123c", "#0f766e", "#a16207"]
        for i, (name, pts_) in enumerate(sorted(overlays.items())):
            gd = _as_xy(pts_)
            if gd.shape[0] < 2:
                continue
            col = opal[i % len(opal)]
            # Registered with its colour BEFORE the hide test, so a hidden overlay still
            # appears in the controls -- a toggle you cannot find again is a one-way door.
            if not grp(f"overlay:{name}", name, "overlay", col):
                continue
            # Counted against the FINAL axis, so the legend states how much of the overlay
            # the reader cannot see.
            nof = int(np.count_nonzero((gd[:, 1] < y0) | (gd[:, 1] > y1)))
            # Clipped to the panel: an off-scale excursion would otherwise draw straight
            # across the panels below it.
            gx, gv = gd[:, 0], gd[:, 1]
            for oa, ob in _split_runs(gx):
                if ob - oa < 2:
                    continue
                if ob - oa <= ncol:
                    sxp = np.round(ML + (gx[oa:ob] - x0) / ((x1 - x0) or 1)
                                   * (W - ML - 40), 1)
                    syp = np.round(top1 - (gv[oa:ob] - y0) / ((y1 - y0) or 1)
                                   * (top1 - top0), 1)
                else:
                    # Overlays get the MEAN line only, no band. They are the firmware's
                    # claims rather than measurements, and they stay thin and dashed so the
                    # measured trace and its band remain what the eye reads first.
                    cx_, _lo, _hi, mn_ = _envelope(gx[oa:ob], gv[oa:ob], x0, x1, ncol)
                    sxp = np.round(ML + (cx_ - x0) / ((x1 - x0) or 1) * (W - ML - 40), 1)
                    syp = np.round(top1 - (mn_ - y0) / ((y1 - y0) or 1)
                                   * (top1 - top0), 1)
                flat = np.empty(2 * sxp.size)
                flat[0::2] = sxp
                flat[1::2] = syp
                o.append(["poly", flat.tolist(), col, 1, [5, 3], 0.85, "top"])
            tag = f"{name} ({nof}/{gd.shape[0]} off-scale)" if nof else name
            o.append(["text", W - 44, top0 + 14 + i * 13, tag, "end", col, 10, None, None])
    # Shade what is missing, so a gap reads as absence rather than as an axis break.
    gaps_on = len(runs) > 1 and grp("gaps", "dropout shading", "chrome", None)
    for (_a0, b0), (a1, _b1) in (zip(runs, runs[1:]) if gaps_on else ()):
        gx0, gx1 = px(xs_a[b0 - 1]), px(xs_a[a1])
        if gx1 - gx0 >= 1.0:
            o.append(["rect", r1(gx0), top0, r1(gx1 - gx0), top1 - top0, "shade", 0.05])
    # Dots only where every sample is genuinely drawn -- marking aggregated columns would
    # imply a measurement at the column centre that was never taken.
    if trace_on and 0 < ndrawn <= 800 and ndrawn == xs_a.size:
        for t, v in zip(xs_a.tolist(), vs_a.tolist()):
            o.append(["circle", r1(px(t)), r1(py(v)), 2.5, "#2b6cb0"])

    palette = PANEL_PALETTE
    for bi, ((lab, series), (p0, p1)) in enumerate(zip(extra, bands)):
        allv = np.concatenate([a[:, 1] for a in series.values() if a.size])
        v0, v1 = float(allv.min()), float(allv.max())
        # Rates sit near 44100 with variation of a fraction of a Hz, so the pad has to be
        # relative to the value, not a fixed 1.0, or the trace flattens to a line.
        pad_ = (v1 - v0) * 0.15 or (abs(v0) * 1e-6 or 1.0)
        v0, v1 = v0 - pad_, v1 + pad_
        pyb = mk_py(p0, p1, v0, v1)
        axis(p0, p1, v0, v1, lab, pyb, bi == len(extra) - 1)
        if v0 <= 0 <= v1:
            zz = pyb(0.0)
            o.append(["line", ML, r1(zz), W - 40, r1(zz), "zero2", None, [2, 3], None])
        # Bands from two series in one panel overlap over their whole width and composite
        # to a muddy neutral that reads as a third colour. Thinner when there is more than
        # one, so the overlap stays legible and the MEAN lines carry the comparison.
        # (Hidden series are already empty arrays here, so they do not count.)
        nvis = sum(1 for a in series.values() if a.size)
        band_a = 0.22 if nvis < 2 else 0.10
        for i, (name, arr) in enumerate(sorted(series.items())):
            if not arr.size:
                continue
            col = palette[i % len(palette)]
            sx, sv = arr[:, 0], arr[:, 1]
            for pa, pb in _split_runs(sx):
                if pb - pa < 2:
                    continue
                if pb - pa <= ncol:
                    flat = []
                    for j in range(pa, pb):
                        flat.append(r1(px(sx[j])))
                        flat.append(r1(pyb(sv[j])))
                    o.append(["poly", flat, col, 1.4, None, None, None])
                    continue
                cx, lo_, hi_, mean_ = _envelope(sx[pa:pb], sv[pa:pb], x0, x1, ncol)
                bxs = np.round(ML + (cx - x0) / ((x1 - x0) or 1) * (W - ML - 40), 1)
                bl = np.round(p1 - (lo_ - v0) / ((v1 - v0) or 1) * (p1 - p0), 1)
                bh = np.round(p1 - (hi_ - v0) / ((v1 - v0) or 1) * (p1 - p0), 1)
                bm = np.round(p1 - (mean_ - v0) / ((v1 - v0) or 1) * (p1 - p0), 1)
                if band_on:
                    band = np.empty(4 * bxs.size)
                    band[0::2] = np.concatenate((bxs, bxs[::-1]))
                    band[1::2] = np.concatenate((bh, bl[::-1]))
                    o.append(["area", band.tolist(), col, band_a, None])
                flat = np.empty(2 * bxs.size)
                flat[0::2] = bxs
                flat[1::2] = bm
                o.append(["poly", flat.tolist(), col, 1.2, None, None, None])
            # Left-aligned inside the panel: at the right edge it was easy to miss.
            o.append(["text", ML + 8, p0 + 14 + i * 13, name, None, col, 10, None, None])
    return o, W, H, clip, legend


# Set by --serve. Every write_svg goes through here, so the live view follows the same
# four call sites the file always did -- there is no second "live" path to keep in step.
PLOT_SERVER = None


def write_svg(path, *a, write_file=True, **kw):
    """Lay the plot out, optionally write the SVG, and push it to any live view.

    The layout is computed once and both back ends consume it, so the two outputs can run
    at different rates without laying the plot out twice. Serializing the SVG is the
    expensive half (measured at 20k captures: 11.8 ms to lay out, 3.4 ms to serialize,
    ~16 ms total with the write) -- which is why --svg-every skips the write and not the
    frame. Pass a falsy path to write no file at all.
    """
    srv = PLOT_SERVER
    out = None
    if write_file and path:
        # The FILE is never filtered. It is the artefact of the run, and a toggle someone
        # flicked in a browser must not silently decide what a saved plot contains.
        out = build_plot(*a, width=PLOT_WIDTH, **kw)
        _write_atomic(path, _ops_to_svg(out[0], out[1], out[2], out[3]))
    if srv is not None:
        live_w, hid = srv.width, srv.hidden
        # A second layout only when the browser differs from the file -- which is the
        # common case, and it is still the cheaper half of a frame (11.8 ms against ~16 ms
        # for serializing and writing the SVG at 20k captures).
        if out is None or out[1] != live_w or hid:
            out = build_plot(*a, width=live_w, hidden=hid, **kw)
        srv.publish(*out)
    elif out is None:
        out = build_plot(*a, width=PLOT_WIDTH, **kw)
    return out


# The live view, folded in from the standalone svgwatch.py it replaces.
#
# svgwatch watched the .svg file and, on every change, reassigned img.src. That made the
# browser refetch the document, rebuild an SVG DOM of ~17k nodes and rasterize it from
# scratch -- measured at 70-100 ms per frame with rsvg, and it scaled with the point
# count. It also raced the writer: a plain truncate-and-write was visible to the watcher
# half-finished.
#
# Now the process serves its own display list over one long-lived SSE connection and the
# page paints it to a canvas. No document is reparsed: what crosses the socket is JSON
# arrays of numbers, and the canvas redraw is a few hundred microseconds. The SVG file is
# still written for anything that wants a file.
PAGE = """<!doctype html><meta charset="utf-8"><title>i2s-skew</title>
<style>
 :root{--page:#f6f6f6;--ink:#666;--edge:rgba(0,0,0,.20);--btn:#fff;--btnink:#444}
 @media (prefers-color-scheme: dark){
   :root{--page:#0e1013;--ink:#8b929c;--edge:rgba(0,0,0,.6);--btn:#1d2127;--btnink:#c3c9d2}
 }
 html[data-theme=light]{--page:#f6f6f6;--ink:#666;--edge:rgba(0,0,0,.20);--btn:#fff;--btnink:#444}
 html[data-theme=dark]{--page:#0e1013;--ink:#8b929c;--edge:rgba(0,0,0,.6);--btn:#1d2127;--btnink:#c3c9d2}
 html,body{height:100%}
 html,body{margin:0;background:var(--page);color:var(--ink);font:12px -apple-system,sans-serif}
 /* A column of [toolbar][legend | plot]. Everything is IN FLOW: the toolbar and the
    legend used to be position:fixed and sat on top of the canvas, hiding the y axis and
    the title. In flow they take their own space and the plot gets the rest -- and because
    the relayout width is read from #wrap, opening the legend re-lays the plot narrower
    rather than covering it. */
 body{display:flex;flex-direction:column}
 #bar{flex:none;display:flex;gap:6px;align-items:center;flex-wrap:wrap;
      justify-content:flex-end;padding:5px 8px;border-bottom:1px solid var(--edge)}
 #main{flex:1;display:flex;min-height:0}
 #wrap{flex:1;min-width:0;overflow:auto;padding:8px}
 canvas{box-shadow:0 1px 4px var(--edge);display:block}
 #hud{font-variant-numeric:tabular-nums;margin-right:auto;padding-left:2px}
 #hud.stale{color:#d9534f}
 button{cursor:pointer;border:1px solid var(--edge);background:var(--btn);color:var(--btnink);
     border-radius:5px;padding:2px 7px;font:11px -apple-system,sans-serif}
 button.on{border-color:currentColor;font-weight:600}
 #win{display:flex;gap:3px;flex-wrap:wrap;justify-content:flex-end}
 #legend{flex:none;display:flex;flex-direction:column;gap:2px;overflow:auto;
         padding:7px 10px 10px;border-right:1px solid var(--edge);font-size:11px}
 #legend.hide{display:none}
 #legend h4{margin:5px 0 1px;font-size:10px;font-weight:600;opacity:.65;
            text-transform:uppercase;letter-spacing:.4px}
 #legend label{display:flex;gap:5px;align-items:center;cursor:pointer;white-space:nowrap}
 #legend label.off{opacity:.4;text-decoration:line-through}
 #legend .sw{width:9px;height:9px;border-radius:2px;flex:none}
</style>
<div id=bar><span id=hud>connecting</span>
 <span id=win title="seconds of history plotted (--plot-window)"></span>
 <button id=el title="show/hide the element toggles">elements</button>
 <button id=tt title="colour scheme">auto</button></div>
<div id=main><div id=legend class=hide></div>
<div id=wrap><canvas id=c></canvas></div></div>
<script>
const cv = document.getElementById("c"), ctx = cv.getContext("2d"),
      hud = document.getElementById("hud"), tt = document.getElementById("tt"),
      winbar = document.getElementById("win"), wrap = document.getElementById("wrap"),
      legendEl = document.getElementById("legend");
let W = 0, H = 0, lastFrame = null, last = 0, scale = 1;
const PAD = 16;   // matches #wrap padding, both sides

// How much room the plot actually has. The bar floats, so only the padding is subtracted.
// Math.max(420, NaN) is NaN, and a canvas sized NaN renders nothing at all -- a blank
// page that looks like a dead stream rather than a layout that has not happened yet. A
// pane measured before layout, or in a context with no box, reads 0 or undefined, so the
// floor is applied to a number that is known to be one.
function span(v, min) { return Math.max(min, Math.floor(Number(v) - PAD) || min); }
function availW() { return span(wrap.clientWidth, 420); }
function availH() { return span(wrap.clientHeight, 300); }

// Ask the capture to lay the NEXT frame out at this width. Relayout, not stretch: the
// extra pixels go to the time axis and the type stays 12 px. Debounced, because a drag
// fires resize continuously and each one would otherwise be a request.
let sizeTimer = 0, sentW = 0;
function reportSize() {
  clearTimeout(sizeTimer);
  sizeTimer = setTimeout(() => {
    const w = availW();
    if (w === sentW) return;
    sentW = w;
    fetch("/control?w=" + w).catch(() => {});
  }, 150);
}
window.addEventListener("resize", () => { reportSize(); if (lastFrame) paint(lastFrame); });

// Element toggles, built from the frame's own legend rather than a hard-coded list --
// which series and event kinds exist depends on the run, the flags and the logs, so any
// list written here would be wrong for most runs. Hiding is done by the CAPTURE, not
// here: a hidden panel gives its band back and a hidden series stops stretching its
// panel's axis, neither of which a renderer that just skipped ops could do.
const KINDS = [["trace","trace"],["overlay","overlays"],["event","event marks"],
               ["panel","panels"],["series","panel series"],["chrome","chrome"]];
let hidden = new Set(), legendSig = "";
function sendHidden() {
  fetch("/control?hidden=" + encodeURIComponent([...hidden].join(","))).catch(() => {});
}
function buildLegend(items) {
  // Rebuilt only when the SET of groups changes, not every frame: rebuilding on each
  // frame would drop the scroll position and fight the pointer at 10 fps.
  const sig = items.map(g => g.id).join("|");
  if (sig === legendSig) { syncLegend(); return; }
  legendSig = sig;
  legendEl.innerHTML = "";
  legendEl.classList.toggle("hide", items.length === 0);
  for (const [kind, title] of KINDS) {
    const mine = items.filter(g => g.kind === kind);
    if (!mine.length) continue;
    const h = document.createElement("h4"); h.textContent = title;
    legendEl.appendChild(h);
    for (const g of mine) {
      const lab = document.createElement("label");
      const cb = document.createElement("input");
      cb.type = "checkbox"; cb.dataset.gid = g.id;
      cb.onchange = () => {
        if (cb.checked) hidden.delete(g.id); else hidden.add(g.id);
        lab.className = cb.checked ? "" : "off";
        sendHidden();
      };
      const sw = document.createElement("span");
      sw.className = "sw";
      sw.style.background = g.colour || "transparent";
      if (!g.colour) sw.style.boxShadow = "inset 0 0 0 1px currentColor";
      lab.append(cb, sw, document.createTextNode(g.label));
      legendEl.appendChild(lab);
    }
  }
  syncLegend();
}
function syncLegend() {
  for (const cb of legendEl.querySelectorAll("input")) {
    cb.checked = !hidden.has(cb.dataset.gid);
    cb.parentNode.className = cb.checked ? "" : "off";
  }
}

// Seconds of history. "all" is 0, which the capture reads as the whole run.
const WINDOWS = [["1s",1],["5s",5],["10s",10],["15s",15],["30s",30],
                 ["1m",60],["5m",300],["15m",900],["1h",3600],["6h",21600],["all",0]];
let curWin = null;
for (const [label, secs] of WINDOWS) {
  const b = document.createElement("button");
  b.textContent = label; b.dataset.secs = secs;
  b.onclick = () => {
    curWin = secs;
    for (const el of winbar.children) el.className = (+el.dataset.secs === secs) ? "on" : "";
    fetch("/control?window=" + secs).catch(() => {});
  };
  winbar.appendChild(b);
}

// The chrome tokens the display list names, resolved per scheme. The SVG on disk resolves
// the same tokens to the light column, so the file and a light-mode tab are the same
// picture; only this table knows about dark.
const THEME = {
  light: {bg:"#ffffff", fg:"#000000", grid:"#e5e5e5", axis:"#333333", tick:"#555555",
          tickmark:"#888888", stats:"#666666", zero:"#888888", zero2:"#bbbbbb",
          shade:"#000000", shadeAlpha:1, contrast:0},
  dark:  {bg:"#16181d", fg:"#e8eaed", grid:"#272b33", axis:"#9aa1ab", tick:"#8b929c",
          tickmark:"#6b727c", stats:"#7d848e", zero:"#6f7681", zero2:"#464c55",
          shade:"#ffffff", shadeAlpha:1.3, contrast:4.5}
};

let mode = localStorage.getItem("i2s-theme") || "auto";
const sysDark = window.matchMedia("(prefers-color-scheme: dark)");
function themeName() { return mode === "auto" ? (sysDark.matches ? "dark" : "light") : mode; }
function applyMode() {
  document.documentElement.dataset.theme = mode === "auto" ? "" : mode;
  tt.textContent = mode;
  if (lastFrame) paint(lastFrame);
}
tt.onclick = () => {
  mode = {auto:"light", light:"dark", dark:"auto"}[mode];
  localStorage.setItem("i2s-theme", mode);
  applyMode();
};
sysDark.addEventListener("change", () => { if (mode === "auto") applyMode(); });

// Data colours are never re-chosen, only lightened: the reader identifies a series by
// hue, and a dark palette that reassigned hues would make two screenshots of one run
// disagree about which trace is which. Hue and saturation survive.
//
// The target is CONTRAST against the background, not a fixed HSL lightness. Lightness is
// not perceptual and the palette is not equiluminant: raising every series to L=62% left
// purple at 3.7:1 and teal at 11.9:1 against the same background -- a three-fold spread in
// visual weight that the light theme does not have, applied by a rule that looked uniform.
// Raising each colour only until it clears the ratio keeps the whole palette in one band.
const liftCache = new Map();
function srgbLum(r, g, b) {
  const f = x => x <= 0.03928 ? x/12.92 : Math.pow((x + 0.055)/1.055, 2.4);
  return 0.2126*f(r) + 0.7152*f(g) + 0.0722*f(b);
}
function hslToRgb(h, s, l) {
  const c = (1 - Math.abs(2*l - 1)) * s, hp = h/60, x = c * (1 - Math.abs(hp % 2 - 1));
  const [r, g, b] = hp < 1 ? [c,x,0] : hp < 2 ? [x,c,0] : hp < 3 ? [0,c,x]
                  : hp < 4 ? [0,x,c] : hp < 5 ? [x,0,c] : [c,0,x];
  const m = l - c/2;
  return [r+m, g+m, b+m];
}
function lift(hex, T) {
  if (!T.contrast) return hex;
  let v = liftCache.get(hex);
  if (v !== undefined) return v;
  // Anything that is not a hex triple is handed back untouched rather than parsed into
  // NaN. parseInt("wh", 16) is NaN, NaN*100|0 is 0, and the result was a confident
  // hsl(0 0% 0%) -- a colour that looks like an answer. A colour this cannot read is one
  // it has no business rewriting.
  if (!/^#([0-9a-f]{3}|[0-9a-f]{6})$/i.test(hex)) { liftCache.set(hex, hex); return hex; }
  let h = hex.replace("#", "");
  if (h.length === 3) h = h[0]+h[0]+h[1]+h[1]+h[2]+h[2];
  const r = parseInt(h.slice(0,2),16)/255, g = parseInt(h.slice(2,4),16)/255,
        b = parseInt(h.slice(4,6),16)/255;
  const mx = Math.max(r,g,b), mn = Math.min(r,g,b), l0 = (mx+mn)/2, d = mx-mn;
  let hh = 0, ss = 0;
  if (d) {
    ss = d / (1 - Math.abs(2*l0 - 1));
    hh = mx === r ? ((g-b)/d + (g<b?6:0)) : mx === g ? ((b-r)/d + 2) : ((r-g)/d + 4);
    hh *= 60;
  }
  ss = Math.min(ss, 0.85);
  const bgL = srgbLum(...[1,3,5].map(i => parseInt(T.bg.slice(i,i+2),16)/255));
  const ratio = L => (Math.max(L,bgL) + 0.05) / (Math.min(L,bgL) + 0.05);
  // Monotone in l over the range we search, so bisection is exact enough at 12 steps
  // (~0.02% of the lightness range) and costs nothing behind the cache.
  let lo = l0, hi = 0.92;
  if (ratio(srgbLum(...hslToRgb(hh, ss, lo))) < T.contrast) {
    for (let i = 0; i < 12; i++) {
      const mid = (lo + hi)/2;
      if (ratio(srgbLum(...hslToRgb(hh, ss, mid))) < T.contrast) lo = mid; else hi = mid;
    }
    lo = hi;
  }
  v = `hsl(${hh.toFixed(0)} ${(ss*100)|0}% ${(lo*100).toFixed(1)}%)`;
  liftCache.set(hex, v);
  return v;
}

// The frame arrives at whatever width the capture last laid it out at, which lags a
// resize by one frame and can never match a viewport shorter than the plot is tall. So
// the canvas also scales to fit -- down only. Upscaling would be the stretch this is
// meant to avoid, and it is unnecessary: the next frame arrives at the right width.
function fit(w, h) {
  scale = Math.min(1, availW() / w, availH() / h);
  const r = (window.devicePixelRatio || 1) * scale;
  const cw = Math.round(w * r), ch = Math.round(h * r);
  if (cv.width !== cw || cv.height !== ch) {
    W = w; H = h;
    cv.width = cw; cv.height = ch;
    cv.style.width = Math.round(w * scale) + "px";
    cv.style.height = Math.round(h * scale) + "px";
  }
  ctx.setTransform(r, 0, 0, r, 0, 0);
}

// SVG puts the text origin on the alphabetic baseline, which is canvas's default, so the
// two back ends agree without a fudge factor. Everything else here is a direct
// translation of one op; the page computes no layout of its own.
function paint(f) {
  const t0 = performance.now();
  lastFrame = f;
  if (f.hidden) hidden = new Set(f.hidden);
  if (f.legend) buildLegend(f.legend);
  const T = THEME[themeName()];
  const col = c => (c === null || c === undefined) ? T.fg : (T[c] || lift(c, T));
  fit(f.w, f.h);
  ctx.clearRect(0, 0, f.w, f.h);
  ctx.lineCap = "butt"; ctx.lineJoin = "round";
  let clipped = false;
  for (const op of f.ops) {
    const want = (op[0] === "poly" && op[6] !== null) || (op[0] === "area" && op[4] !== null);
    if (want && !clipped) { ctx.save(); ctx.beginPath();
      ctx.rect(f.clip[0], f.clip[1], f.clip[2], f.clip[3]); ctx.clip(); clipped = true; }
    else if (clipped && !want) { ctx.restore(); clipped = false; }
    switch (op[0]) {
      case "rect": {
        const [, x, y, w, h, fill, opac] = op;
        // The gap shade is the one mark whose job is a fixed fraction of contrast against
        // the background, so its alpha follows the theme rather than the display list.
        ctx.globalAlpha = opac === null ? 1 : opac * (fill === "shade" ? T.shadeAlpha : 1);
        ctx.fillStyle = col(fill); ctx.fillRect(x, y, w, h); ctx.globalAlpha = 1;
        break;
      }
      case "line": {
        const [, x1, y1, x2, y2, st, wd, dash, opac] = op;
        ctx.globalAlpha = opac === null ? 1 : opac;
        ctx.strokeStyle = col(st); ctx.lineWidth = wd === null ? 1 : wd;
        ctx.setLineDash(dash || []);
        ctx.beginPath(); ctx.moveTo(x1, y1); ctx.lineTo(x2, y2); ctx.stroke();
        ctx.setLineDash([]); ctx.globalAlpha = 1;
        break;
      }
      case "poly": {
        const [, p, st, wd, dash, opac] = op;
        ctx.globalAlpha = opac === null ? 1 : opac;
        ctx.strokeStyle = col(st); ctx.lineWidth = wd === null ? 1 : wd;
        ctx.setLineDash(dash || []);
        ctx.beginPath(); ctx.moveTo(p[0], p[1]);
        for (let i = 2; i < p.length; i += 2) ctx.lineTo(p[i], p[i+1]);
        ctx.stroke(); ctx.setLineDash([]); ctx.globalAlpha = 1;
        break;
      }
      case "area": {
        const [, p, fill, opac] = op;
        ctx.globalAlpha = opac === null ? 1 : opac;
        ctx.fillStyle = col(fill);
        ctx.beginPath(); ctx.moveTo(p[0], p[1]);
        for (let i = 2; i < p.length; i += 2) ctx.lineTo(p[i], p[i+1]);
        ctx.closePath(); ctx.fill(); ctx.globalAlpha = 1;
        break;
      }
      case "circle": {
        const [, cx, cy, rad, fill] = op;
        ctx.fillStyle = col(fill); ctx.beginPath();
        ctx.arc(cx, cy, rad, 0, 6.283185307179586); ctx.fill();
        break;
      }
      case "text": {
        const [, x, y, str, anchor, fill, size, weight, rot] = op;
        ctx.fillStyle = col(fill);
        ctx.font = (weight ? weight + " " : "") + (size === null ? 12 : size) +
                   "px -apple-system,sans-serif";
        ctx.textAlign = anchor === "middle" ? "center" : (anchor === "end" ? "right" : "left");
        if (rot) { ctx.save(); ctx.translate(rot[1], rot[2]);
                   ctx.rotate(rot[0] * Math.PI / 180); ctx.translate(-rot[1], -rot[2]); }
        ctx.fillText(str, x, y);
        if (rot) ctx.restore();
        break;
      }
    }
  }
  if (clipped) ctx.restore();
  last = Date.now();
  hud.className = "";
  hud.textContent = `${f.w}\u00d7${f.h}` + (scale < 0.999 ? ` @${(scale*100)|0}%` : "") +
                    ` · ${f.ops.length} ops · ${(performance.now() - t0).toFixed(1)} ms`;
}

// One connection for the life of the page. EventSource reconnects on its own if the
// capture is restarted, so the tab does not have to be.
const es = new EventSource("/events");
es.onmessage = e => paint(JSON.parse(e.data));
es.onerror = () => { hud.className = "stale"; hud.textContent = "disconnected"; };
setInterval(() => {
  if (last && Date.now() - last > 15000) {
    hud.className = "stale";
    hud.textContent = `no frame for ${((Date.now() - last)/1000)|0}s`;
  }
}, 2000);
document.getElementById("el").onclick = () => {
  legendEl.classList.toggle("hide");
  // The legend is in flow, so showing it narrows #wrap. Ask for a relayout at the new
  // width and repaint at once so the change is not a frame late.
  reportSize();
  if (lastFrame) paint(lastFrame);
};
applyMode();
reportSize();
</script>
"""


class PlotServer:
    """Serves the live canvas view and the display list that feeds it.

    Frames are published, not polled: a draw calls publish() and every connected client
    thread wakes on the condition. A client that is mid-write when the next frame lands
    skips it rather than queueing -- the plot is a current-state view, and a backlog of
    stale frames is worse than a dropped one.
    """

    def __init__(self, port, svg_path):
        self.port = port
        self.svg_path = svg_path
        self.cv = threading.Condition()
        self.version = 0
        self.frame = None
        self.httpd = None
        self.bad_frames = 0
        # Set by the page. `width` is the browser's plotting width; `window` overrides
        # --plot-window in seconds (0 = the whole run, None = leave the flag alone). Both
        # are read by the capture loop on its next tick rather than applied here: the
        # server holds no data, and a control that redrew from the server would need a
        # second copy of it.
        self.width = PLOT_WIDTH
        self.window = None
        self.hidden = frozenset()
        # Set by the capture loop to a zero-argument callable that redraws from the latest
        # snapshot. Without it a toggle would not take effect until the next capture --
        # up to --plot-every seconds of a control that appears not to work. Replaced (not
        # called) by the draw path, so it always closes over current data.
        self.redraw = None

    def publish(self, ops, W, H, clip, legend=()):
        payload = {"w": W, "h": H, "clip": clip, "ops": ops, "legend": list(legend),
                   "hidden": sorted(self.hidden)}
        with self.cv:
            payload["v"] = self.version + 1
            try:
                # allow_nan defaults to True and emits bare NaN/Infinity, which are not
                # JSON: the browser rejects the whole frame with "unexpected character at
                # column <somewhere in the middle>", naming an offset that says nothing
                # about which series produced it. Failing here instead names the frame and
                # keeps the number out of the payload.
                blob = json.dumps(payload, separators=(",", ":"), allow_nan=False)
            except ValueError as e:
                self.bad_frames += 1
                if self.bad_frames == 1:
                    print(f"\n  WARNING: plot frame {payload['v']} is not serializable "
                          f"({e}); the live view will hold the previous frame. This is a "
                          f"non-finite coordinate, not a display problem -- report it")
                return
            self.version = payload["v"]
            self.frame = blob.encode()
            self.cv.notify_all()

    def start(self):
        server = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *a):
                pass

            def _send(self, body, ctype):
                self.send_response(200)
                self.send_header("Content-Type", ctype)
                self.send_header("Cache-Control", "no-store")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                if self.path == "/":
                    self._send(PAGE.encode(), "text/html; charset=utf-8")
                elif self.path.startswith("/plot.svg"):
                    try:
                        with open(server.svg_path, "rb") as f:
                            self._send(f.read(), "image/svg+xml")
                    except OSError:
                        self.send_error(404)
                elif self.path.startswith("/control"):
                    # keep_blank_values, or `?hidden=` -- the page's "everything back
                    # on" state -- parses to an EMPTY DICT and silently does nothing.
                    # The one value that has to clear the set is the one parse_qs drops.
                    q = urllib.parse.parse_qs(
                        urllib.parse.urlparse(self.path).query, keep_blank_values=True)
                    try:
                        if "w" in q:
                            # Clamped: a bogus width from a control channel should not be
                            # able to make the process lay out a 200k-pixel plot.
                            server.width = max(420, min(6000, int(float(q["w"][0]))))
                        if "window" in q:
                            wv = float(q["window"][0])
                            server.window = max(0.0, wv)
                        if "hide" in q or "show" in q:
                            h = set(server.hidden)
                            h |= {g for g in q.get("hide", [""])[0].split(",") if g}
                            h -= {g for g in q.get("show", [""])[0].split(",") if g}
                            server.hidden = frozenset(h)
                        if "hidden" in q:      # absolute set, for the page's own state
                            server.hidden = frozenset(
                                g for g in q["hidden"][0].split(",") if g)
                    except (ValueError, IndexError):
                        self.send_error(400); return
                    self._send(json.dumps({"width": server.width,
                                           "window": server.window,
                                           "hidden": sorted(server.hidden)}).encode(),
                               "application/json")
                    # Redraw now rather than at the next capture. Off the handler thread:
                    # a layout can take tens of milliseconds and the control should return
                    # immediately.
                    cb = server.redraw
                    if cb is not None:
                        threading.Thread(target=cb, daemon=True).start()
                elif self.path == "/events":
                    self.send_response(200)
                    self.send_header("Content-Type", "text/event-stream")
                    self.send_header("Cache-Control", "no-store")
                    # An HTTP/1.1 body with neither Content-Length nor chunked framing is
                    # undelimited, and the only legal way to delimit one is by closing the
                    # connection -- so it must say so. Both forms happen to deliver bytes
                    # against CPython's own handler here (checked), but "keep-alive" on an
                    # unframed body is a protocol lie, and the client that eventually reads
                    # it is not guaranteed to be this one.
                    self.send_header("Connection", "close")
                    self.close_connection = True
                    self.end_headers()
                    seen = 0
                    try:
                        while True:
                            with server.cv:
                                # A reconnecting page must not stare at a blank canvas
                                # until the next capture, so the current frame is sent
                                # immediately when there is one.
                                if server.version == seen:
                                    server.cv.wait(timeout=10.0)
                                frame, seen = server.frame, server.version
                            if frame is None:
                                self.wfile.write(b": ping\n\n")
                            else:
                                self.wfile.write(b"data: " + frame + b"\n\n")
                            self.wfile.flush()
                    except (BrokenPipeError, ConnectionResetError, OSError):
                        pass
                else:
                    self.send_error(404)

        class Server(ThreadingHTTPServer):
            daemon_threads = True
            allow_reuse_address = True

        self.httpd = Server(("127.0.0.1", self.port), Handler)
        self.port = self.httpd.server_address[1]
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()
        return self.port


def _load_probe_bias_ns():
    """Analyser zero error, in ns, from scripts/probe-cal.py. 0 when uncalibrated.

    Kept OUT of the CSV schema deliberately: the file records what the rig measured after
    correction, and a schema change would invalidate every capture running across it. The value is
    printed at startup instead, so a corrected run is never mistaken for an uncorrected one.
    """
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "probe-cal.json")
    try:
        with open(path) as f:
            cal = json.load(f)
    except (OSError, ValueError):
        return 0.0, None
    return float(cal.get("bias_us", 0.0)) * 1000.0, cal


PROBE_BIAS_NS, PROBE_CAL = _load_probe_bias_ns()

# How far the offset may sit from frame_lag * frame_period before the capture is rejected as
# mis-locked. Three frames is ~15x the measured MAD of that residue and still ~120x smaller than
# the mis-lock it exists to catch, so it cannot plausibly reject a real reading.
MAX_LAG_RESIDUE_FRAMES = 3.0
# Accepted values behind the continuity reference. Long enough that a handful of ambiguous
# blocks cannot move the median, short enough to follow a genuine slew: at ~45 rows/s this is
# well under a second, while the true skew moves nanoseconds in that time.
REF_WINDOW = 15
# Consecutive consistency rejections before the reference is rebuilt from the correlation.
REF_RESEED_AFTER = 3
# How far this capture's frame count may sit from the running median before the rebuild declines
# to touch it. Inside this, a differing k is ambiguity; beyond it, it may be a real step and the
# jump gate decides.
FRAME_REBUILD_MAX = 4
# Above this margin (coef - rival) the capture's own frame count is trusted and the whole
# disambiguation machinery is skipped. It exists only for stimuli where the runner-up lag is a
# near-tie; with a sharp-autocorrelation stimulus it has nothing to fix and, measured, actively
# destroys the answer -- see the A/B in the commit that added this.
DISAMBIG_MARGIN = 0.50


SCHEMA = ("elapsed_s,unix_s,offset_ns,ppm,pcm_coef,frame_lag,rival,scatter_ns,"
          "fs_a_hz,fs_b_hz,phase_a_us,phase_b_us,ramp_a_ppm,ramp_b_ppm,"
          "crystal_a_ppm,crystal_b_ppm,dl_err_a_us,dl_err_b_us,dl_diff_us,"
          "trim_a_ppm,trim_b_ppm,int_a_ppm,int_b_ppm,"
          "rgate_a_us,rgate_b_us,reason")
# trim_*/int_* (2026-08-30): the DAC trim the delay loop commands and its integral, nearest-in-time
# like dl_err. Appended BEFORE reason only; every pre-existing column keeps its index. These are
# the columns that let corr(fs_diff, trim_diff) decide whether the differential rate wander is
# commanded by the loop or arises downstream of it (PLAN-sub-microsecond R6.3).
# The firmware columns are HELD from that board's most recent log line, not resampled: the
# firmware emits these far slower than rows arrive, so the same value repeats across many rows
# and is empty until the first line arrives.
#
# tsflocal is here because it is the only OUTSIDE-THE-LOOP rate reference the device has: each
# board's local clock against the radio timebase, so its DIFFERENCE between two boards is their
# crystal difference. Measured at -5.347 ppm, which accounts for the whole of the differential
# trim's constant offset from the true rate -- 505 us per 100 s of integrated error down to 17.
# Held rather than interpolated for the same reason as the rest: it steps every 4 s on the
# device (RATE_WINDOW_US) and inventing values between those steps would fabricate resolution
# the measurement does not have.
HELD_COLS = (("a", "ramp_ppm"), ("b", "ramp_ppm"),
             ("a", "crystal_ppm"), ("b", "crystal_ppm"))
# phase_us is NO LONGER HELD (2026-08-30): held, it printed a run-start constant on every row for
# two hours as if it were a per-row measurement -- the sentinel-as-a-number failure inside the
# instrument of record, and it nearly discredited a real finding (PLAN-sub-microsecond R6.1).
# It is now nearest-in-time like dl_err, blanking honestly when no line is close.
PHASE_MATCH_S = 5.0  # the Render phase line ticks ~1 Hz; 5 s tolerates a stall without holding forever

# The delay-loop error is NEAREST-in-time, not held, and that is the point. It ticks about
# once a second (measured: median 1.048 s, p90 1.341 s, longest gap 27 s), which is close
# enough to the capture rate that a real pairing exists most of the time and dishonest to
# fake when it does not. Holding the last value across a 27 s dropout would put a stale
# number beside a fresh measurement and invite exactly the comparison it cannot support.
#
# 0.7 s is derived from that cadence, not picked. Measured on a.log/b.log at a 1 Hz row
# cadence, with BOTH boards required to pair:
#
#     tol 0.35 s -> 26.7%    tol 0.70 s -> 73.3%
#     tol 0.50 s -> 54.8%    tol 1.00 s -> 73.7%
#
# 0.7 s is the knee. Going to 1.0 s buys 0.4 points, because what is left unpaired is not
# marginal timing but real dropouts -- the longest gap in the log is 27 s -- and widening
# the tolerance further would start pairing ACROSS those rather than through them.
#
# Note 73%, not the ~99% an interval count suggests: 99.3% of INTERVALS are under 2*tol,
# but a handful of long gaps hold a large share of the elapsed time, and both boards have
# to land at once (~0.86 each, ~0.73 jointly). The interval figure is time-blind and was
# the wrong statistic for a per-row fill rate.
DL_MATCH_S = 0.7


def nearest_value(series, t, tol=DL_MATCH_S):
    """Value of `series` nearest in time to t, or None if nothing is within tol.

    Returns None rather than the closest-whatever-the-distance so that "no sample" and
    "a sample that happens to be far away" cannot be confused downstream -- an empty CSV
    cell is a fact, a stale one is a fabrication.
    """
    if not series:
        return None
    i = bisect.bisect_left(series, (t,))
    best = None
    for j in (i - 1, i):
        if 0 <= j < len(series):
            d = abs(series[j][0] - t)
            if d <= tol and (best is None or d < best[0]):
                best = (d, series[j][1])
    return None if best is None else best[1]


DUMP_SCHEMA = "elapsed_s,skew_ns"


def load_column(path, name):
    """[(elapsed_s, value)] for one named CSV column; empty if the file lacks it.

    Older CSVs predate some columns, so a replot of one simply omits that trace rather
    than failing.
    """
    out, cols = [], None
    if not os.path.exists(path):
        return out
    for line in open(path):
        if line.startswith("elapsed_s"):
            cols = line.strip().split(",")
            continue
        if line[:1] in "#e" or cols is None or name not in cols:
            continue
        f = line.strip().split(",")
        try:
            v = float(f[cols.index(name)])
            if math.isfinite(v):
                out.append((float(f[0]), v))
        except (ValueError, IndexError):
            pass
    return out


def load_rates(path):
    """(rate_a, rate_b) from a CSV that carries the fs columns; empty if it does not.

    Older files predate those columns, so replotting one simply shows no rate panels
    rather than failing.
    """
    ra, rb = [], []
    if not os.path.exists(path):
        return ra, rb
    cols = None
    for line in open(path):
        if line.startswith("elapsed_s"):
            cols = line.strip().split(",")
            continue
        if line[:1] in "#e" or cols is None:
            continue
        f = line.strip().split(",")
        if "fs_a_hz" not in cols or len(f) < len(cols) - 1:
            return [], []
        ia, ib, it = cols.index("fs_a_hz"), cols.index("fs_b_hz"), 0
        try:
            t = float(f[it])
            for idx, dst in ((ia, ra), (ib, rb)):
                v = float(f[idx])
                if math.isfinite(v) and v > 0:
                    dst.append((t, v))
        except (ValueError, IndexError):
            pass
    return ra, rb


def load_existing(path, for_append=False):
    """Rows from a previous run, but only if the file is really ours.

    Returns (elapsed, offsets_ns, anchor) where anchor is the wall-clock time that
    row 0's elapsed=0 corresponded to -- needed so an appended run continues the same
    time axis, with the real gap between runs visible rather than collapsed.
    """
    ts, ys, anchor = [], [], None
    if not os.path.exists(path):
        # Same arity as the normal return (R7.4): short by one, a missing --out crashed the
        # caller's unpacking instead of starting fresh.
        return ts, ys, anchor
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
    if PROBE_CAL:
        print(f"probe calibration: subtracting {PROBE_CAL.get('bias_us', 0.0):+.2f} us "
              f"(measured {PROBE_CAL.get('when', '?')}, {PROBE_CAL.get('method', '?')})"
              + (f", rig noise MAD {PROBE_CAL['noise_mad_us']:.2f} us" if "noise_mad_us" in PROBE_CAL else ""))
    else:
        print("probe calibration: NONE -- absolute offsets carry the rig's zero error "
              "(~25 us measured once); run scripts/probe-cal.py")
    if head and head != SCHEMA:
        # PREFIX HEADERS ARE READABLE (R7.1): columns are append-only before `reason`, so an
        # older file's header is the current SCHEMA minus a tail. Hard-exiting made every
        # historical capture unreplottable the moment a column was added -- including the file
        # the current baselines live in -- contradicting the stated contract ("a replot of one
        # simply omits that trace"). Reading only needs the shared prefix; appending to a
        # prefix-header file mixes row widths, so warn rather than allow it silently.
        old_cols = head.rstrip().rstrip(",").split(",")
        new_cols = SCHEMA.split(",")
        old_body = [c for c in old_cols if c != "reason"]
        if old_body == new_cols[: len(old_body)]:
            if for_append:
                # R9.6: reading a prefix header is fine; APPENDING to one writes 24-field rows
                # into a 20-field file -- DictReader shunts the tail into restkey and positional
                # readers get trim_a_ppm where reason was. Hard stop, as before the R7.1 fix.
                sys.exit(f"{path} has an older column layout; appending would mix row widths.\n"
                         f"Pass a fresh --out (or drop --append to replace it).")
            print(f"  {path}: older schema ({len(old_cols)} cols); absent columns read as "
                  f"absent. Appending will MIX ROW WIDTHS -- prefer a fresh --out.")
        else:
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


def rate_derivative(series, window):
    """d(rate)/dt in Hz/s, by least squares over a sliding window.

    Differencing consecutive points would be dominated by measurement noise: each rate
    comes from a regression over one capture, good to ~0.04 Hz, which across a 16.7 ms
    block is ~2 Hz/s of noise for a quantity that moves far slower. Fitting a slope over
    a window of points trades time resolution for a derivative that means something.
    """
    # Drop non-finite points BEFORE the prefix sums. cumsum past a NaN is NaN for every
    # window after it, so one failed capture -- a row printing "--", best coef 0.00 -- took
    # out the rest of the panel: measured, a single NaN at sample 50 of 200 left 155 of 200
    # output slopes non-finite. In the SVG those became `nan` coordinates, which render as
    # nothing, so the panel simply stopped and looked like an axis that ran out of data.
    # The `ok` mask below cannot catch it: it tests the fit's denominator, not its inputs.
    series = [(x, v) for x, v in series if math.isfinite(x) and math.isfinite(v)]
    if len(series) < 3:
        return [], window
    xs = np.array([x for x, _ in series])
    ys_ = np.array([v for _, v in series])
    # The window must actually contain points to fit. Asked for 1 s at 1 row/s it held
    # one, and the panel silently vanished; widen to at least five samples' worth.
    spacing = float(np.median(np.diff(xs))) if xs.size > 1 else 0.0
    window = max(window, 5 * spacing)
    # Least squares over every window at once, from prefix sums: slope is
    # (n*Sxy - Sx*Sy) / (n*Sxx - Sx^2), and each window's sums are a difference of two
    # prefixes. Exact for non-uniform spacing, and O(n) instead of n polyfits.
    lo = np.searchsorted(xs, xs - window / 2.0, side="left")
    hi = np.searchsorted(xs, xs + window / 2.0, side="right")
    z = lambda a: np.concatenate(([0.0], np.cumsum(a)))
    Sx, Sy = z(xs), z(ys_)
    Sxy, Sxx = z(xs * ys_), z(xs * xs)
    cnt = (hi - lo).astype(np.float64)
    sx, sy = Sx[hi] - Sx[lo], Sy[hi] - Sy[lo]
    sxy, sxx = Sxy[hi] - Sxy[lo], Sxx[hi] - Sxx[lo]
    den = cnt * sxx - sx * sx
    ok = (cnt >= 3) & (np.abs(den) > 0) & np.isfinite(den) & np.isfinite(sxy) & np.isfinite(sy)
    slope = np.where(ok, (cnt * sxy - sx * sy) / np.where(ok, den, 1.0), np.nan)
    return [(float(x), float(v)) for x, v in zip(xs[ok], slope[ok])], window


def build_panels(args, rate_a, rate_b, temps, lo, hi, dtrim=None,
                 skew=None, ppm_series=None):
    """Extra plot panels: A-B rate, temperature, absolute I2S rate and its derivative,
    differential trim, firmware ramp."""
    panels = []
    win = lambda ser: [(x, v) for x, v in ser if lo <= x <= hi]

    # How fast the A-B offset is changing -- the derivative of the panel above, in ppm.
    # Three independent routes to the same quantity, which is the point of putting them on
    # one axis: they should agree, and where they do not, one of them is wrong.
    ab, ab_sids = {}, {}
    if skew:
        d, w = rate_derivative(win(skew), args.rate_window)
        # skew is ns against seconds, so the slope is ns/s -- ppm is that over 1000.
        if d:
            # The window is in the LABEL, never in the id: it moves with the sample
            # cadence, and an id that moves is a toggle that stops working.
            ab_sids[f"d(skew)/dt, {w:.2g}s fit"] = "dskew_dt"
            ab[f"d(skew)/dt, {w:.2g}s fit"] = [(x, v / 1000.0) for x, v in d]
    if ppm_series:
        p = win(ppm_series)
        if p:
            ab["per-capture slope"] = p
    ra_, rb_ = win(rate_a), win(rate_b)
    if ra_ and rb_:
        # Independent of the offset entirely: each board's own rate, differenced. Paired by
        # nearest timestamp because the two series are sampled on the same rows anyway.
        bx = np.array([x for x, _ in rb_])
        bv = np.array([v for _, v in rb_])
        idx = np.clip(np.searchsorted(bx, [x for x, _ in ra_]), 0, bx.size - 1)
        diff = [(x, (bv[j] - v) / v * 1e6) for (x, v), j in zip(ra_, idx) if v > 0]
        if diff:
            ab["fs_b - fs_a"] = diff
    if ab:
        panels.append(("A-B rate of change (ppm)", ab, "ab_rate", ab_sids))

    t2 = trim_temps(temps, lo, hi)
    if t2:
        panels.append(("temperature (\u00b0C)", t2, "temp", None))
    ra, rb = win(rate_a), win(rate_b)
    if ra or rb:
        panels.append(("I2S frame rate (Hz)", {"a rate": ra, "b rate": rb}, "fs", None))
        da, wa = rate_derivative(ra, args.rate_window)
        db, wb = rate_derivative(rb, args.rate_window)
        if da or db:
            panels.append((f"d(rate)/dt (Hz/s), {max(wa, wb):.2g}s fit",
                           {"a d/dt": da, "b d/dt": db}, "drate_dt", None))
    # The boards' own view of the differential rate, on the same time axis as the offset
    # above it: this is the quantity whose integral the offset is supposed to be, so the
    # two panels should be visibly derivative and integral of one another.
    if dtrim:
        dt = win(dtrim)
        if dt:
            panels.append(("differential trim mean (ppm)", {"b - a": dt}, "dtrim", None))
    # What each board BELIEVES its offset from the group is, next to the measured ppm.
    # Same units, so it goes on one panel and any disagreement is read directly.
    ramp = {f"{b} ramp": win(FIRMWARE.get((b, "ramp_ppm"), [])) for b in ("a", "b")}
    ramp = {k: v for k, v in ramp.items() if v}
    if ramp:
        panels.append(("firmware offset ramp (ppm)", ramp, "ramp", None))
    # tsf-local: each board's clock against the RADIO timebase, so it is measured outside the
    # audio servo loop -- the only reference here that is. Its own panel rather than sharing
    # with the ramp above: these sit around +42 and +37 ppm where the ramp sits near 0, so one
    # shared axis would flatten the ramp. The useful quantity is the SEPARATION between the two
    # boards, which is their crystal difference (-5.35 ppm measured), so both are drawn.
    tsfl = {f"{b} crystal delta": win(FIRMWARE.get((b, "crystal_ppm"), [])) for b in ("a", "b")}
    tsfl = {k: v for k, v in tsfl.items() if v}
    if tsfl:
        panels.append(("crystal delta vs group (ppm)", tsfl, "crystal", None))
    return panels


def firmware_overlays(lo, hi, which=("phase",)):
    """The firmware's own offset estimates, in ns, for drawing over the measured skew.

    Both are the same quantity this script measures on the wire, which is the whole point:
    every firmware timing metric is computed against that device's own predicted playout,
    so a modelling error moves the audio and the metric together and reads as zero. Drawn
    on the skew panel rather than below it because they are directly comparable -- if
    belief and measurement diverge, the gap is the blind spot.
    """
    # Selectable because the two series live on different scales: render phase sits at a
    # median of -41 us, comparable with the wire, while depth-vs-group medians are +12 ms
    # and -6.6 ms. Since the axis now expands to fit whatever is overlaid, including depth
    # by default would stretch the range 300x and flatten the measurement to a line.
    out = {}
    for board in ("a", "b"):
        for sel, key, label in (("phase", "phase_us", "render phase"),
                                ("depth", "depth_us", "depth vs group")):
            if sel not in which:
                continue
            pts = [(x, v * 1000.0) for x, v in FIRMWARE.get((board, key), [])
                   if lo <= x <= hi]
            if pts:
                out[f"{board} {label}"] = pts
    # The delay loop's own error difference, on the measured axis. This is the one the wire
    # actually tracks -- r = 0.88, ~2.5 us bias, ~13 us/s noise -- where render phase is
    # biased by tens of microseconds, 3-4x noisier and carries stall-stamp spikes. Same
    # sign convention as the measurement, which is board B minus board A on the wire and
    # errA - errB here: the loop's error is what it must still REMOVE, so a board running
    # late has a positive error and appears late on the wire.
    if "dl" in which:
        pts = [(x, v * 1000.0) for x, v in DL_DIFF if lo <= x <= hi]
        if pts:
            out["delay-loop err a-b"] = pts
    return out


def split_gaps(pts):
    """Splits a series at gaps, so a polyline is never drawn across missing data.

    A dropout is not a measurement of zero drift, but a single polyline through the
    surviving points says exactly that: the line from the last sample before a gap to the
    first one after it looks like a smooth glide at whatever slope the endpoints imply. A
    boot does this every time -- ~25 s with no audio to correlate, so ``pcm_coef`` is 0 and
    the offset is NaN -- and it was read off the plot as a real 40 ppm ramp before anyone
    checked the CSV. Breaking the line makes a gap look like a gap.

    The threshold is derived from the data rather than fixed, because the sample cadence
    depends on --samples: ten times the median spacing, floored at half a second so ordinary
    scheduling jitter never splits a run.
    """
    if len(pts) < 3:
        return [pts] if pts else []
    gaps = sorted(pts[i + 1][0] - pts[i][0] for i in range(len(pts) - 1))
    med = gaps[len(gaps) // 2]
    limit = max(10 * med, 0.5) if med > 0 else 0.5
    segs, cur = [], [pts[0]]
    for prev, nxt in zip(pts, pts[1:]):
        if nxt[0] - prev[0] > limit:
            segs.append(cur)
            cur = []
        cur.append(nxt)
    segs.append(cur)
    return [sg for sg in segs if sg]


def stats_caption(ts, ys):
    """One-line summary for the plot header: spread first, slope last."""
    v = [y for y in ys if math.isfinite(y)]
    if not v:
        return ""
    parts = [f"n={len(v)}", f"mean {np.mean(v)/1000:+.3f} us",
             f"sd {np.std(v)/1000:.3f} us",
             f"p2p {(max(v)-min(v))/1000:.3f} us"]
    # Slope over the LONGEST CONTIGUOUS SEGMENT, never across a gap. Fitted across one it
    # measures the interpolation: a boot with -6 ms before it and -14 us after reads as a
    # tidy +40 ppm that nothing in the audio did.
    segs = split_gaps([(t, y) for t, y in zip(ts, ys) if math.isfinite(y)])
    if segs:
        seg = max(segs, key=len)
        f = fit_slope([t for t, _ in seg], [y for _, y in seg])
        if f:
            span = seg[-1][0] - seg[0][0]
            note = "" if len(segs) == 1 else f" over {span:.0f}s of {len(segs)} runs"
            parts.append(f"slope {f[0]:+,.1f} ns/s ({f[0]/1000:+.4f} ppm){note}")
    return "   ".join(parts)


# Pairing tolerance between two boards' report streams. Each runs its window on its own phase,
# so half a window is the most that can be demanded without discarding good rows. Derived from
# the observed cadence rather than fixed, because the trim line moved from 3.35 s to 1 s and a
# constant tuned for the old cadence silently changes meaning at the new one.
PAIR_WINDOW_S = 1.8
# A window whose reported AUDIO time falls short of the wall clock since the previous report
# did not spend that gap playing continuously -- a starvation, a stall, a re-baseline. The
# integral has no idea what happened in the missing time, so the segment breaks there.
#
# RELATIVE, not absolute. At the old 3.35 s cadence 0.5 s was a 15% tolerance; at the 1 s
# cadence the same constant is 50%, which would let a 400 ms starvation through unbroken -- and
# catching exactly that is what makes the fit honest. Expressed as a fraction of the interval
# with a small absolute floor for scheduling jitter, so it keeps its meaning if the cadence
# moves again.
AUDIO_SHORTFALL_FRAC = 0.15
AUDIO_SHORTFALL_FLOOR_S = 0.05
# Below this a segment's correlation is noise. The finding's own runs were 92-499 s, i.e.
# 28-150 windows.
MIN_FIT_ROWS = 10


def nearest_at(series, t):
    """Nearest (t, ...) row in a time-sorted series, or None if empty."""
    if not series:
        return None
    lo, hi = 0, len(series) - 1
    while lo < hi:
        mid = (lo + hi) // 2
        if series[mid][0] < t:
            lo = mid + 1
        else:
            hi = mid
    best = series[lo]
    if lo > 0 and abs(series[lo - 1][0] - t) < abs(best[0] - t):
        best = series[lo - 1]
    return best


def pair_diff(a, b, to_ppm=None):
    """Pair two per-board series on time and return [(t, differential)] as b - a.

    to_ppm converts the paired values to ppm when they are not already: the fs columns are
    absolute Hz, so their difference is scaled by the pair's own mean rather than by an
    assumed 44.1 or 48 kHz.
    """
    out = []
    for t, va in a:
        hit = nearest_at(b, t)
        if hit is None or abs(hit[0] - t) > PAIR_WINDOW_S:
            continue
        vb = hit[1]
        if va is None or vb is None:
            continue
        out.append((t, to_ppm(va, vb) if to_ppm else vb - va))
    return out


def trim_diff(a, b):
    """([(t, dppm)], breaks) from two boards' trim-window series, as b - a.

    Rows are (t, mean_ppm | None, audio_s). A break is recorded wherever the integral goes
    blind: either board reporting no programmed trim, or either board reporting less AUDIO
    time than the wall clock since its previous window -- a starvation, stall or re-baseline
    means the trim in the missing time is unknown, and joining across it would read the
    interpolation rather than the data.
    """
    out, breaks = [], []
    prev_t = None
    # A dropped window cannot carry the break itself: it contributes no differential point,
    # so a break recorded at ITS timestamp would match no row and split nothing. Carry the
    # break forward to the next row that is actually emitted.
    pending = False
    for t, mean_a, audio_a in a:
        hit = nearest_at(b, t)
        if hit is None or abs(hit[0] - t) > PAIR_WINDOW_S:
            prev_t, pending = None, True
            continue
        _tb, mean_b, audio_b = hit
        if mean_a is None or mean_b is None:
            prev_t, pending = None, True
            continue
        if prev_t is not None:
            dt = t - prev_t
            slack = max(AUDIO_SHORTFALL_FRAC * dt, AUDIO_SHORTFALL_FLOOR_S)
            if dt <= 0 or min(audio_a, audio_b) < dt - slack:
                pending = True
        if pending:
            breaks.append(t)
            pending = False
        out.append((t, mean_b - mean_a))
        prev_t = t
    return out, breaks


def integral_fit(dppm, offset_us, breaks=()):
    """Integrate a differential rate and fit the result against the measured wire offset.

    The wire offset IS the integral of the differential achieved rate: integrating the
    analyser's own fs columns reproduces it at corr -0.997..-1.000, slope -1.0, residual sd
    0.14-0.38 us. This runs that comparison for any differential-ppm series, so the same
    check can be pointed at the boards' own reported trim -- the question of whether a
    device can see its own offset without an analyser.

    Sign: positive trim plays faster, so board B running fast renders a given frame EARLIER
    and the CSV's "B minus A, positive means B later" goes DOWN. The expected slope is -1.

    Units are free: 1 ppm sustained for 1 s is exactly 1 us, so ppm*s IS microseconds.

    Segments never span a gap or a break. Fitted across one, the integral measures the
    interpolation -- the same failure that made a post-boot hole read as a tidy 40 ppm ramp.
    Yields one result dict per segment long enough to mean anything.
    """
    if len(dppm) < 2:
        return
    brk = set(breaks)
    # Gap-split on the differential's own cadence, then split again at explicit breaks
    # (a window that did not account for its wall clock as played audio).
    segs = []
    for seg in split_gaps(dppm):
        cur = []
        for row in seg:
            if row[0] in brk and cur:
                segs.append(cur)
                cur = []
            cur.append(row)
        if cur:
            segs.append(cur)
    for seg in segs:
        # Running integral. The first row defines the origin and contributes no interval;
        # the arbitrary constant is absorbed by the fit's intercept.
        integral, xs, ys = 0.0, [], []
        prev_t = seg[0][0]
        for t, dv in seg:
            integral += dv * (t - prev_t)
            prev_t = t
            hit = nearest_at(offset_us, t)
            if hit is None or abs(hit[0] - t) > PAIR_WINDOW_S:
                continue
            xs.append(integral)
            ys.append(hit[1])
        if len(xs) < MIN_FIT_ROWS:
            continue
        x = np.array(xs)
        y = np.array(ys)
        if np.ptp(x) < 1e-12 or np.ptp(y) < 1e-12:
            continue
        slope, icept = np.polyfit(x, y, 1)
        resid = y - (slope * x + icept)
        off_sd = float(np.std(y))
        resid_sd = float(np.std(resid))
        yield {
            "span_s": seg[-1][0] - seg[0][0],
            "n": len(xs),
            "corr": float(np.corrcoef(x, y)[0, 1]),
            "slope": float(slope),
            "off_sd": off_sd,
            "resid_sd": resid_sd,
            # How much of the measured offset's spread the fit removes, reported the way
            # the fs-column result was. sd is defensible for a fit residual: the
            # medians-not-sd rule is about network events dominating a raw field.
            "explained": 100.0 * (1.0 - resid_sd / off_sd) if off_sd > 0 else float("nan"),
            "dppm_sd": float(np.std([d for _, d in seg])),
        }


def report_integral(label, dppm, offset_us, breaks=(), expect=None):
    """Print integral_fit's per-segment table. Returns True if anything was fitted."""
    rows = list(integral_fit(dppm, offset_us, breaks))
    if not rows:
        print(f"   {label}: no segment of {MIN_FIT_ROWS}+ paired rows to fit "
              f"({len(dppm)} differential point(s))")
        return False
    print(f"   {label}: {len(rows)} segment(s), expected slope -1.0")
    print(f"      {'span_s':>7s} {'n':>4s} {'d_sd_ppm':>9s} {'off_sd_us':>10s}"
          f" {'corr':>7s} {'slope':>7s} {'resid_us':>9s} {'expl':>6s}")
    for r in rows:
        print(f"      {r['span_s']:7.1f} {r['n']:4d} {r['dppm_sd']:9.3f} {r['off_sd']:10.2f}"
              f" {r['corr']:+7.3f} {r['slope']:+7.3f} {r['resid_sd']:9.2f}"
              f" {r['explained']:5.0f}%")
    if expect:
        print(f"      {expect}")
    return True


def window_means(dfs, windows):
    """Mean of a dense differential-rate series over each report window.

    The analyser measures a rate per capture (tens of Hz); a board reports one time-mean per
    ~3.3 s window. Comparing them needs the dense series averaged over the SAME interval the
    board averaged, which is [t - audio_s, t] -- the window's own audio duration, which is
    why the firmware publishes it. Windows without enough coverage are dropped rather than
    compared against a partial average.
    """
    out, thin = [], 0
    for t, val, audio_s in windows:
        if val is None:
            continue
        seg = [v for ts_, v in dfs if t - audio_s <= ts_ <= t]
        # Three is the floor at which a mean is a mean rather than a sample. The analyser
        # normally supplies ~40 per window, so this only bites on a sparse capture -- and it
        # is counted rather than dropped quietly, because "no windows compared" and "windows
        # compared and they agreed" must not look the same.
        if len(seg) < 3:
            thin += 1
            continue
        out.append((t, val, float(np.mean(seg))))
    return out, thin


def report_rate_reference(label, paired):
    """How good is a candidate rate reference, measured against the analyser's own rates?

    This is the question that decides whether a device can know its own offset. The offset is
    the integral of the differential rate, so a reference is only usable to the extent that
    (a) its CONSTANT offset from the true rate is known, and (b) what is left over is small.
    Both are reported in the units that matter -- ppm, and what that ppm integrates to.

    A high correlation is NOT sufficient and is the trap here: the trim tracks the true rate
    closely while sitting a few ppm away from it, because each board's trim cancels its OWN
    crystal error, so the differential trim carries the crystal DIFFERENCE. That constant is
    unobservable on-device and integrates without bound.
    """
    paired, thin = paired
    thin_note = f", {thin} window(s) had too few analyser samples to average" if thin else ""
    if len(paired) < MIN_FIT_ROWS:
        print(f"   {label}: {len(paired)} paired window(s), need {MIN_FIT_ROWS}{thin_note}")
        return False
    x = np.array([p[1] for p in paired])      # candidate reference
    y = np.array([p[2] for p in paired])      # analyser's own differential rate
    bias = float(np.mean(x) - np.mean(y))
    if np.ptp(x) < 1e-12 or np.ptp(y) < 1e-12:
        print(f"   {label}: reference or rate is constant, nothing to fit")
        return False
    corr = float(np.corrcoef(x, y)[0, 1])
    slope, icept = np.polyfit(x, y, 1)
    resid_sd = float(np.std(y - (slope * x + icept)))
    print(f"   {label}: {len(paired)} window(s){thin_note}")
    print(f"      corr {corr:+.3f}   slope {slope:+.3f}   "
          f"constant offset {bias:+.3f} ppm   residual {resid_sd:.3f} ppm")
    # What each error term costs the OFFSET, which is the only thing that matters. The
    # constant is the killer: it is a rate, so it integrates linearly and forever.
    print(f"      integrated over 100 s: constant {abs(bias) * 100:.0f} us, "
          f"residual {resid_sd * 100:.0f} us")
    return True


def report_recovery(ts, ys):
    """Time for the offset to settle after a boot or a re-lock, per contiguous segment.

    The servo gain trades steady-state skew against exactly this, so it is half of every
    TRIM_KP decision -- and it was being read off plots by eye, which is how "~42 s" and
    "~135 s" entered the notes without a method attached.

    A recovery is a segment that STARTS far out and decays, which is what a boot looks like:
    ~25 s of no audio to correlate, then playout resuming with an offset planted by the
    re-baseline anchor. Segments that merely continue at the floor are not recoveries and are
    skipped, so this prints nothing on an ordinary quiet capture rather than inventing an
    event.

    Both numbers are reported against the segment's OWN settled level, not against zero: the
    absolute offset carries whatever the anchor planted, and the question here is how fast the
    loop converges, not where it converges to.
    """
    pts = [(t, y / 1000.0) for t, y in zip(ts, ys) if math.isfinite(y)]
    if len(pts) < 40:
        return
    printed = False
    for seg in split_gaps(pts):
        if len(seg) < 40:
            continue
        tail = [v for _t, v in seg[int(len(seg) * 0.8):]]
        settled = float(np.median(tail))
        # Tolerance from the tail's own scatter, so a quieter floor demands a tighter
        # settle rather than being graded against a constant from another era. MAD, not sd:
        # network events dominate sd here by ~30x.
        mad = float(np.median([abs(v - settled) for v in tail]))
        tol = max(4.0 * mad, 5.0)
        start_err = abs(seg[0][1] - settled)
        # Not a recovery unless it began meaningfully outside the band it ends in.
        if start_err < 4.0 * tol:
            continue
        t0 = seg[0][0]
        # Settled = the first instant after which it never leaves the band again. "First
        # entry" would report the first overshoot crossing, which on a decaying oscillation
        # is far too optimistic.
        settle_t = None
        for i in range(len(seg)):
            if all(abs(v - settled) <= tol for _t, v in seg[i:]):
                settle_t = seg[i][0] - t0
                break
        # 1/e of the initial excursion: comparable to the tau values already in the notes.
        tau = next((t - t0 for t, v in seg if abs(v - settled) <= start_err / math.e), None)
        if not printed:
            print("   recovery (offset settling after a boot or re-lock)")
            printed = True
        print(f"      t={t0:8.1f}s  from {seg[0][1] - settled:+8.1f} us  "
              f"tau {('%.0f s' % tau) if tau is not None else '   --':>6s}  "
              f"settled {('%.0f s' % settle_t) if settle_t is not None else 'not yet':>8s}  "
              f"(band +-{tol:.1f} us, floor MAD {mad:.2f} us)")


def measure_step(pts, t0, settle_s=4.0):
    """(before, after, step, floor) at t0, or None when either side is too thin.

    Each side is fitted and EXTRAPOLATED to t0 rather than summarised by a median, because
    differencing medians across the gap turns any ramp into an apparent step -- the windows are
    centred a window apart, so a mere 3.5 us/s slope reads as +6.8 us of "step". Fitting measures
    the DISCONTINUITY, which is the only thing that separates "something displaced the audio here"
    from "it was already moving through here".
    """
    before = [(t, v) for t, v in pts if t0 - settle_s <= t < t0]
    after = [(t, v) for t, v in pts if t0 < t <= t0 + settle_s]
    if len(before) < 5 or len(after) < 5:
        return None

    def edge(seg):
        tt = np.array([t for t, _v in seg])
        vv = np.array([v for _t, v in seg])
        if np.ptp(tt) < 1e-9:
            return float(np.median(vv)), 0.0
        sl, ic = np.polyfit(tt, vv, 1)
        return float(sl * t0 + ic), float(np.std(vv - (sl * tt + ic)))

    b_val, b_res = edge(before)
    a_val, a_res = edge(after)
    # Floor from the fit residuals, so "step" means bigger than the scatter around the local
    # trend rather than bigger than the scatter including it.
    return b_val, a_val, a_val - b_val, max(min(b_res, a_res), 0.05)


def report_repair_steps(ts, ys):
    """Does an accounting-split REPAIR displace the audio?

    The repair fires on a split that has held for DRIFT_REPAIR_HOLD_US (3 s). For those 3 s the
    servo was steering real audio against a prediction wrong by the split, so the audio may already
    have moved -- and the repair then corrects the ACCOUNTING, which makes that displacement
    invisible to every on-device metric afterwards. Only the wire can say whether it happened.

    Measured 23 times across two logs on splits from 4.7 to 57 ms, so if each one displaces audio
    this is a far more frequent source of planted offsets than re-baselines are.
    """
    pts = [(t, y / 1000.0) for t, y in zip(ts, ys) if math.isfinite(y)]
    if not pts or not REPAIRS:
        return
    printed = False
    for board, rows in sorted(REPAIRS.items()):
        for t0, us in rows:
            m = measure_step(pts, t0)
            if m is None:
                continue
            b_val, a_val, step, floor = m
            if not printed:
                print("   split repairs (does the repair displace the audio?)")
                print(f"      {'board':>5s} {'t':>8s} {'repaired_us':>12s} {'step_us':>9s}"
                      f" {'floor':>7s}")
                printed = True
            verdict = "STEP" if abs(step) > max(6.0 * floor, 5.0) else "no step"
            # Tag repairs that a deliberate injection caused, so a provoked point is never mistaken
            # for a natural one and the two are never averaged together.
            # 90 s, not 12: the split is RAMPED in at ~100 us/s, so the repair fires a ramp
            # duration plus DRIFT_REPAIR_HOLD_US after the request -- 23 s for a 2500 us target,
            # and proportionally longer for a bigger one. A 12 s window silently left provoked
            # repairs untagged and therefore indistinguishable from natural ones.
            inj = [u for t, u in INJECTS.get(board, []) if 0 <= t0 - t <= 90.0]
            tag = f"  <- injected {inj[-1]:+d} us" if inj else ""
            print(f"      {board:>5s} {t0:8.1f} {us:+12d} {step:+9.1f} {floor:7.2f}  {verdict}{tag}")
    if printed:
        print("      a STEP means the repair path displaced real audio, which no on-device metric"
              "\n      reports once the accounting is reconciled. Compare step_us against"
              "\n      repaired_us: they need not match, since the servo had 3 s to act on it.")


def report_seed_steps(ts, ys, settle_s=4.0):
    """Does a re-baseline STEP the wire offset, and by how much?

    The hypothesis: a planted static offset is the per-device error in the `latency` the seed
    anchors to. That error is unobservable on-device -- the servo measures against the very
    prediction it anchors, so it reads ~0 while the audio sits that far off -- so the wire is the
    only witness.

    A step is the signature that distinguishes the two candidate stories, and they want different
    fixes. If the anchor plants the offset, the wire should JUMP at the seed instant. If instead
    the offset arrives because the servo integrates a rate difference to a new resting point, it
    should ramp over the servo's time constant with no step at all. Today's recovery ramped (tau
    80 s, 94% explained by the rate integral), so the step is what needs establishing separately.

    Reported per seed as the offset just before against just after, using medians over a short
    window either side so a single noisy sample cannot invent a step.
    """
    pts = [(t, y / 1000.0) for t, y in zip(ts, ys) if math.isfinite(y)]
    if not pts or not SEEDS:
        return
    printed = False
    for board, rows in sorted(SEEDS.items()):
        for t0, lat, age, frames, aerr in rows:
            before = [v for t, v in pts if t0 - settle_s <= t < t0]
            after = [v for t, v in pts if t0 < t <= t0 + settle_s]
            if len(before) < 5 or len(after) < 5:
                continue
            # EXTRAPOLATED to the seed instant from each side, not differenced medians.
            # Differencing medians turns any ramp into an apparent step, because the two windows
            # are centred a window apart: a synthetic 3.5 us/s ramp with no discontinuity at all
            # read as a +6.8 us "step" that way. Fitting each side and evaluating both at t0
            # measures the DISCONTINUITY, which is the thing that separates "the anchor planted
            # it" from "the servo integrated to a new resting point".
            def edge(seg, lo_t, hi_t):
                tt = np.array([t for t, _v in pts if lo_t <= t <= hi_t])
                vv = np.array(seg)
                if tt.size != vv.size or tt.size < 5 or np.ptp(tt) < 1e-9:
                    return float(np.median(vv)), 0.0
                sl, ic = np.polyfit(tt, vv, 1)
                return float(sl * t0 + ic), float(np.std(vv - (sl * tt + ic)))
            b_med, b_res = edge(before, t0 - settle_s, t0 - 1e-9)
            a_med, a_res = edge(after, t0 + 1e-9, t0 + settle_s)
            # Floor from the fit residuals: "step" must beat the scatter around the local trend,
            # not the scatter including it.
            mad = max(min(b_res, a_res), 0.05)
            step = a_med - b_med
            if not printed:
                print("   seed anchors (does a re-baseline STEP the wire?)")
                print(f"      {'board':>5s} {'t':>8s} {'latency_ms':>11s} {'age_ms':>7s}"
                      f" {'step_us':>9s} {'floor':>7s} {'anchor_err':>11s} {'match':>7s}")
                printed = True
            verdict = "STEP" if abs(step) > max(6.0 * mad, 5.0) else "no step"
            # THE TEST: the on-device anchor error against the step the wire saw. If the anchor's
            # latency error is what plants the offset, these are the same number. Reported per seed
            # rather than as a correlation, because latency itself was near-constant across seeds
            # and correlating against a constant proved worthless.
            if aerr is None:
                ae, match = f"{'--':>11s}", f"{'--':>7s}"
            else:
                ae = f"{aerr:+11d}"
                match = "yes" if abs(step) > 1e-9 and abs(aerr - step) <= max(0.25 * abs(step), 5.0) \
                        else "NO"
                match = f"{match:>7s}"
            print(f"      {board:>5s} {t0:8.1f} {lat/1000.0:11.1f} {age/1000.0:7.1f}"
                  f" {step:+9.1f} {mad:7.2f} {ae} {match}  {verdict}")
    if printed:
        print("      a STEP supports the anchor planting the offset; a ramp with no step means the"
              "\n      servo integrated to a new resting point instead. anchor_err is the SAME"
              "\n      quantity measured on-device, with no prediction in its path -- if the"
              "\n      anchor's latency error is what plants the offset, anchor_err == step_us.")


def report_pre_trigger_drain(board, pre):
    """Why did the ring empty? The half of the episode the armed burst cannot see.

    Every offset-planting event measured so far runs one chain: ring empties -> late playout ->
    resync -> repair -> permanent displacement. Links 2-4 are characterised; this is link 1. The
    armed burst is armed by the threshold crossing, so it opens with the ring already gone.

    Two mechanisms end with an empty ring and want opposite fixes. If chunks STOPPED ARRIVING the
    drain is a supply gap -- radio, server, decode -- and the servo is a victim. If they kept
    arriving while the ring fell, consumption outran an intact supply and the fault is downstream.

    THE DISCRIMINATOR IS THE RING'S SLOPE, NOT THE CADENCE. dt is the servo LOOP's cadence, and
    the loop is fed from a backlog: with a second of audio buffered, it keeps iterating at exactly
    playout rate long after the network has gone silent, so a starved device shows a textbook
    26 ms cadence and no gaps at all. The first capture (13:19:18 on board a) is exactly that --
    perfect cadence, err inside +-9 us, and the ring falling by one chunk per chunk. Believing dt
    there would have read "supply intact" off a total supply outage.

    Playout is real time, so over a window of `span` the sink consumed `span` of audio. Whatever
    the ring gained on top of what it lost is what arrived:

        supplied = span + (ring_end - ring_start)

    A ratio near 1 is an intact supply; near 0 is an outage. Both terms come off the same trace,
    neither depends on the chunk period, and the loop's own pacing cancels out.
    """
    if len(pre) < 8:
        return
    pre = sorted(pre, key=lambda r: r[1])
    rings = [r[4] for r in pre]
    ring_max = max(rings)
    # Chunk cadence from the placed timestamps. Degenerate when device time was unavailable and
    # every sample on a line fell back to the line's own log timestamp; say so rather than
    # reporting a cadence that is an artefact of the fallback.
    dts = [(pre[i][0] - pre[i - 1][0]) * 1000.0 for i in range(1, len(pre))]
    usable = [d for d in dts if d > 0]
    span_ms = (pre[-1][0] - pre[0][0]) * 1000.0
    print(f"      {board}  pre-trigger: ring {rings[0]:5d} -> {rings[-1]:5d} ms over "
          f"{len(pre)} chunks ({span_ms:.0f} ms), peak {ring_max} ms")
    if ring_max <= 0:
        print(f"      {'':>{len(board)}}  -> ring was ALREADY empty a full window before the arm;"
              f" widen RESYNC_PRE_CHUNKS to catch the drain")
        return
    # Drain onset: the last chunk at which the ring still held half its peak. Half rather than
    # the peak itself because the ring breathes by a chunk either way in normal running.
    onset = max((i for i, v in enumerate(rings) if v >= ring_max / 2), default=0)
    onset_ms = (pre[-1][0] - pre[onset][0]) * 1000.0
    print(f"      {'':>{len(board)}}  drain began at seq {pre[onset][1]:+d} "
          f"({onset_ms:.0f} ms before the arm, from {rings[onset]} ms)")
    if span_ms <= 0:
        print(f"      {'':>{len(board)}}  window has no device time; supply ratio unavailable")
        return
    supplied_ms = span_ms + (rings[-1] - rings[0])
    ratio = supplied_ms / span_ms
    print(f"      {'':>{len(board)}}  supply: {supplied_ms:.0f} ms of audio arrived during "
          f"{span_ms:.0f} ms of playout  ratio {ratio:.2f}")
    if ratio < 0.35:
        print(f"      {'':>{len(board)}}  -> SUPPLY STALLED: the ring drained because nothing was"
              f" arriving; the servo is a victim here, not the cause")
    elif ratio < 0.85:
        print(f"      {'':>{len(board)}}  -> SUPPLY SLOW: arriving, but below real time")
    elif rings[-1] < ring_max / 2:
        print(f"      {'':>{len(board)}}  -> SUPPLY INTACT: audio kept arriving at ~real time"
              f" while the ring fell; the loss is DOWNSTREAM of the ring")
    else:
        print(f"      {'':>{len(board)}}  -> inconclusive: supply intact and no sustained decline")
    # Cadence second, and only as colour: it is the loop's pacing, which tracks the backlog rather
    # than the network. Reported because a REAL stall of the loop itself still shows here.
    if usable:
        usable.sort()
        med_dt = usable[len(usable) // 2]
        gaps = [d for d in usable if d > 2 * med_dt]
        print(f"      {'':>{len(board)}}  loop cadence: median {med_dt:.1f} ms, max "
              f"{max(usable):.1f} ms, {len(gaps)} pause(s) > 2x median")


def report_resync_bursts(lo, hi):
    """Per-chunk resync bursts: did discarding actually close the error?

    The one question this trace exists to answer. Dropping a chunk buys exactly one chunk of
    deadline, so REAL lateness falls ~1:1 with the audio discarded. A bad prediction or a bad
    deadline reads the same excess on every following chunk, so err stays flat or grows while
    drops climbs. Those two want opposite fixes -- bound the response, or gate the trigger --
    and at report resolution they look identical, which is how a discard cap was flashed and
    reverted. So the verdict is reported per burst, not per event.
    """
    printed = False
    for board, rows in sorted(RSYNCS.items()):
        win = sorted((r for r in rows if lo <= r[0] <= hi), key=lambda r: r[0])
        if len(win) < 4:
            continue
        # Sorted by time above because the pre-trigger rows are LOGGED after the arm (the replay
        # is paced across the chunks that follow it) while they BELONG before it. Sorted, an
        # episode runs -80..-1 then 0..79 and the split below still sees one rising sequence.
        # Split into bursts on the sequence number restarting.
        bursts, cur = [], []
        for r in win:
            if cur and r[1] <= cur[-1][1]:
                bursts.append(cur)
                cur = []
            cur.append(r)
        if cur:
            bursts.append(cur)
        for b in bursts:
            if len(b) < 4:
                continue
            if not printed:
                print("   resync bursts (did discarding close the error?)")
                printed = True
            # The pre-trigger history is a different measurement and must not contaminate this
            # one: it starts at a healthy err, so folding it in would make every burst look like
            # a runaway. Verdict on the armed chunks only; the drain gets its own block below.
            pre = [r for r in b if r[1] < 0]
            live = [r for r in b if r[1] >= 0]
            report_pre_trigger_drain(board, pre)
            if len(live) < 4:
                continue
            b = live
            t0, err0, ring0 = b[0][0], b[0][2], b[0][4]
            errN, ringN, dropsN = b[-1][2], b[-1][4], b[-1][5]
            chunk_us = 26100.0
            closed = err0 - errN
            expected = dropsN * chunk_us
            # Ratio of error closed to error the discards should have closed. ~1 means the
            # lateness was real and discarding was the right tool; ~0 or negative means it
            # was not lateness at all.
            # The ratio only means anything when discards actually happened; with none, the
            # expected closure is zero and a ratio is a division by zero dressed up as data.
            print(f"      {board} t={t0:8.1f}s  n={len(b):3d}  err {err0:+9d} -> {errN:+9d} us  "
                  f"ring {ring0:5d} -> {ringN:5d} ms  drops {dropsN:3d}")
            if dropsN == 0:
                print(f"      {'':>{len(board)}}  closed {closed:+9d} us with NO discards"
                      f"  -> excursion resolved by other means; this path was not the mechanism")
                continue
            ratio = closed / expected
            # Bounded on BOTH sides. An unbounded "ratio > 0.6 means real" called +5.59 a 1:1
            # closure, which it plainly is not: closing five times more than the discards could
            # buy means something else did the work, and that is a third answer, not the first.
            if 0.6 <= ratio <= 1.6:
                verdict = "REAL lateness -- discards account for the closure"
            elif ratio > 1.6:
                verdict = "closed by SOMETHING ELSE -- discards too few to explain it"
            elif ratio < -0.2:
                verdict = "RUNAWAY -- error grew while discarding"
            elif ratio < 0.2:
                verdict = "NOT lateness -- discards did not move it"
            else:
                verdict = "partial -- inconclusive"
            print(f"      {'':>{len(board)}}  closed {closed:+9d} us of {expected:9.0f} the discards"
                  f" could buy  ratio {ratio:+.2f}")
            print(f"      {'':>{len(board)}}  -> {verdict}")
    return printed


def report_offset_integral(args, ts, ys, rate_a, rate_b):
    """Does the integral of the differential rate reproduce the measured wire offset?

    Run against both references available here, which answer different questions:

      fs columns    the analyser's own achieved rates. This is the measurement that
                    established the offset has no second term; kept runnable so the
                    result stays checkable instead of remembered.
      trim mean     what the boards themselves reported. The open question is whether a
                    device can see its own relative offset WITHOUT an analyser, which is
                    what would let it correct one.

    Returns the differential trim series for plotting, or [].
    """
    offset_us = [(t, y / 1000.0) for t, y in zip(ts, ys) if math.isfinite(y)]
    if len(offset_us) < MIN_FIT_ROWS:
        return []
    # Host-side log delay, measured rather than asserted: this is what the device stamp routes
    # around, and if it ever reads small the stamp has stopped being necessary.
    for board, (off, jitter_ms, n) in sorted(DEV_ANCHOR.items()):
        print(f"   {board}: device clock anchored from {n} stamped line(s); "
              f"host log delay MAD {jitter_ms:.0f} ms")
    report_seed_steps(ts, ys)
    report_repair_steps(ts, ys)
    report_resync_bursts(ts[0] - 1 if ts else -1, ts[-1] + 1 if ts else 0)
    report_recovery(ts, ys)
    print("   offset integral (is the offset the integral of the differential rate?)")
    dfs = pair_diff(rate_a, rate_b, to_ppm=lambda a, b: (b - a) / ((a + b) / 2.0) * 1e6)
    report_integral(
        "fs columns  (analyser)", dfs, offset_us,
        expect="established: corr -0.997..-1.000, slope -1.0, resid sd 0.14-0.38 us")

    names = [os.path.basename(p).split(".")[0] for p in (args.annotate or [])]
    have = [n for n in names if TRIMS.get(n)]
    if len(have) < 2:
        if args.annotate:
            missing = [n for n in names if not TRIMS.get(n)]
            print(f"   trim mean   (on-device): needs 'Trim window:' lines from two logs; "
                  f"none in {', '.join(missing) or 'any log'}"
                  f"{' (firmware predates the line)' if missing else ''}")
        return []
    na, nb = have[0], have[1]
    dtrim, breaks = trim_diff(TRIMS[na], TRIMS[nb])
    # --annotate order decides which log is A: the analyser's A/B comes from the probe
    # wiring and nothing in the logs can confirm it, so a swap shows as a flipped sign.
    print(f"\n   rate reference (can a board see its own differential rate?  {nb} - {na})")
    if dfs:
        # Compared against the analyser's own rates rather than against the offset. The
        # integral fit cannot separate a bad reference from a good one with a constant
        # error, and a constant error is exactly what the trim has -- so measure the
        # reference directly and price its two error terms separately.
        # Each differential point is timestamped by board A's window, so A's own reported
        # audio duration is the interval the analyser's rates must be averaged over.
        dur = {t: audio_s for t, _m, audio_s in TRIMS[na]}
        windows = [(t, dv, dur[t]) for t, dv in dtrim if t in dur]
        if report_rate_reference("trim mean   (vs analyser rate)",
                                 window_means(dfs, windows)):
            print(f"      the constant offset is the CRYSTAL DIFFERENCE: each board's trim "
                  f"cancels its own\n      crystal error, so the differential trim carries "
                  f"their difference. Being a rate it\n      integrates without bound, which "
                  f"is why a high correlation here still leaves the\n      offset integral "
                  f"below at ~1%. The device now measures this itself and publishes it\n"
                  f"      (crystal_ppm in the beacon, agreeing with the wire to 0.1 ppm), so "
                  f"compare the\n      constant above against the crystal delta panel: they "
                  f"should be the same number.")
    report_integral(
        f"trim mean   (offset integral, {nb} - {na})", dtrim, offset_us, breaks,
        expect=("fails while the constant above is unknown -- recorded so a change is "
                "visible, not because\n              it is expected to work"))
    return dtrim


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
    p.add_argument("--min-coef", type=float, default=0.0, metavar="COEF",
                   help="reject captures whose PCM correlation peak is below COEF (0 = off, the "
                        "default; 0.99 is the measured mislock threshold -- see the gate's comment). "
                        "Rejected rows stay in the CSV with pcm_coef and a reason, value NaN.")
    p.add_argument("--annotate", nargs="+", metavar="LOG", default=None,
                   help="device logs (e.g. a.log b.log) to mark on the plot as vertical "
                        "bars: frame corrections, hard resyncs, and large trim steps. The "
                        "first two are taken as board A and board B, in that order, for the "
                        "offset-integral check against their reported trim")
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
    p.add_argument("--rendertag-us", type=float, default=500.0,
                   help="--annotate: mark a RENDERTAG line whose measured and inferred phases "
                        "disagree by this much -- a ledger bias the inferred form cannot see. "
                        "A measured=unknown is always marked, since the signal refusing is "
                        "itself the event")
    p.add_argument("--phasein-us", type=float, default=1000.0,
                   help="--annotate: mark a PHASEIN line (observer logs only) where any PEER "
                        "publishes a phase this far from ours, and name that peer. The group "
                        "statistic is computed from 0-2 peers 94%% of the time, so one bad "
                        "contributor often has nothing to outvote it")
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
    p.add_argument("--stream-buffer", type=int, default=16,
                   help="blocks held between the reader and the processing loop; absorbs "
                        "a transient stall instead of dropping blocks")
    p.add_argument("--stream-seconds", type=float, default=60.0,
                   help="--stream: seconds per sigrok acquisition before restarting")
    p.add_argument("--no-prefetch", dest="prefetch", action="store_false",
                   help="do not overlap the next capture with this one's decode")
    p.add_argument("--no-disambiguation", action="store_true",
                   help="disable the whole-frame disambiguation machinery -- the median-lag "
                        "tie-break, the frame-count rebuild and the continuity correction. "
                        "They exist ONLY because program material correlates at ~0.997 between "
                        "adjacent frames. With an MLS stimulus the runner-up is ~0.03 and none "
                        "of it is needed; use this to prove that rather than assume it")
    p.add_argument("--max-frame-correct", type=int, default=0,
                   help="cap on the whole-frame ambiguity undone per row (0 = no cap). "
                        "The ambiguity is not small -- it was measured wandering over "
                        "+-10 frames -- so a cap mainly re-breaks the series")
    p.add_argument("--rate-window", type=float, default=1.0,
                   help="seconds of points fitted for the d(rate)/dt panel; differencing "
                        "consecutive rates is pure noise at high row rates")
    p.add_argument("--plot-window", type=float, default=0.0,
                   help="plot only the last N seconds (0 = the whole run). A live chart "
                        "wants a bounded window: the cost of a write grows with the "
                        "points in it, so an unbounded run slows the refresh over time")
    p.add_argument("--serve", type=int, nargs="?", const=8000, default=None,
                   metavar="PORT",
                   help="serve a live canvas plot on http://127.0.0.1:PORT/ (default "
                        "8000; 0 picks a free port). Replaces the old svgwatch.py: the "
                        "display list is streamed over one SSE connection and painted to "
                        "a canvas, so no SVG document is refetched or reparsed per frame. "
                        "The .svg file is still written")
    p.add_argument("--plot-every", type=float, default=2.0,
                   help="seconds between plot FRAMES -- a layout, pushed to --serve and, "
                        "subject to --svg-every, written to the .svg. The CSV is always "
                        "written per capture. Redrawing every capture makes a long run "
                        "quadratic")
    p.add_argument("--svg-every", type=float, default=None, metavar="SECONDS",
                   help="seconds between .svg FILE writes, independent of --plot-every "
                        "(default: every frame). Serializing and writing the file is the "
                        "expensive half of a frame, so --serve --plot-every 0.1 "
                        "--svg-every 10 gives a live view at the capture rate while the "
                        "file stays a periodic artefact. Ignored without --serve, where "
                        "the file IS the output. The final plot is always written")
    p.add_argument("--dump-skew", default=None,
                   help="write the PER-FRAME skew series (one row per audio frame, "
                        "~44100/s) to this CSV, not just one row per capture")
    p.add_argument("--overlay-expand", action="store_true",
                   help="let the overlays widen the skew axis. Off by default: fitting the "
                        "firmware estimates costs an order of magnitude of range and the "
                        "convergence being read on that panel flattens to a line")
    p.add_argument("--overlay", default="dl",
                   help="firmware offset estimates to draw over the measured skew, comma "
                        "separated: dl, phase, depth, none. Default is dl -- the delay "
                        "loop's own errA-errB, which tracks the wire at r=0.88 with ~2.5 us "
                        "of bias, where phase_b-phase_a is biased by tens of microseconds, "
                        "3-4x noisier and carries stall-stamp spikes. The axis expands to "
                        "fit whatever is overlaid, and depth-vs-group is ms-scale, so that "
                        "one stays opt-in")
    p.add_argument("--dl-event-s", type=float, default=5.0,
                   help="--annotate: minimum spacing between Delay loop engaged/holding "
                        "marks. The loop alternates about every ten seconds, so marking "
                        "every transition would bury the plot")
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
    primed_offsets = None
    overlay_sel = tuple(x.strip() for x in args.overlay.split(",")
                        if x.strip() and x.strip() != "none")

    if args.serve is not None:
        global PLOT_SERVER
        PLOT_SERVER = PlotServer(args.serve, args.plot)
        try:
            port = PLOT_SERVER.start()
        except OSError as e:
            # Loud, not fatal: a busy port is no reason to lose a capture, but a run that
            # silently served nothing while the operator watched a stale tab is worse.
            PLOT_SERVER = None
            print(f"  WARNING: --serve failed to bind port {args.serve}: {e}\n"
                  f"           continuing without the live view")
        else:
            print(f"  live plot: http://127.0.0.1:{port}/   "
                  f"(canvas, streamed; {args.plot} still written)")

    def collect_events(t_start, offsets=None, host_ref=None):
        """Log events as (elapsed_seconds, kind, label), relative to the run start."""
        out = []
        if not args.annotate:
            return out, offsets
        offsets = offsets or {}
        for path in args.annotate:
            board = os.path.basename(path).split(".")[0]
            span = [None, None]
            ev, end, st, tp, tw, xt, rs, sd, rpr, inj = parse_sync_events(path, board, args.trim_ppm,
                                                                offsets.get(path, 0), span,
                                                                LOG_STATE.get(path), args.sync_us,
                                                                args.peak_us, args.pipeline_ms,
                                                                args.log_tail_mb * (1 << 20),
                                                                args.rendertag_us, args.phasein_us,
                                                                args.dl_event_s)
            # Device time is placed on the host axis PER BOOT EPOCH -- see place_device_times for
            # why a single offset across a log spanning reboots lands nowhere. Each series is
            # anchored from its own rows, in log order, so the epoch split is well defined.
            #
            # DAY REFERENCE IS A PROPERTY OF WHEN THE LINES WERE READ (R7.2), not a constant.
            # tod_to_unix picks the day (of -1/0/+1 around the reference's midnight) that puts
            # the dateless log time nearest the reference -- i.e. it covers +-12 h around the
            # reference, no more. Referenced to t_start, a run older than ~12 h mapped every
            # FRESH line a day into the past (measured 2026-08-30 on a 31 h run: phase_a/b
            # frozen, dl_err_* blank on 100 % of rows). A live poll's lines were just written,
            # so its reference is now; a REPLOT's lines belong to the capture's own era, so the
            # caller passes the CSV's midpoint. A priming pass or replot spanning more than
            # +-12 h of log still folds the far half onto the wrong day -- that is the bound,
            # not "covered".
            ref = host_ref if host_ref is not None else time.time()
            host = lambda tod: tod_to_unix(tod, ref) - t_start
            jit_all, n_all = [], 0
            for key, pts in xt.items():
                placed, jit, nst = place_device_times(pts, lambda r: host(r[0]), lambda r: r[1])
                if jit is not None:
                    jit_all.append(jit)
                    n_all += nst
                dst = FIRMWARE.setdefault((board, key), [])
                for (tod, _dev, val), p in zip(pts, placed):
                    dst.append((p if p is not None else host(tod), val))
            offsets[path], LOG_STATE[path] = end, st
            for tod, name, val in tp:
                TEMPS.setdefault(name, []).append((host(tod), val))
            placed, jit, nst = place_device_times(tw, lambda r: host(r[0]), lambda r: r[3])
            if jit is not None:
                jit_all.append(jit)
                n_all += nst
            for (tod, mean_ppm, audio_s, _dev), p in zip(tw, placed):
                TRIMS.setdefault(board, []).append(
                    (p if p is not None else host(tod), mean_ppm, audio_s))
            placed, _jit, _n = place_device_times(rs, lambda r: host(r[0]), lambda r: r[2])
            for (tod, n, _dev, err, med, ring, drops), p in zip(rs, placed):
                RSYNCS.setdefault(board, []).append(
                    (p if p is not None else host(tod), n, err, med, ring, drops))
            placed, _jit, _n = place_device_times(sd, lambda r: host(r[0]), lambda r: r[1])
            for (tod, _dev, lat, age, frames, aerr), p in zip(sd, placed):
                SEEDS.setdefault(board, []).append(
                    (p if p is not None else host(tod), lat, age, frames, aerr))
            for tod, _dev, us in rpr:
                REPAIRS.setdefault(board, []).append((host(tod), us))
            placed, _j, _n = place_device_times(inj, lambda r: host(r[0]), lambda r: r[1])
            for (tod, _dev, us), pp in zip(inj, placed):
                INJECTS.setdefault(board, []).append((pp if pp is not None else host(tod), us))
            if n_all:
                DEV_ANCHOR[board] = (0.0, max(jit_all), n_all)
            if span[0] is not None:
                LOG_COVERAGE[path] = (tod_to_unix(span[0], t_start) - t_start,
                                      tod_to_unix(span[1], t_start) - t_start)
            for tod, kind, label in ev:
                out.append((tod_to_unix(tod, t_start) - t_start, kind, label))
        return out, offsets

    if args.simulate:
        capture, chan, sim = make_simulator(args)
        # The simulator has no acquisition to drop blocks, but the drop check at the end of
        # the run reads this unconditionally -- so leaving it unbound crashed every
        # --simulate run in its final lines, after all the work was done and printed.
        stream_state = None
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
        if args.annotate:
            # Establish trim/resync/pipeline baselines and file offsets before the
            # acquisition begins. Doing it inside the loop stalled the first block for
            # ~0.5 s -- reading 4 MB of tail from each log -- which at 17 ms blocks
            # dropped everything queued behind it. Later polls read only new bytes.
            #
            # The returned offsets MUST be carried into the loop. Discarding them meant
            # the first in-loop poll restarted from byte zero and paid the same 0.5 s all
            # over again, which is exactly the stall this was meant to remove.
            t_prime = time.time()
            _, primed_offsets = collect_events(t_prime)
            for lp, (_lo, hi) in sorted(LOG_COVERAGE.items()):
                print(f"  {lp}: log reaches t={hi:+.1f}s; keep it running for the "
                      f"whole capture")
            LOG_COVERAGE.clear()
            print(f"  primed log baselines in {time.time()-t_prime:.2f}s "
                  f"({sum(primed_offsets.values())/(1<<20):.1f} MB read; "
                  f"--log-tail-mb {args.log_tail_mb:g})")
        if args.stream:
            capture, stream_state = stream_reader(args, depth=args.stream_buffer)
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
    if args.replot:
        ts0, ys0, anchor0 = load_existing(args.out)
        if not ts0:
            sys.exit(f"{args.out} has no rows to plot")
        # Priming above already read each log once, against a placeholder anchor, and the
        # call below re-reads all of it from byte zero against the CSV's real anchor. These
        # series APPEND per line, so without a reset every point is recorded twice -- which
        # for a time series is not merely redundant: the axis runs forward, jumps back and
        # runs forward again, so gap-splitting sees two overlapping segments and every fit
        # is reported twice over. (LOG_COVERAGE is already cleared after priming for the
        # same reason.) The live path must NOT do this: there, priming's data is the only
        # copy, since the loop reads only bytes appended after it.
        TEMPS.clear()
        TRIMS.clear()
        # Every per-log series needs this, not just the two that had it: priming read each log
        # once already and the call below re-reads it from byte zero, so anything that APPENDS
        # records each point twice -- which for a time series splits one run into two
        # overlapping segments rather than merely duplicating points.
        FIRMWARE.clear()
        RSYNCS.clear()
        SEEDS.clear()
        REPAIRS.clear()
        INJECTS.clear()
        DEV_ANCHOR.clear()
        DL_DIFF.clear()
        ev, _ = collect_events(anchor0 or time.time(),
                               host_ref=(anchor0 + ts0[-1] / 2) if (anchor0 and ts0) else None)
        ev = [e for e in ev if -1 <= e[0] <= ts0[-1] + 1]
        # The overlay comes from the CSV on a replot, not from a second pass over the logs:
        # the column IS the pairing that was made when the row was written, and recomputing
        # it here against a re-parsed log would silently answer a slightly different
        # question -- with a different tolerance outcome at every gap.
        DL_DIFF.extend(load_column(args.out, "dl_diff_us"))
        if not DL_DIFF and args.annotate:
            # The column is only written by a live capture, so a CSV from before this
            # existed -- or from a run without --annotate -- has none. Recompute it from
            # the logs at the CSV's own row times, using the same tolerance, and SAY SO:
            # it is the same rule applied to the same data, but the pairing was made now
            # rather than then, so a gap that has since been re-read could pair differently.
            fa = FIRMWARE.get(("a", "dl_err_us"), [])
            fb = FIRMWARE.get(("b", "dl_err_us"), [])
            for t in ts0:
                va, vb = nearest_value(fa, t), nearest_value(fb, t)
                if va is not None and vb is not None:
                    DL_DIFF.append((t, va - vb))
            if DL_DIFF:
                print(f"  dl_diff_us absent from {args.out}; recomputed "
                      f"{len(DL_DIFF)}/{len(ts0)} pairing(s) from the logs "
                      f"(tol {DL_MATCH_S:g}s)")
        print(f"replot: {len(ts0)} rows spanning {ts0[-1]-ts0[0]:.1f} s, "
              f"{len(ev)} log event(s) in window"
              + (f", {len(DL_DIFF)} delay-loop pairing(s)" if DL_DIFF else ""))
        for path, (lo, hi) in sorted(LOG_COVERAGE.items()):
            miss = ""
            if hi < ts0[-1] - 5 or lo > ts0[0] + 5:
                miss = ("  <-- does not span the run; the unmarked stretch is "
                        "un-logged, not event-free")
            print(f"   {path}: log covers t={lo:+.1f}..{hi:+.1f} s{miss}")
        for x, kind, label in sorted(ev)[:40]:
            print(f"   t={x:8.2f}s  {kind:9s} {label}")
        ra0, rb0 = load_rates(args.out)
        ppm0 = load_column(args.out, "ppm")
        t2 = trim_temps(TEMPS, ts0[0] - 1, ts0[-1] + 1)
        if args.annotate and not t2:
            print(f"   no temperature found in {', '.join(args.annotate)} within the "
                  f"run window -- looked for 'Sending state N C', 'NAMETEMP N C', "
                  f"and 'temp=N'")
        if t2:
            print(f"   temperature series: " + ", ".join(
                f"{k} ({len(v)} pts, {min(v_ for _, v_ in v):.1f}-"
                f"{max(v_ for _, v_ in v):.1f} C)" for k, v in sorted(t2.items())))
        dtrim = report_offset_integral(args, ts0, ys0, ra0, rb0)
        write_svg(args.plot, ts0, ys0, "I2S playout skew",
                  "board B - board A (ns)   [+ = B later]",
                  stats=stats_caption(ts0, ys0),
                  include_zero=not args.y_free, events=ev,
                  overlays=firmware_overlays(ts0[0] - 1, ts0[-1] + 1, overlay_sel),
                  expand_for_overlays=args.overlay_expand,
                  panels=build_panels(args, ra0, rb0, TEMPS,
                                      ts0[0] - 1, ts0[-1] + 1, dtrim,
                                      skew=list(zip(ts0, ys0)), ppm_series=ppm0))
        print(f"  plot {args.plot}")
        return

    if args.stability:
        run_stability(args, chan, capture)
        return

    ts, ys, anchor = [], [], None
    if args.append:
        ts, ys, anchor = load_existing(args.out, for_append=True)
        if ts:
            gap = time.time() - (anchor + ts[-1]) if anchor else float("nan")
            print(f"appending to {len(ts)} existing rows in {args.out}; continuing that "
                  f"run's clock after a {gap:.0f} s gap, which the plot will show as a gap")
        new = not os.path.exists(args.out)
        log = open(args.out, "a", buffering=1)
    else:
        prev = load_existing(args.out)[0] if os.path.exists(args.out) else []
        if os.path.exists(args.out) and os.path.getsize(args.out) > 0:
            # ARCHIVE BEFORE TRUNCATING (R11.3, hardened R12.3): a manual archive can only catch
            # whatever fragment happens to be current -- two captures' raw data were lost tonight
            # to exactly this open("w"). Rename is atomic and free. Gated on the file existing,
            # not on rows having PARSED (an unparseable file still deserves the archive).
            stamp = time.strftime("%Y%m%d-%H%M%S")
            root, ext = os.path.splitext(args.out)
            keep = f"{root}-{stamp}{ext or '.csv'}"
            n_c = 0
            while os.path.exists(keep):     # never silently overwrite an archive (same-second restarts)
                n_c += 1
                keep = f"{root}-{stamp}-{n_c}{ext or '.csv'}"
            os.replace(args.out, keep)
            print(f"replacing {args.out} ({len(prev)} rows from a previous run; "
                  f"archived to {keep})")
            # RETENTION: keep the newest 5 archives for this root; a 105 MB csv times unbounded
            # restarts fills the disk, and a full disk kills the capture mid-window, silently.
            import glob
            olds = sorted(glob.glob(f"{root}-2*{ext or '.csv'}"), key=os.path.getmtime)
            for stale in olds[:-5]:
                try:
                    os.remove(stale)
                    print(f"  retention: removed old archive {stale}")
                except OSError:
                    pass
        new = True
        log = open(args.out, "w", buffering=1)
    if new:
        log.write("# i2s-skew.py: board B minus board A, positive means B later\n")
        log.write(SCHEMA + "\n")

    t_start = anchor if (args.append and anchor is not None) else time.time()
    ppms, n, shown_cfg, prefer, pending, last_off = [], 0, False, None, None, None
    # Continuity reference. The frame count is reconstructed from recent ACCEPTED values, and
    # the reference is their MEDIAN rather than the single previous value: one stray block must
    # not be able to move the anchor, because the anchor is what decides the next block's frame
    # count and a bad one propagates until something knocks it out.
    ref_hist = collections.deque(maxlen=REF_WINDOW)
    consec_reject = 0
    # Recent ACCEPTED frame counts. `prefer` is fed to frame_lag(), where a peak within
    # RIVAL_MARGIN of the best is treated as tied and the one nearest `prefer` wins -- so this
    # is what actually decides the frame count when the correlation cannot, and the frame count
    # decides which frames get PAIRED. A wrong k does not shift the answer after the fact; it
    # pairs the wrong frames and the measurement is wrong by a whole frame by construction.
    # Hence a median here too, for the same reason as ref_hist.
    lag_hist = collections.deque(maxlen=REF_WINDOW)
    ppm_series = []          # (elapsed, ppm) so the per-capture slope can be plotted
    # Start from the primed offsets so the first in-loop poll reads only new bytes.
    events, log_off, last_plot = [], primed_offsets, 0.0
    # Without a live view the file is the only output, so throttling it would just throw
    # frames away for nothing.
    svg_every = args.svg_every if (args.svg_every and PLOT_SERVER is not None) else 0.0
    last_svg = 0.0
    # A write in flight means this one is skipped, not queued: the chart is cosmetic and
    # must never hold up the acquisition.
    plot_busy = threading.Lock()
    # A window of recent values, long enough to out-vote a run of bad blocks but short
    # enough to follow a real move.
    ncorr = 0
    rate_a, rate_b = [], []
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
    # TWO DIFFERENT THINGS, TWO NAMES. `pending` was the prefetch Future AND the held frame-lag
    # candidate (an int) in one scope. In --stream, prefetch is off and only the lag use runs, so
    # the collision was invisible until the process exited: `finally: pending.cancel()` then hit
    # an int and raised AttributeError, REPLACING the reason the run ended. That is how the
    # capture governor's "GIVING UP: 40 consecutive capture failures" -- working exactly as
    # designed -- reached the operator as a confusing traceback about `int` having no `.cancel`,
    # four times before anyone read the code (2026-09-02).
    pool = pending = None
    pending_lag = None
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
                FAILURES.ok()      # a capture got through; the failure run is over
                # Remove the analyser's own zero error. It is a property of the RIG -- probe
                # delay, channel skew, threshold asymmetry -- not of the boards, and it is the
                # same size as the offsets being chased: measured at ~-25 us on 2026-08-27, when a
                # probe swap left the reading unchanged where a real difference must flip. A step
                # calibration cannot see it, because a step is a difference and the bias cancels
                # inside it. scripts/probe-cal.py measures it directly.
                if PROBE_BIAS_NS and np.isfinite(off):
                    off -= PROBE_BIAS_NS
                # FRAME-LAG UNWRAP: TRIED AND REMOVED, 2026-08-27. The theory was that the PCM
                # correlation flips between rival peaks and moves the reported offset by exactly one
                # frame (22.68 us) while the audio has not moved -- which would have explained a day
                # of ~20-25 us readings that no correction could remove.
                #
                # The data refutes it. Across 20000 rows, frame_lag changed 3070 times and the
                # offset delta at those rows was ZERO in every case: the lag increments smoothly as
                # the true offset crosses a frame boundary, which is the decomposition working. And
                # the apparent evidence -- that offset minus lag*frame is "tighter" than the raw
                # offset (MAD 5.6 vs 9.5) -- is that subtraction removing the real drift, the same
                # trap as de-meaning a series and declaring it stable.
                #
                # What IS happening is a genuine slow drift: ~23 us over 120 s, about 0.19 ppm of
                # differential rate. Do not re-add an unwrap; fix the rate.
            except RuntimeError as e:
                # Backoff, bounded output, and an eventual exit -- see CaptureFailures. The
                # wait is a FLOOR over --interval, which is 0 in --stream: without it a stuck
                # device spins the loop as fast as it can print.
                delay = FAILURES.failed(str(e), elapsed)
                pending = None
                n += 1
                if args.count and n >= args.count:
                    break
                if not args.simulate:
                    time.sleep(max(delay, args.interval))
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
            frame_ns_pre = 1e9 / info["fs"][0] if np.isfinite(info.get("fs", (0,))[0]) \
                and info["fs"][0] > 1000 else 0.0
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
            # REBUILD THE FRAME COUNT FROM THE MEDIAN, KEEPING THIS CAPTURE'S SUB-FRAME PART.
            #
            # The offset is skew = (tb[ib] - ta[ia]) over frames paired k apart, so it decomposes
            # exactly into k whole frames plus a sub-frame residue from the LRC edges. The residue
            # is unambiguous -- it comes from edge times, not from the correlation. Only k is in
            # doubt, and adjacent frames correlate at ~0.997 so a neighbouring lag is always a
            # near-tie.
            #
            # Making `prefer` a median biases the TIE-BREAK, but does not override a capture that
            # picks a neighbour confidently. Measured 2026-08-27 on a quiet run: residual dips of
            # -0.99, -1.00 and -3.03 FRAMES (coef 0.965-0.985 against rival 0.925-0.931, i.e. a
            # margin of 0.04-0.06 that scrapes past MIN_RIVAL_MARGIN). Real skew does not move in
            # exact 22.68 us steps; those are frame-count errors and nothing else.
            #
            # So take the median frame count and this capture's residue. Bounded to +-4 frames of
            # the median: beyond that the capture may be seeing a genuine step, which the jump
            # gate below is there to confirm, and this must not quietly flatten it.
            # AMBIGUOUS CAPTURES ONLY. A capture whose runner-up peak is nowhere near the best
            # one has already answered the frame count; overriding it with a running median can
            # only be wrong. Measured on an MLS stimulus (rival 0.03): forcing it gave sd 792 us
            # and rejected a third of rows, against sd 13.9 us and 100% accepted when left alone.
            unambiguous = (coef - info.get("rival", 1.0)) >= DISAMBIG_MARGIN
            disambiguate = not args.no_disambiguation and not unambiguous
            if (disambiguate
                    and math.isfinite(off) and frame_ns_pre > 0 and prefer is not None
                    and abs(k - prefer) <= FRAME_REBUILD_MAX):
                residue_pre = off - k * frame_ns_pre
                off = prefer * frame_ns_pre + residue_pre

            nwrap = 0
            # MEDIAN of the recent accepted values, not the previous one. Measured 2026-08-27:
            # with a single-value reference, 22% of accepted rows read ~0 us against a true
            # +207 us. A block that mis-correlates to lag 0 is SELF-CONSISTENT -- offset ~0 and
            # frame_lag 0 agree -- so no gate here can catch it on its own merits; it is caught
            # only by disagreeing with where the signal has recently been. One such block then
            # became the reference and dragged its successors to 0 until the next rejection,
            # which is the run of dropouts to zero seen on the plot.
            last_off = (sorted(ref_hist)[len(ref_hist) // 2] if ref_hist else None)
            if (disambiguate
                    and math.isfinite(off) and fperiod > 0 and last_off is not None):
                cand = round((last_off - off) / fperiod)
                if args.max_frame_correct:
                    cand = int(np.clip(cand, -args.max_frame_correct,
                                       args.max_frame_correct))
                if cand and abs(off + cand * fperiod - last_off) < 0.3 * fperiod:
                    nwrap = int(cand)
                    off += nwrap * fperiod
                    ncorr += 1
            reason = ("" if math.isfinite(off)
                      else info.get("hint", f"no frame match (coef {coef:.2f})"))
            if math.isfinite(off) and info.get("overlap", 1.0) < 0.75:
                reason = ""  # still a valid measurement, just note the reduced overlap
            fs_meas = info.get("fs", (float("nan"), float("nan")))
            frame_ns = (1e9 / fs_meas[0]) if (fs_meas and fs_meas[0] and fs_meas[0] > 1000) else float("nan")
            k = info.get("frame_lag", 0)
            # INTERNAL CONSISTENCY. The offset is built as an integer frame count from the PCM
            # match plus a sub-frame residue from the LRC edges, so `off` and `frame_lag` cannot
            # disagree by more than a frame or so BY CONSTRUCTION. When they do, the capture has
            # mis-locked and the number is not a measurement.
            #
            # Measured over 62088 confident rows: residue med -0.78 us, MAD 7.27, p5/p95
            # -20.9/+14.7 -- inside one frame (22.68 us) -- with only 0.1% beyond three frames.
            # Against that, the failure this catches was off by 362 FRAMES: on 2026-08-27 a 5 ms
            # reference offset was reported as +3187 us while frame_lag said -222, which implies
            # -5035. It plotted as a clean trace and would have been believed.
            #
            # coef alone does NOT catch it -- thousands of internally inconsistent rows carry
            # coef 1.000 -- which is why the test is the identity rather than the correlation.
            if math.isfinite(off) and math.isfinite(frame_ns) and frame_ns > 0:
                residue = off - k * frame_ns
                if abs(residue) > MAX_LAG_RESIDUE_FRAMES * frame_ns:
                    reason = (f"offset {off/1000:.0f} us disagrees with frame_lag {k:+d} "
                              f"(implies {k*frame_ns/1000:.0f} us) -- mis-locked capture")
                    off, ppm = float("nan"), float("nan")
                    # RE-SEED THE CONTINUITY REFERENCE FROM THE CORRELATION. nwrap above
                    # reconstructs the whole-frame count from last_off rather than from the
                    # correlation, so a reference that has just been shown wrong will re-derive
                    # the same wrong count next block, fail this same test, and reject FOREVER --
                    # the run cannot recover without a restart. Measured 2026-08-27: a deliberate
                    # +5 ms step on one board sent the rejection rate to 100% and held it there
                    # for 20 minutes, while frame_lag sat at +230 (5215 us) reporting the step
                    # correctly the whole time.
                    #
                    # Seeded from k, NOT cleared to None. Clearing was tried first and swapped one
                    # failure for another: with no reference the next block is uncorrected, and a
                    # block that happens to correlate at lag 0 is then accepted at ~0 us AND
                    # becomes the anchor, so continuity propagates the zero until the next
                    # rejection. That is a run at the right ~207 us with repeated dropouts to 0.
                    #
                    # k is the right seed because this test has just established that `off` and
                    # `frame_lag` disagree, and frame_lag is the estimator that survived the step.
                    # Frame accuracy is all that is needed: the reference only ever supplies the
                    # whole-frame count, never the sub-frame part.
                    #
                    # Only after several rejections IN A ROW, though. A lone rejection is far more
                    # likely to be one ambiguous block than a genuine step, and re-seeding on it
                    # would handrail the reference straight onto that block's own bad lag -- the
                    # single-value failure again, wearing a different hat. A real step keeps
                    # failing this test until the reference moves, so it clears the run easily.
                    consec_reject += 1
                    if consec_reject >= REF_RESEED_AFTER:
                        ref_hist.clear()
                        ref_hist.append(k * frame_ns)

            if math.isfinite(off):
                if prefer is not None and abs(k - prefer) > args.max_jump_frames:
                    if pending_lag is not None and abs(k - pending_lag) <= 4:
                        lag_hist.clear()               # confirmed twice: a real resync
                        lag_hist.append(k)
                        prefer, pending_lag = k, None
                    else:
                        pending_lag = k
                        reason = (f"frame lag jumped {k - prefer:+d} frames -- ambiguous "
                                  f"match or resync; held until the next capture agrees")
                        off, ppm = float("nan"), float("nan")
                else:
                    # MEDIAN of recent accepted lags, not the last one. Measured 2026-08-27:
                    # with prefer = last k, the reported offset sat on plateaus that stepped by
                    # whole frames -- lag flipped between 12 and 13 while the true skew held
                    # still, and each flip re-paired the frames and moved the answer 22.68 us.
                    # A tie broken toward one recent outlier propagates, because that outlier
                    # then becomes the tie-breaker for the next capture.
                    lag_hist.append(k)
                    pending_lag = None
                    prefer = (None if (args.no_disambiguation or unambiguous)
                              else int(sorted(lag_hist)[len(lag_hist) // 2]))
            # RIVAL MARGIN, CHECKED ON EVERY ROW. This used to be gated on `prefer is None`, so
            # once a run had a preferred lag it never rejected on rival grounds again -- and that
            # is exactly when it matters, because by then the ambiguity shows up as a lag that
            # flips between two near-equal peaks rather than as a failure to lock at all.
            #
            # Measured 2026-08-27 over 6000 rows, splitting on the known-true branch (~270 us)
            # against the spurious near-zero one:
            #
            #   good branch  coef 0.985  rival 0.884  margin +0.102 median
            #   near-zero    coef 0.984  rival 0.988  margin -0.004 median
            #
            # On the bad rows the RUNNER-UP IS STRONGER THAN THE PICK. coef alone cannot see it
            # (0.984 vs 0.985), which is why this is a margin test and not a coefficient test.
            # A 0.05 margin keeps 66% of good rows and none of the bad; at ~45 rows/s, throwing
            # away a third of the good ones to eliminate the branch entirely is a cheap trade.
            if math.isfinite(off) and (coef - info.get("rival", 0.0)) < MIN_RIVAL_MARGIN:
                reason = (f"ambiguous frame match: peak {coef:.3f} vs rival "
                          f"{info.get('rival', 0.0):.3f} -- runner-up too close to call")
                off, ppm = float("nan"), float("nan")
            elif prefer is None and info.get("rival", 0) > RIVAL_MARGIN:
                reason = reason or f"ambiguous frame match (rival {info['rival']:.2f})"

            # ABSOLUTE CONFIDENCE GATE (--min-coef, default off). This is NOT the margin test above
            # and cannot be folded into it: on the mislocks it was added for, `coef` fell to
            # 0.46-0.95 while `rival` stayed at 0.03-0.07, so the MARGIN was 0.43-0.92 -- far above
            # MIN_RIVAL_MARGIN, which passed every one of them. Measured 2026-08-31: during the
            # ms-class reference steps the correlator locks 35-36 WHOLE FRAMES away; ungated, a real
            # 49-197 us differential plots as 813-3295 us, and the excursions were exactly 794 and
            # 816 us == 35 and 36 x 22.68 us. Only ~3.4 % of rows sit below 0.99, so the gate is
            # nearly free. Rejected rows keep their place in the CSV with pcm_coef and this reason
            # recorded -- the value is NaN'd, not dropped, so "absent" stays distinguishable from
            # "zero" and the row count is unchanged.
            if args.min_coef > 0 and math.isfinite(off) and coef < args.min_coef:
                reason = (f"low confidence: peak {coef:.3f} < --min-coef {args.min_coef:.3f} "
                          f"-- whole-frame mislock likely (frame_lag {info.get('frame_lag', 0):+d})")
                off, ppm = float("nan"), float("nan")

            # ONLY A VALUE THAT SURVIVED EVERY GATE EARNS THE RIGHT TO ANCHOR THE NEXT BLOCK.
            # This sat above the gates, which is what made a bad lock self-perpetuating; it has
            # to stay below ALL of them, not just the consistency test, because the jump and
            # rival gates invalidate `off` too and an anchor taken from a value one of them is
            # about to reject repeats the same defect one layer down.
            if math.isfinite(off):
                ref_hist.append(off)
                consec_reject = 0
            ts.append(elapsed)
            ys.append(off if math.isfinite(off) else float("nan"))
            if math.isfinite(ppm):
                ppms.append(ppm)
                ppm_series.append((elapsed, ppm))
            fs_a, fs_b = info.get("fs", (float("nan"), float("nan")))
            if math.isfinite(fs_a):
                rate_a.append((elapsed, fs_a))
            if math.isfinite(fs_b):
                rate_b.append((elapsed, fs_b))
            # Sample-and-hold of the firmware's own numbers, so belief and measurement sit
            # on the same row and can be compared offline rather than only by eye.
            held = []
            for key in HELD_COLS:
                pts = FIRMWARE.get(key)
                held.append(f"{pts[-1][1]:.4g}" if pts else "")
            # phase_us: nearest-in-time (see the HELD_COLS note) -- blank past PHASE_MATCH_S.
            pha = nearest_value(FIRMWARE.get(("a", "phase_us"), []), elapsed, tol=PHASE_MATCH_S)
            phb = nearest_value(FIRMWARE.get(("b", "phase_us"), []), elapsed, tol=PHASE_MATCH_S)
            phase_cols = ",".join("" if v is None else f"{v:.1f}" for v in (pha, phb))
            # Nearest-in-time, not held: see DL_MATCH_S. errA - errB is the firmware's own
            # view of the quantity measured on the wire, so it goes on the same row as the
            # measurement and the two can be differenced offline rather than only by eye.
            dla = nearest_value(FIRMWARE.get(("a", "dl_err_us"), []), elapsed)
            dlb = nearest_value(FIRMWARE.get(("b", "dl_err_us"), []), elapsed)
            dld = (dla - dlb) if (dla is not None and dlb is not None) else None
            if dld is not None:
                DL_DIFF.append((elapsed, dld))
            dlcols = ",".join("" if v is None else f"{v:.1f}" for v in (dla, dlb, dld))
            # Commanded trim and integral, same treatment (PLAN-sub-microsecond R6.3).
            trimcols = ",".join(
                "" if v is None else f"{v:.2f}"
                for v in (nearest_value(FIRMWARE.get(("a", "dl_trim_ppm"), []), elapsed),
                          nearest_value(FIRMWARE.get(("b", "dl_trim_ppm"), []), elapsed),
                          nearest_value(FIRMWARE.get(("a", "dl_int_ppm"), []), elapsed),
                          nearest_value(FIRMWARE.get(("b", "dl_int_ppm"), []), elapsed)))
            # RPHASE dgate: the GATED delta, what align/the servo act on. Nearest-in-time at
            # PHASE_MATCH_S like phase_*, and blank rather than held when absent.
            rgcols = ",".join(
                "" if v is None else f"{v:.1f}"
                for v in (nearest_value(FIRMWARE.get(("a", "rphase_gate_us"), []), elapsed, tol=PHASE_MATCH_S),
                          nearest_value(FIRMWARE.get(("b", "rphase_gate_us"), []), elapsed, tol=PHASE_MATCH_S)))
            log.write(f"{elapsed:.3f},{wall:.3f},{off:.1f},{ppm:.4f},{coef:.4f},"
                      f"{info.get('frame_lag',0)},{info.get('rival',float('nan')):.3f},"
                      f"{info.get('scatter_ns',float('nan')):.1f},"
                      f"{fs_a:.4f},{fs_b:.4f},{phase_cols},{','.join(held)},{dlcols},"
                      f"{trimcols},{rgcols},{reason}\n")

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

            # Only true when priming was skipped (no --annotate, or the simulator).
            first_poll = args.annotate and log_off is None
            now = time.time()
            last_frame = (n + 1 >= args.count > 0)
            due = (now - last_plot >= args.plot_every) or last_frame
            # Two clocks, one layout. svg_every only ever SUBTRACTS writes from the frames
            # --plot-every already produced: a file cannot be written more often than the
            # plot is laid out, and asking for that silently gets the frame rate instead.
            due_svg = (svg_every <= 0 or now - last_svg >= svg_every or last_frame)
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
            if due and not plot_busy.locked():
                last_plot = now
                if due_svg:
                    last_svg = now
                pt, py_ = list(ts), list(ys)
                # The live view can widen or narrow the window without restarting the
                # capture. It overrides the flag rather than replacing it, so a run
                # started with --plot-window keeps that value until someone touches the
                # control, and the CSV is unaffected either way -- this only ever changes
                # what is DRAWN.
                pw = args.plot_window
                if PLOT_SERVER is not None and PLOT_SERVER.window is not None:
                    pw = PLOT_SERVER.window
                if pw > 0 and ts:
                    cut = ts[-1] - pw
                    i0 = bisect.bisect_left(ts, cut)
                    pt, py_ = ts[i0:], ys[i0:]
                snap = (pt, py_, list(events), list(rate_a), list(rate_b),
                        list(ppm_series))

                def draw(pt=snap[0], py_=snap[1], evs=snap[2], ra=snap[3], rb=snap[4],
                         ps=snap[5], wf=due_svg):
                    try:
                        write_svg(args.plot, pt, py_, "I2S playout skew",
                                  "board B - board A (ns)   [+ = B later]",
                                  write_file=wf,
                                  stats=stats_caption(pt, py_),
                                  include_zero=not args.y_free, events=evs,
                                  overlays=firmware_overlays(pt[0] - 1, pt[-1] + 1, overlay_sel),
                                  expand_for_overlays=args.overlay_expand,
                                  panels=build_panels(args, ra, rb, TEMPS,
                                                      pt[0] - 1, pt[-1] + 1,
                                                      skew=list(zip(pt, py_)),
                                                      ppm_series=ps))
                    except Exception:
                        # A draw runs on its own thread, so an exception here vanishes
                        # into the default thread hook and the live view simply stops
                        # updating -- looking exactly like a stalled capture. Say so.
                        traceback.print_exc()
                    finally:
                        plot_busy.release()

                plot_busy.acquire()
                threading.Thread(target=draw, daemon=True).start()
                if PLOT_SERVER is not None:
                    # A control toggle redraws this same snapshot, but never writes the
                    # file: the .svg records the run, not what someone was looking at.
                    def _redraw(d=draw):
                        if plot_busy.acquire(blocking=False):
                            d(wf=False)
                    PLOT_SERVER.redraw = _redraw
            n += 1
            if (args.count == 0 or n < args.count) and args.interval > 0 \
                    and not args.simulate:
                time.sleep(args.interval)
    except KeyboardInterrupt:
        print("\ninterrupted")
    finally:
        # Never let cleanup raise: whatever brought the loop down -- a SystemExit from the capture
        # governor, most usefully -- is what the operator needs to read, and an exception in here
        # replaces it.
        try:
            if pending is not None:
                pending.cancel()
            if pool is not None:
                pool.shutdown(wait=False)
        except Exception as exc:
            print(f"  (cleanup: {type(exc).__name__}: {exc})", file=sys.stderr)

    if ts:      # always leave a current plot behind, whatever the throttle did
        # Fitted once, at the end: the run is complete here, and a fit per plot tick would
        # be both wasteful and unreadable while the integral is still a few points long.
        dtrim = report_offset_integral(args, ts, ys, rate_a, rate_b)
        write_svg(args.plot, ts, ys, "I2S playout skew",
                  "board B - board A (ns)   [+ = B later]",
                  stats=stats_caption(ts, ys),
                  include_zero=not args.y_free, events=events,
                  overlays=firmware_overlays(-1, ts[-1] + 1, overlay_sel),
                  expand_for_overlays=args.overlay_expand,
                  panels=build_panels(args, rate_a, rate_b, TEMPS, -1, ts[-1] + 1, dtrim,
                                      skew=list(zip(ts, ys)), ppm_series=ppm_series))
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
