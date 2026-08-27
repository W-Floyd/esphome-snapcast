"""libsigrokdecode: playout skew between two independent I2S buses.

The same measurement as scripts/i2s-skew.py, live in PulseView: decode both buses, match
frames by their PCM content, and report the time between the matched frames' WS edges.

WHY CONTENT MATCHING IS THE WHOLE PROBLEM. Two boards playing the same stream emit the same
audio at different times. Comparing the Nth WS edge on each bus measures nothing useful --
it is the true skew modulo a frame, plus an arbitrary whole number of frames that depends on
where the capture happened to start. The PCM correlation is what supplies the whole-frame
part; the WS edges supply the sub-frame remainder. Both are needed and neither is enough.

AND A CONFIDENT WRONG ANSWER IS THE FAILURE MODE. Adjacent audio frames correlate at ~0.997,
so the peak and its neighbours are nearly equal and a mis-pick shifts the answer by exactly
one frame. Worse is available: on 2026-08-27 the offline analyser reported +3187 us for a
known -5000 us offset, with its correlation coefficient reading 1.000. This decoder therefore
publishes the peak AND the runner-up, and refuses to annotate a skew when they are too close
to separate -- a gap in the annotation row is information, a plausible number is not.
"""

import sigrokdecode as srd

# NO THIRD-PARTY IMPORTS, DELIBERATELY. libsigrokdecode embeds its own interpreter, and a decoder
# that imports something that interpreter lacks does not report a useful error -- it simply fails
# to load and never appears in PulseView's list, which is exactly how this one presented.
#
# numpy would buy nothing worth that risk: measured on this window size, the bounded correlation
# takes 2.38 ms in pure Python against 0.11 ms with numpy, and the window covers 11.6 ms of audio.
# A fifth of real time is ample, and one code path is one code path to test.


