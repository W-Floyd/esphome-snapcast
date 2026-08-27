#!/usr/bin/env python3
"""Offline test for the i2s_skew decoder's measurement maths.

PulseView cannot be scripted, and the part worth testing is not the plumbing -- it is whether a
known skew comes back as that skew, and whether an unmatchable window is REFUSED rather than
answered. Both are checked here by stubbing sigrokdecode and driving report_window() directly
with synthetic frames.

    python3 scripts/decoders/i2s-skew-2/test_pd.py
"""

import importlib.util
import random
import os
import sys
import types

# Stub the sigrokdecode module the decoder imports. Only the pieces it touches are needed.
srd = types.ModuleType("sigrokdecode")
srd.SRD_CONF_SAMPLERATE = 1
srd.OUTPUT_ANN = 0
srd.Error = RuntimeError


class _Decoder:
    def register(self, *_a, **_k):
        return 0

    def put(self, ss, es, out, data):
        self.emitted.append((ss, es, data[0], data[1][0]))


srd.Decoder = _Decoder
sys.modules["sigrokdecode"] = srd

spec = importlib.util.spec_from_file_location(
    "pd", os.path.join(os.path.dirname(os.path.abspath(__file__)), "pd.py"))
pd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pd)

RATE = 24e6          # logic analyser samples/s
FRAME = RATE / 44100  # samples per audio frame


def build(lag_frames, skew_us, n=512, seed=0):
    """Two buses playing the same audio, B lagging A by `lag_frames` and skewed by `skew_us`."""
    rng = random.Random(seed)
    audio = [rng.gauss(0, 1000) for _ in range(n + abs(lag_frames) + 8)]
    dec = pd.Decoder()
    dec.emitted = []
    dec.samplerate = RATE
    dec.options = {"bits": 8, "bit_delay": 1, "win_frames": n, "min_margin": 0.05,
                   "max_lag": 64, "min_peak": 0.50, "lag_slack": 2}
    dec.out_ann = 0
    dec.start()
    skew_samples = skew_us * 1e-6 * RATE
    for i in range(n):
        dec.state_a["pcm"].append(float(audio[i]))
        dec.state_a["ws_times"].append(int(i * FRAME))
        # B carries the audio A played `lag` frames earlier, emitted `skew` later.
        dec.state_b["pcm"].append(float(audio[i - lag_frames]) if 0 <= i - lag_frames else 0.0)
        dec.state_b["ws_times"].append(int(i * FRAME + skew_samples))
    return dec


def run(label, lag, skew_us, expect_reject=False, **kw):
    dec = build(lag, skew_us, **kw)
    dec.report_window()
    skews = [e for e in dec.emitted if e[2] == 0]
    warns = [e for e in dec.emitted if e[2] == 2]
    if expect_reject:
        ok = not skews and warns
        print(f"  {label:38s} {'PASS' if ok else 'FAIL'}  "
              f"{'refused: ' + warns[0][3][:44] if warns else 'ANSWERED (should not have)'}")
        return ok
    if not skews:
        print(f"  {label:38s} FAIL  no skew emitted"
              f"{' (' + warns[0][3][:40] + ')' if warns else ''}")
        return False
    got = float(skews[0][3].split()[0])
    # The reported skew includes the whole-frame part, since that is what a listener hears.
    want = skew_us + lag * (1e6 / 44100)
    ok = abs(got - want) < 0.5
    print(f"  {label:38s} {'PASS' if ok else 'FAIL'}  got {got:+9.2f} us, want {want:+9.2f} us")
    return ok


print("i2s_skew decoder, measurement maths:")
results = [
    run("aligned, no lag", 0, 0.0),
    run("sub-frame skew only", 0, 7.5),
    run("negative sub-frame skew", 0, -7.5),
    run("B lags A by 3 frames", 3, 0.0),
    run("A lags B by 3 frames", -3, 0.0),
    run("lag plus sub-frame", 5, 11.0),
    run("negative lag plus sub-frame", -5, -11.0),
]
# A window with no shared content must be refused, not answered: this is the failure that
# produced +3187 us for a known -5000 us offset in the offline analyser.
dec = build(0, 0.0)
_r99 = random.Random(99)
dec.state_b["pcm"] = [_r99.gauss(0, 1000) for _ in dec.state_b["pcm"]]
dec.emitted = []
dec.report_window()
uncorrelated_ok = not [e for e in dec.emitted if e[2] == 0]
print(f"  {'uncorrelated audio is refused':38s} {'PASS' if uncorrelated_ok else 'FAIL'}")
results.append(uncorrelated_ok)

# CONTINUITY. Real music is heavily autocorrelated, so the margin is routinely tiny and the
# margin test alone refuses nearly every genuine window. A lag that agrees with the previous
# window is accepted despite a close rival -- a spurious peak from the audio's own periodicity
# moves between windows, a real inter-board lag does not.
dec = build(3, 0.0)
dec.min_margin = 1.01          # nothing can ever clear the margin test
dec.prev_lag = 3               # ...but this is where the previous window landed
dec.emitted = []
dec.report_window()
cont_ok = bool([e for e in dec.emitted if e[2] == 0])
print(f"  {'close rival, agrees with previous':38s} {'PASS' if cont_ok else 'FAIL'}")
results.append(cont_ok)

# ...and it must NOT be accepted when it disagrees, or continuity would just be "accept
# everything" with extra steps.
dec = build(3, 0.0)
dec.min_margin = 1.01
dec.prev_lag = -20
dec.emitted = []
dec.report_window()
discont_ok = not [e for e in dec.emitted if e[2] == 0]
print(f"  {'close rival, disagrees -> refused':38s} {'PASS' if discont_ok else 'FAIL'}")
results.append(discont_ok)

# A REFUSED window must clear prev_lag. Otherwise one bad lock would authenticate its own
# successors and the decoder would report a whole run of confident wrong answers -- which is
# precisely the failure continuity was added to catch.
dec = build(3, 0.0)
dec.min_margin = 1.01
dec.prev_lag = -20             # disagrees -> refused, and prev_lag must be cleared
dec.emitted = []
dec.report_window()
cleared_ok = dec.prev_lag is None
print(f"  {'refusal clears continuity seed':38s} {'PASS' if cleared_ok else 'FAIL'}")
results.append(cleared_ok)

# Silence must be refused too, not divided by zero.
dec = build(0, 0.0)
dec.state_a["pcm"] = [0.0] * len(dec.state_a["pcm"])
dec.state_b["pcm"] = [0.0] * len(dec.state_b["pcm"])
dec.emitted = []
dec.report_window()
silent_ok = not [e for e in dec.emitted if e[2] == 0]
print(f"  {'silent window is refused':38s} {'PASS' if silent_ok else 'FAIL'}")
results.append(silent_ok)

print(f"\n{sum(results)}/{len(results)} passed")
sys.exit(0 if all(results) else 1)
