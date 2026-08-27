#!/usr/bin/env python3
"""Generate an MLS test stimulus for inter-device timing measurement.

WHY NOT MUSIC. Every disambiguation heuristic in i2s-skew.py exists because adjacent frames of
program material correlate at ~0.997: the correct alignment is never more than marginally
better than a wrong one, so the correlation picks a neighbouring frame and the measurement is
wrong by a whole frame period (22.68 us) while looking entirely confident. That is a stimulus
problem, and it is not fixable downstream.

A maximum-length sequence has the autocorrelation the job needs: one sharp peak, with every
other lag at -1/N. At N=511 the runner-up is 0.002 instead of 0.997, so the frame count is
unambiguous from a single capture with no continuity, no median, and no history. This is the
standard approach for impulse-response and delay measurement (MLS, Golay pairs, or a swept
sine -- the Farina method); using program material is the anomaly.

PERIOD. 511 frames is 11.6 ms at 44.1 kHz, which fits inside the analyser's 16.7 ms capture, so
every capture sees a full period. The sequence repeats, so the measurement is ambiguous at
multiples of 11.6 ms -- three orders above the offsets being measured and far outside the
+-64 frame search.

    python3 scripts/test-signal.py --self-test         # prove the autocorrelation
    python3 scripts/test-signal.py --fifo /tmp/snapfifo
"""

import argparse
import math
import struct
import sys
import time

# Galois LFSR masks, each VERIFIED to give the full 2**order - 1 period rather than taken from
# a tap table. A first attempt used Fibonacci taps from a table and produced periods of 21, 15
# and 7 instead of 511, 1023 and 2047 -- the sequence still looked like noise and still wrote a
# valid WAV, but its sidelobes were 0.94, which is the one property the whole exercise needs.
# Hence mls_period() below and the --self-test: a stimulus whose autocorrelation is not checked
# is worth nothing here.
MASKS = {9: 0x0108, 10: 0x0204, 11: 0x0402, 13: 0x100D}


def mls(order):
    """+-1 sequence of length 2**order - 1."""
    mask = MASKS[order]
    reg = 1
    out = []
    for _ in range((1 << order) - 1):
        out.append(1.0 if (reg & 1) else -1.0)
        reg = (reg >> 1) ^ mask if (reg & 1) else reg >> 1
    return out


def mls_period(order):
    """Actual cycle length of the register -- must equal 2**order - 1."""
    mask = MASKS[order]
    reg = 1
    for i in range(1, (1 << order) + 1):
        reg = (reg >> 1) ^ mask if (reg & 1) else reg >> 1
        if reg == 1:
            return i
    return -1


def self_test():
    """The whole point is the sidelobe level; measure it rather than trusting the theory."""
    print(f"{'order':>6} {'N':>6} {'period_ms':>10} {'cycle':>7} {'peak':>7} {'worst sidelobe':>15}")
    for order in sorted(MASKS):
        s = mls(order)
        n = len(s)
        # Circular autocorrelation, normalised.
        peak = sum(x * x for x in s) / n
        worst = 0.0
        for lag in range(1, n):
            c = sum(s[i] * s[(i + lag) % n] for i in range(n)) / n
            worst = max(worst, abs(c))
        cyc = mls_period(order)
        ok = "OK" if cyc == n else f"SHORT({cyc})"
        print(f"{order:6d} {n:6d} {n/44100*1000:10.2f} {ok:>7} {peak:7.3f} {worst:15.4f}")
    # For contrast: what music looks like at lag 1.
    print("\nfor comparison, adjacent-frame correlation of program material: ~0.997")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--order", type=int, default=9, help="MLS order; 9 -> 511 frames, 11.6 ms")
    ap.add_argument("--rate", type=int, default=44100)
    ap.add_argument("--amplitude", type=float, default=0.20,
                    help="0-1. MLS is full-band noise; this is deliberately not loud")
    ap.add_argument("--fifo", help="write raw PCM to this FIFO")
    ap.add_argument("--tcp", help="HOST:PORT of a snapserver tcp source (mode=server)")
    ap.add_argument("--block-ms", type=float, default=12,
                    help="bytes per write, in ms of audio; sets the granularity of the "
                         "server's arrival timestamps")
    ap.add_argument("--seconds", type=float, default=0, help="0 = forever")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()

    if args.self_test:
        self_test()
        return

    seq = mls(args.order)
    amp = int(32767 * args.amplitude)
    # One period as interleaved stereo 16-bit LE, identical on both channels.
    frame = b"".join(struct.pack("<hh", int(v * amp), int(v * amp)) for v in seq)
    period_s = len(seq) / args.rate

    if not args.fifo and not args.tcp:
        sys.exit("--fifo or --tcp is required (or --self-test)")

    print(f"MLS order {args.order}: {len(seq)} frames, {period_s*1000:.2f} ms period, "
          f"amplitude {args.amplitude}", file=sys.stderr)
    # NO PACING, DELIBERATELY. snapserver reads a pipe source at the audio rate, so a blocking
    # write is paced by the READER and the writer's timing never reaches the stream. Pacing it
    # here instead was tried first and made the stream unusable: Python's sleep granularity put
    # milliseconds of jitter on chunk arrival, snapserver stamped the chunks with it, and both
    # clients swung +-600-1260 us and never converged enough to unmute. The FIFO buffer is the
    # elasticity; back-pressure is the clock.
    # ONE PERIOD PER WRITE. snapserver timestamps chunks by ARRIVAL for pipe and tcp sources, so
    # the write size sets the granularity of those timestamps. Writing 200 ms at a time put
    # 200 ms of burstiness into the stamps and both clients swung +-500-870 us and never locked.
    # Small writes, paced by back-pressure, give smoothly arriving data and smooth stamps.
    blocks = max(1, int(args.block_ms / 1000.0 / period_s))
    payload = frame * blocks
    written = 0.0

    if args.tcp:
        # PREFERRED OVER A FIFO. A FIFO's write-open blocks until a reader appears, snapserver
        # opens its end, sees no data within 120 ms, latches to idle and closes -- and the two
        # ends then deadlock, each waiting for the other. It also creates the FIFO itself by
        # default (mode=create), unlinking the one you made and leaving your writer on an
        # orphaned inode. TCP has the same back-pressure and none of that.
        import socket
        host, _, port = args.tcp.rpartition(":")
        while True:
            s = socket.create_connection((host, int(port)))
            print(f"connected to {host}:{port}", file=sys.stderr)
            try:
                while True:
                    s.sendall(payload)
                    written += period_s * blocks
                    if args.seconds and written >= args.seconds:
                        return
            except OSError as e:
                print(f"disconnected ({e}); retrying", file=sys.stderr)
                s.close()
                time.sleep(1.0)
        return

    with open(args.fifo, "wb") as f:
        while True:
            f.write(payload)
            f.flush()
            written += period_s * blocks
            if args.seconds and written >= args.seconds:
                return


if __name__ == "__main__":
    main()
