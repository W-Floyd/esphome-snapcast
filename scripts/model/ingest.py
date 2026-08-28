"""Parsers for what the bench actually produced: board logs and the analyser CSV.

Deliberately tail-oriented. The logs are half a gigabyte, span days and carry no date, so
anchoring on a timestamp matches a previous day's build -- every reader here anchors on
FILE POSITION instead (HANDOFF.md, "traps that cost real time").
"""

from __future__ import annotations

import re
import subprocess
from dataclasses import dataclass

import numpy as np

RAW_RE = re.compile(
    r"\[(\d\d):(\d\d):(\d\d\.\d\d\d)\].*RAW s_ts=(\d+) pushed=(\d+) played=(\d+) "
    r"played_ts=(\d+) tsf=(\d+) tsf_local=(\d+) sw=(\d+) rate=(\d+)")
TRIM_RE = re.compile(
    r"\[(\d\d):(\d\d):(\d\d\.\d\d\d)\].*Trim window: mean ([-+][\d.]+) ppm over ([\d.]+) s")


def _wall_s(h, m, s) -> float:
    return int(h) * 3600 + int(m) * 60 + float(s)


@dataclass
class Raw:
    """RAW lines: direct observations only, one per chunk (~38/s)."""
    wall_s: np.ndarray
    s_ts: np.ndarray
    pushed: np.ndarray
    played: np.ndarray
    played_ts: np.ndarray
    tsf: np.ndarray
    tsf_local: np.ndarray
    sw: np.ndarray
    rate: np.ndarray

    def __len__(self):
        return len(self.wall_s)

    @property
    def depth_us(self):
        return (self.pushed - self.played) * 1e6 / self.rate

    @property
    def render_server(self):
        """Server audio time of the last rendered frame (the RAW line's own arithmetic)."""
        return self.s_ts - (self.pushed - self.played) * 1e6 / self.rate

    @property
    def render_tsf(self):
        return self.played_ts + (self.tsf - self.tsf_local)

    @property
    def phase_us(self):
        """render_phase: TSF instant at which this device renders server audio time zero."""
        return self.render_tsf - self.render_server


def tail_lines(path: str, nbytes: int) -> list:
    out = subprocess.run(["tail", "-c", str(nbytes), path], capture_output=True, text=True,
                         errors="replace").stdout
    lines = out.split("\n")
    return lines[1:-1] if len(lines) > 2 else []


def read_raw(path: str, nbytes: int = 200_000_000) -> Raw:
    cols = [[] for _ in range(9)]
    for line in tail_lines(path, nbytes):
        m = RAW_RE.search(line)
        if not m:
            continue
        g = m.groups()
        cols[0].append(_wall_s(g[0], g[1], g[2]))
        for i in range(8):
            cols[i + 1].append(float(g[3 + i]))
    a = [np.asarray(c, dtype=float) for c in cols]
    return Raw(*a)


def read_trim(path: str, nbytes: int = 200_000_000):
    """`Trim window: mean +NN.NNN ppm over X s` -- the loop's own output, once a second."""
    t, ppm = [], []
    for line in tail_lines(path, nbytes):
        m = TRIM_RE.search(line)
        if m:
            t.append(_wall_s(*m.groups()[:3]))
            ppm.append(float(m.group(4)))
    return np.asarray(t), np.asarray(ppm)


def read_analyser(path: str = "test.csv"):
    """i2s-skew.py output. `offset_ns` is B minus A, positive means B later."""
    return np.genfromtxt(path, delimiter=",", names=True, skip_header=1, invalid_raise=False)


def unwrap_wall(t: np.ndarray) -> np.ndarray:
    """Wall-clock seconds cross midnight in these logs; make the series monotonic."""
    if t.size == 0:
        return t
    out = t.astype(float).copy()
    day = 0.0
    for i in range(1, out.size):
        if t[i] + day < out[i - 1] - 1.0:
            day += 86400.0
        out[i] = t[i] + day
    return out
