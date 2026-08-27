#!/usr/bin/env python3
"""Plot the i2s_skew decoder's output over time, which PulseView itself cannot do.

libsigrokdecode offers only ANN, BINARY, META and PYTHON outputs -- there is no analog or
trace output type -- so a decoder's numbers can only ever be drawn as blocks of text in
PulseView, however many of them there are. This runs the same decoder headlessly over a
saved capture, reads its CSV binary stream, and writes the dependency-free SVG the offline
analyser already produces.

    python3 scripts/plot-decoder-skew.py capture.sr
    python3 scripts/plot-decoder-skew.py capture.sr --out skew.svg --bits 16

The channel names default to the ones in scripts/logic-analyzer.pvs, so a capture taken with
that layout needs no mapping arguments.
"""

import argparse
import importlib.util
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DECODERS = os.path.join(HERE, "decoders")

# Reuse the analyser's SVG writer rather than growing a second one. Its filename is
# hyphenated, so it cannot be imported by name.
_spec = importlib.util.spec_from_file_location("i2s_skew_offline",
                                               os.path.join(HERE, "i2s-skew.py"))
_offline = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_offline)
write_svg = _offline.write_svg

# Matches scripts/logic-analyzer.pvs: board A on D1/D2/D4, board B on D6/D5/D3.
DEFAULT_MAP = "din_a=DIN_ONE:bclk_a=BCLK_ONE:ws_a=LRC_ONE:" \
              "din_b=DIN_TWO:bclk_b=BCLK_TWO:ws_b=LRC_TWO"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("capture", help="a .sr file saved from PulseView")
    ap.add_argument("--out", default=None, help="SVG path (default: alongside the capture)")
    ap.add_argument("--channels", default=DEFAULT_MAP,
                    help="decoder channel map (default: the logic-analyzer.pvs layout)")
    ap.add_argument("--bits", type=int, default=None, help="bits per slot to correlate")
    ap.add_argument("--win-frames", type=int, default=None, help="frames per window")
    ap.add_argument("--max-lag", type=int, default=None, help="max lag searched, frames")
    ap.add_argument("--csv", default=None, help="also write the decoder's raw CSV here")
    args = ap.parse_args()

    opts = ""
    for name, val in (("bits", args.bits), ("win_frames", args.win_frames),
                      ("max_lag", args.max_lag)):
        if val is not None:
            opts += f":{name}={val}"

    cmd = ["sigrok-cli", "-i", args.capture,
           "-P", f"i2s_skew:{args.channels}{opts}",
           "-B", "i2s_skew=csv"]
    env = dict(os.environ)
    # Point libsigrokdecode at the in-repo decoder, so this plots what the repo contains
    # rather than whatever copy happens to be installed.
    env["SIGROKDECODE_DIR"] = DECODERS
    print(" ".join(cmd), file=sys.stderr)
    proc = subprocess.run(cmd, capture_output=True, env=env)
    if proc.returncode != 0:
        sys.exit(f"sigrok-cli failed:\n{proc.stderr.decode(errors='replace')}")

    text = proc.stdout.decode(errors="replace")
    if args.csv:
        open(args.csv, "w").write(text)

    ts, ys = [], []
    for line in text.splitlines():
        parts = line.split(",")
        if len(parts) != 7 or parts[0] == "time_s":
            continue
        try:
            ts.append(float(parts[0]))
            ys.append(float(parts[1]))
        except ValueError:
            continue

    if not ts:
        sys.exit("decoder produced no accepted windows -- nothing to plot"
                 f"{chr(10) + proc.stderr.decode(errors='replace') if proc.stderr else ''}")

    out = args.out or os.path.splitext(args.capture)[0] + "-skew.svg"
    span = ts[-1] - ts[0]
    write_svg(out, ts, ys,
              f"I2S playout skew, {os.path.basename(args.capture)}",
              "skew (us)", include_zero=True)
    print(f"{len(ts)} accepted windows over {span:.2f} s -> {out}")
    print(f"  mean {sum(ys) / len(ys):+.2f} us, "
          f"min {min(ys):+.2f}, max {max(ys):+.2f}, "
          f"p-p {max(ys) - min(ys):.2f}")


if __name__ == "__main__":
    main()
