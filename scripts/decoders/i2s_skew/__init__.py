"""Measures sub-frame playout timing skew between two independent I2S streams.

Both buses are decoded to PCM, the two streams are cross-correlated to find which frames
correspond, and the skew is the mean difference between the matched frames' WS edges -- so
the answer is the audible offset between two speakers, whole frames included, not merely the
phase between two clocks.

A window is reported only when the match is trustworthy: the correlation peak must be strong
in absolute terms and must either stand clear of its rivals or agree with the previous
window's lag. Music is heavily autocorrelated, so a close rival is normal and continuity is
the stronger evidence. Unmatched windows appear on the Rejected row with the reason.

PulseView cannot graph these numbers -- libsigrokdecode has no analog output type -- so the
decoder also emits a CSV binary class; scripts/plot-decoder-skew.py turns it into an SVG.
"""

# THIS FILE IS REQUIRED. The decoder DIRECTORY is imported as a package and the loader looks
# for a `Decoder` attribute on it, so without this import the decoder silently never appears
# in PulseView's list -- which is exactly how it presented: no error in the UI, just absent.
# `sigrok-cli -L` is what surfaces the real message ("no 'Decoder' attribute in imported
# module"); PulseView shows nothing at all.
from .pd import Decoder