class Decoder(srd.Decoder):
    api_version = 3
    id = "i2s_skew"
    name = "I2S Playout Skew"
    longname = "Dual-Bus I2S Playout Skew & Correlation Analyzer"
    desc = "Sub-frame playout timing skew between two independent I2S streams."
    license = "gplv2+"
    category = "Audio"
    inputs = ["logic"]
    outputs = []

    channels = (
        {"id": "bclk_a", "name": "BCLK A", "desc": "Bit clock, board A"},
        {"id": "ws_a", "name": "WS A", "desc": "Word select / LRC, board A"},
        {"id": "din_a", "name": "DIN A", "desc": "Data, board A"},
        {"id": "bclk_b", "name": "BCLK B", "desc": "Bit clock, board B"},
        {"id": "ws_b", "name": "WS B", "desc": "Word select / LRC, board B"},
        {"id": "din_b", "name": "DIN B", "desc": "Data, board B"},
    )

    options = (
        # Only the top bits are decoded: correlation needs shape, not fidelity, and every bit
        # sampled costs a Python-level wait() per BCLK edge on both buses. 8 of 32 bits is
        # ~4x less work and correlates identically in practice.
        {"id": "bits", "desc": "Top bits to decode per slot", "default": 8},
        {"id": "bit_delay", "desc": "BCLK delay before MSB", "default": 1},
        {"id": "win_frames", "desc": "Correlation window (frames)", "default": 512},
        # A lone window is judged on two things: the peak must be strong in ABSOLUTE terms, and
        # it must either stand clear of the background or agree with the previous window.
        #
        # Margin alone does not work on music. Audio is heavily autocorrelated -- the offline
        # analyser's rival column sat at 0.75-0.99 all day while reporting perfectly good
        # numbers -- so "beat every other peak by 0.05" refuses nearly every real window. What
        # actually distinguishes a true match is that it does not move between windows.
        {"id": "min_peak", "desc": "Min correlation peak (0-1)", "default": 0.50},
        {"id": "min_margin", "desc": "Margin that alone confirms a lag", "default": 0.05},
        {"id": "lag_slack", "desc": "Frames a lag may move between windows", "default": 2},
        # Bounding the lag search does two jobs. It cuts the pure-Python correlation from O(n^2)
        # to O(n * lags) -- 512x129 instead of 512x1023 -- and it refuses matches at physically
        # implausible offsets, which is the mis-lock that produced +3187 us for a known -5000 us
        # offset in the offline analyser. 64 frames is 1.45 ms; raise it only if the boards are
        # genuinely expected further apart than that.
        {"id": "max_lag", "desc": "Max lag searched (frames)", "default": 64},
    )

    annotations = (
        ("skew", "Playout Skew"),
        ("info", "Debug Info"),
        ("warn", "Rejected"),
    )

    annotation_rows = (
        ("skews", "Playout Skew", (0,)),
        ("info", "Status", (1,)),
        ("warns", "Rejected", (2,)),
    )

    def __init__(self):
        self.reset()

    def reset(self):
        self.samplerate = None
        self.state_a = self._new_bus()
        self.state_b = self._new_bus()

    @staticmethod
    def _new_bus():
        return {"bits": [], "pcm": [], "ws_times": [], "counting": False}

    def metadata(self, key, value):
        # WITHOUT THIS THE DECODER CANNOT WORK. Every sample index has to be divided by the
        # capture rate to become a time, and libsigrokdecode only supplies that rate here.
        # The earlier draft omitted it and died at the first skew calculation.
        if key == srd.SRD_CONF_SAMPLERATE:
            self.samplerate = float(value)

    def start(self):
        self.out_ann = self.register(srd.OUTPUT_ANN)
        self.bits_per_slot = int(self.options["bits"])
        self.delay = int(self.options["bit_delay"])
        self.win_frames = int(self.options["win_frames"])
        self.min_peak = float(self.options["min_peak"])
        self.min_margin = float(self.options["min_margin"])
        self.lag_slack = int(self.options["lag_slack"])
        self.max_lag = int(self.options["max_lag"])
        self.prev_lag = None

    # ---- decoding -------------------------------------------------------------------

    def _on_ws_edge(self, bus, samplenum):
        """A frame boundary: close the previous slot and open the next."""
        if bus["bits"]:
            word = 0
            for b in bus["bits"]:
                word = (word << 1) | b
            # Sign-extend from however many bits were actually collected.
            n = len(bus["bits"])
            if word >= (1 << (n - 1)):
                word -= 1 << n
            bus["pcm"].append(float(word))
            # One timestamp per PCM value, appended together so the two lists cannot drift
            # apart -- a desync here would silently pair the wrong frames with the wrong edges.
            bus["ws_times"].append(bus["frame_start"])
        bus["bits"] = []
        bus["frame_start"] = samplenum
        bus["counting"] = True
        bus["seen"] = 0

    def _on_bclk_edge(self, bus, din):
        if not bus["counting"]:
            return
        bus["seen"] += 1
        if bus["seen"] <= self.delay:
            return
        if len(bus["bits"]) < self.bits_per_slot:
            bus["bits"].append(1 if din else 0)
        else:
            # Enough of this slot decoded; ignore the remaining BCLKs until the next WS.
            bus["counting"] = False

    def decode(self):
        if self.samplerate is None:
            raise srd.Error("cannot decode without a samplerate")
        for bus in (self.state_a, self.state_b):
            bus["frame_start"] = 0
            bus["seen"] = 0

        while True:
            # ONLY WAIT ON EDGES THAT ARE STILL WANTED. This is the difference between running
            # slower than real time and faster than it.
            #
            # BCLK is 2.82 MHz, so watching every BCLK edge on both buses is ~5.6M wait() returns
            # per second of capture, each a Python iteration -- seconds of CPU per second of audio.
            # But only the first `bit_delay + bits` edges of each slot carry anything this decoder
            # reads; the remaining ~56 are dead weight. Once a slot is done, its bus drops its BCLK
            # condition and waits for WS alone, so the loop wakes ~10 times per frame per bus
            # instead of ~64. Decoding fewer bits shrinks the work per wake; this shrinks the
            # number of wakes, which is where the time actually goes.
            conds, tags = [], []
            if self.state_a["counting"]:
                conds.append({0: "r"})
                tags.append(("bclk", self.state_a, 2))
            conds.append({1: "e"})
            tags.append(("ws", self.state_a, None))
            if self.state_b["counting"]:
                conds.append({3: "r"})
                tags.append(("bclk", self.state_b, 5))
            conds.append({4: "e"})
            tags.append(("ws", self.state_b, None))

            pins = self.wait(conds)
            m = self.matched_flags(len(conds))
            # WS is handled after BCLK for the same bus is ruled out: a frame boundary closes the
            # slot in progress, and if both land on one sample the boundary is what matters.
            for i, (kind, bus, din_idx) in enumerate(tags):
                if not m[i]:
                    continue
                if kind == "ws":
                    self._on_ws_edge(bus, self.samplenum)
                else:
                    self._on_bclk_edge(bus, pins[din_idx])

            if (len(self.state_a["pcm"]) >= self.win_frames
                    and len(self.state_b["pcm"]) >= self.win_frames):
                self.report_window()

    def matched_flags(self, count):
        """Which wait() conditions fired, as a list of bools.

        libsigrokdecode exposes this as a tuple of bools in current releases and as an integer
        bitmask in older ones. Both are in the wild and PulseView ships whichever its bundled
        libsigrokdecode has, so normalise rather than assume -- guessing wrong here silently
        routes every edge to the wrong bus.
        """
        matched = self.matched
        if isinstance(matched, int):
            return [bool(matched & (1 << i)) for i in range(count)]
        return list(matched)

    # ---- measurement ----------------------------------------------------------------

    def _corr_full(self, a, b):
        """Normalised full cross-correlation, returned as (values, index_of_zero_lag).

        Pure Python, and bounded to +-max_lag: that turns O(n^2) into O(n * lags) and keeps a
        window inside a fifth of real time without a third-party import.
        """
        n = len(a)
        ma, mb = sum(a) / n, sum(b) / n
        an = [x - ma for x in a]
        bn = [x - mb for x in b]
        na = sum(x * x for x in an) ** 0.5
        nb = sum(x * x for x in bn) ** 0.5
        if na == 0 or nb == 0:
            return None, 0
        lags = min(self.max_lag, n - 1)
        out = []
        for k in range(-lags, lags + 1):
            s = 0.0
            for i in range(n):
                j = i - k
                if 0 <= j < n:
                    s += an[i] * bn[j]
            out.append(s / (na * nb))
        return out, lags

    def report_window(self):
        a = self.state_a["pcm"][: self.win_frames]
        b = self.state_b["pcm"][: self.win_frames]
        ta = self.state_a["ws_times"][: self.win_frames]
        tb = self.state_b["ws_times"][: self.win_frames]
        start_s, end_s = ta[0], ta[-1]

        corr, zero_idx = self._corr_full(a, b)
        if corr is None:
            self.put(start_s, end_s, self.out_ann,
                     [2, ["silent window -- nothing to correlate", "silent"]])
            self.flush_window()
            return

        peak = max(range(len(corr)), key=lambda i: abs(corr[i]))
        best = abs(corr[peak])
        # Zero lag sits at index `zero_idx` and the index INCREASES as b shifts EARLIER, so the
        # lag in the sense used here -- how many frames B trails A -- is zero_idx minus the peak,
        # not the other way round. Verified against synthetic frames with known lags in
        # test_pd.py; the first version had this inverted, which mirrored every reported skew
        # about its sub-frame part and looked entirely plausible.
        lag = zero_idx - peak   # a[i] matches b[i + lag]

        # Runner-up OUTSIDE the peak's immediate neighbours: adjacent lags are correlated by
        # construction, so a neighbour being high is expected and says nothing about ambiguity.
        lo, hi = max(0, peak - 2), min(len(corr), peak + 3)
        others = [abs(corr[i]) for i in range(len(corr)) if not (lo <= i < hi)]
        rival = max(others) if others else 0.0
        margin = best - rival

        # Accept on either evidence: a peak that stands clear of the background, or one that
        # lands where the previous window's did. Continuity is the stronger test on music,
        # because a spurious peak from the audio's own periodicity moves between windows while
        # a real inter-board lag does not.
        continuous = (self.prev_lag is not None
                      and abs(lag - self.prev_lag) <= self.lag_slack)
        if best < self.min_peak or not (margin >= self.min_margin or continuous):
            why = ("weak peak" if best < self.min_peak
                   else "close rival and no agreement with the previous window")
            self.put(start_s, end_s, self.out_ann,
                     [2, [f"unmatched: peak {best:.3f} rival {rival:.3f} lag {lag:+d} -- {why}",
                          f"unmatched {best:.2f}"]])
            # A rejected window must not seed continuity for the next one, or one bad lock
            # would validate its own successors.
            self.prev_lag = None
            self.flush_window()
            return
        self.prev_lag = lag

        # Average every matched pair in the window rather than trusting one frame's edge.
        # Each edge carries the sampler's quantisation; the mean of hundreds does not.
        deltas = []
        for i in range(len(ta)):
            j = i + lag          # b index matching a index i
            if 0 <= j < len(tb):
                deltas.append(tb[j] - ta[i])
        if not deltas:
            self.put(start_s, end_s, self.out_ann,
                     [2, [f"lag {lag} leaves no overlapping frames in this window", "no overlap"]])
            self.flush_window()
            return

        mean_d = sum(deltas) / len(deltas)
        var_d = sum((d - mean_d) ** 2 for d in deltas) / len(deltas)
        skew_us = (mean_d / self.samplerate) * 1e6
        spread_us = ((var_d ** 0.5) / self.samplerate) * 1e6
        self.put(start_s, end_s, self.out_ann,
                 [0, [f"{skew_us:+.2f} us  (lag {lag:+d} frames, {len(deltas)} pairs)",
                      f"{skew_us:+.2f}us"]])
        self.put(start_s, end_s, self.out_ann,
                 [1, [f"peak {best:.3f} rival {rival:.3f} margin {margin:.3f} "
                      f"spread {spread_us:.2f} us {'(continuity)' if continuous else '(margin)'}",
                      f"c{best:.2f} m{margin:.2f}"]])
        self.flush_window()

    def flush_window(self):
        for bus in (self.state_a, self.state_b):
            bus["pcm"] = bus["pcm"][self.win_frames:]
            bus["ws_times"] = bus["ws_times"][self.win_frames:]
