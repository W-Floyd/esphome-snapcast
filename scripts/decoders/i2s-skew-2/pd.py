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

import numpy as np
import sigrokdecode as srd


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
        # Refuse to report when the runner-up peak is within this fraction of the best one.
        # Adjacent frames correlate at ~0.997, so an ambiguous match is the normal failure and
        # it produces a whole-frame error, not a small one.
        {"id": "min_margin", "desc": "Min peak margin (0-1)", "default": 0.05},
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
        self.min_margin = float(self.options["min_margin"])

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
            pins = self.wait([{0: "r"}, {1: "e"}, {3: "r"}, {4: "e"}])
            din_a, din_b = pins[2], pins[5]
            m = self.matched_flags()

            # WS first: a frame boundary closes the slot in progress, and on the rare sample
            # where both a WS and a BCLK edge land together the boundary is what matters.
            if m[1]:
                self._on_ws_edge(self.state_a, self.samplenum)
            elif m[0]:
                self._on_bclk_edge(self.state_a, din_a)

            if m[3]:
                self._on_ws_edge(self.state_b, self.samplenum)
            elif m[2]:
                self._on_bclk_edge(self.state_b, din_b)

            if (len(self.state_a["pcm"]) >= self.win_frames
                    and len(self.state_b["pcm"]) >= self.win_frames):
                self.report_window()

    def matched_flags(self):
        """Which wait() conditions fired, as a list of bools.

        libsigrokdecode exposes this as a tuple of bools in current releases and as an integer
        bitmask in older ones. Both are in the wild and PulseView ships whichever its bundled
        libsigrokdecode has, so normalise rather than assume -- guessing wrong here silently
        routes every edge to the wrong bus.
        """
        matched = self.matched
        if isinstance(matched, int):
            return [bool(matched & (1 << i)) for i in range(4)]
        return list(matched)

    # ---- measurement ----------------------------------------------------------------

    def report_window(self):
        a = np.asarray(self.state_a["pcm"][: self.win_frames], dtype=np.float64)
        b = np.asarray(self.state_b["pcm"][: self.win_frames], dtype=np.float64)
        ta = self.state_a["ws_times"][: self.win_frames]
        tb = self.state_b["ws_times"][: self.win_frames]
        start_s, end_s = ta[0], ta[-1]

        an = a - a.mean()
        bn = b - b.mean()
        na, nb = np.linalg.norm(an), np.linalg.norm(bn)
        if na == 0 or nb == 0:
            self.put(start_s, end_s, self.out_ann,
                     [2, ["silent window -- nothing to correlate", "silent"]])
            self.flush_window()
            return

        corr = np.correlate(an, bn, mode="full") / (na * nb)
        peak = int(np.argmax(np.abs(corr)))
        best = float(abs(corr[peak]))
        # np.correlate(a, b, "full") puts zero lag at index len(b)-1 and INCREASES the index as
        # b is shifted EARLIER, so the lag in the sense used here -- how many frames B trails A --
        # is the negative of the usual expression. Verified against synthetic frames with known
        # lags in test_pd.py; the first version had this inverted, which mirrored every reported
        # skew about its sub-frame part and would have looked like a plausible answer.
        lag = (len(bn) - 1) - peak   # a[i] matches b[i + lag]

        # Runner-up OUTSIDE the peak's immediate neighbours: adjacent lags are correlated by
        # construction, so a neighbour being high is expected and says nothing about ambiguity.
        mask = np.ones(corr.size, dtype=bool)
        lo, hi = max(0, peak - 2), min(corr.size, peak + 3)
        mask[lo:hi] = False
        rival = float(np.max(np.abs(corr[mask]))) if mask.any() else 0.0
        margin = best - rival

        if margin < self.min_margin:
            self.put(start_s, end_s, self.out_ann,
                     [2, [f"ambiguous match: peak {best:.3f} vs rival {rival:.3f} "
                          f"(margin {margin:.3f}) -- no skew reported",
                          f"ambiguous {margin:.3f}"]])
            self.flush_window()
            return

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

        skew_us = (float(np.mean(deltas)) / self.samplerate) * 1e6
        spread_us = (float(np.std(deltas)) / self.samplerate) * 1e6
        self.put(start_s, end_s, self.out_ann,
                 [0, [f"{skew_us:+.2f} us  (lag {lag:+d} frames, {len(deltas)} pairs)",
                      f"{skew_us:+.2f}us"]])
        self.put(start_s, end_s, self.out_ann,
                 [1, [f"peak {best:.3f} rival {rival:.3f} margin {margin:.3f} "
                      f"spread {spread_us:.2f} us",
                      f"c{best:.2f} m{margin:.2f}"]])
        self.flush_window()

    def flush_window(self):
        for bus in (self.state_a, self.state_b):
            bus["pcm"] = bus["pcm"][self.win_frames:]
            bus["ws_times"] = bus["ws_times"][self.win_frames:]
