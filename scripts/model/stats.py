"""Statistics the analysis needs, numpy-only (no scipy on this machine).

Two of these mirror firmware or bench tooling exactly and must not be "improved":
robust_mean() is the consensus reweighting from tsf_sync.cpp, and robust_fit() is the
2.5-sigma rejection raw-sync.py uses (plain least squares tilts on re-baseline steps).
"""

from __future__ import annotations

import numpy as np


def mad(x) -> float:
    """Median absolute deviation, unscaled -- the same statistic the firmware uses."""
    x = np.asarray(x, dtype=float)
    return float(np.median(np.abs(x - np.median(x)))) if x.size else float("nan")


def robust_mean(values, scale_floor: float, k: float = 2.0) -> float:
    """One reweighting pass around the mean: w = 1/(1 + (d/(k*scale))^2).

    Port of robust_mean() in tsf_sync.cpp. Continuous in every input, so no reordering can
    step it, and with two values it is exactly their mean -- which is why it replaced
    median-of-three (measured hopping +-96 us on data sitting at +-12).
    """
    v = np.asarray(values, dtype=float)
    if v.size == 0:
        return float("nan")
    if v.size <= 2:
        return float(v.mean())
    centre = float(np.median(v))
    scale = max(mad(v - centre), scale_floor)
    w = 1.0 / (1.0 + ((v - centre) / (k * scale)) ** 2)
    return float((w * v).sum() / w.sum())


def robust_fit(x, y, sigma: float = 2.5, iters: int = 5):
    """Iteratively-rejected least squares. Returns (slope, intercept, resid_sd, n_kept)."""
    x = np.asarray(x, dtype=float)
    y = np.asarray(y, dtype=float)
    keep = np.isfinite(x) & np.isfinite(y)
    for _ in range(iters):
        if keep.sum() < 3:
            break
        a, b = np.polyfit(x[keep], y[keep], 1)
        r = y - (a * x + b)
        s = np.std(r[keep])
        if s == 0:
            break
        new = keep & (np.abs(r) <= sigma * s)
        if new.sum() == keep.sum():
            keep = new
            break
        keep = new
    if keep.sum() < 3:
        return float("nan"), float("nan"), float("nan"), int(keep.sum())
    a, b = np.polyfit(x[keep], y[keep], 1)
    r = y[keep] - (a * x[keep] + b)
    return float(a), float(b), float(np.std(r)), int(keep.sum())


def corr(x, y) -> float:
    x = np.asarray(x, dtype=float)
    y = np.asarray(y, dtype=float)
    m = np.isfinite(x) & np.isfinite(y)
    if m.sum() < 3:
        return float("nan")
    return float(np.corrcoef(x[m], y[m])[0, 1])


def bucket_medians(t, v, width_s: float):
    """Median of v within fixed-width buckets of ABSOLUTE t (seconds).

    Absolute, not per-series: bucketing each series from its own first sample left the two
    grids ~10 s apart in the first analysis of this, a third of a bucket.
    """
    t = np.asarray(t, dtype=float)
    v = np.asarray(v, dtype=float)
    idx = np.floor(t / width_s).astype(np.int64)
    out_i, out_v = [], []
    for b in np.unique(idx):
        m = (idx == b) & np.isfinite(v)
        if m.sum() >= 3:
            out_i.append(b)
            out_v.append(float(np.median(v[m])))
    return np.array(out_i), np.array(out_v)


def paired_buckets(t_a, v_a, t_b, v_b, width_s: float):
    """Bucket both series on the same absolute grid and keep buckets present in both."""
    ia, va = bucket_medians(t_a, v_a, width_s)
    ib, vb = bucket_medians(t_b, v_b, width_s)
    common = np.intersect1d(ia, ib)
    return (np.array([va[np.where(ia == c)[0][0]] for c in common]),
            np.array([vb[np.where(ib == c)[0][0]] for c in common]))


def ou_series(n: int, dt_s: float, sd: float, tau_s: float, rng) -> np.ndarray:
    """Ornstein-Uhlenbeck path: the shape the Kalman offset wander actually has (bounded
    amplitude, finite correlation time), as against a random walk (unbounded) or white
    noise (no timescale)."""
    a = np.exp(-dt_s / tau_s)
    q = sd * np.sqrt(1.0 - a * a)
    out = np.empty(n)
    x = rng.normal(0.0, sd)
    for i in range(n):
        x = a * x + rng.normal(0.0, q)
        out[i] = x
    return out


def summarise(v, label: str = "") -> str:
    v = np.asarray(v, dtype=float)
    v = v[np.isfinite(v)]
    if v.size == 0:
        return f"{label}: no data"
    return (f"{label}: n={v.size} median {np.median(v):+.2f} mean {v.mean():+.2f} "
            f"sd {v.std():.2f} MAD {mad(v):.2f} p95 {np.percentile(np.abs(v), 95):.1f}")
