# I2S Skew Decoder for sigrok / PulseView

Measures the inter-board timing skew between two I2S audio streams at
sub-sample precision — the same core idea as
[scripts/i2s-skew.py](../../i2s-skew.py).

## What it does

With BCLK and LRC probed on **both** boards:

1. **LRC marks the frame** → I2S decodes properly into PCM samples
2. **Correlate decoded PCM** (scale-invariant) → find which frames match
3. **Skew = time delta** between the two boards' LRC edges for a matched pair

## Channel wiring

| Analyser Channel | Signal  | Board |
|:---:|:---|:---|
| D0  | BCLK    | A (left)  |
| D1  | LRC / WS | A (left)  |
| D2  | DIN     | A (left)  |
| D3  | BCLK    | B (right) |
| D4  | LRC / WS | B (right) |
| D5  | DIN     | B (right) |

## Install

Drop the decoder directory into your local sigrok-decoders path:

```bash
mkdir -p ~/.local/share/libsigrokdecode/decoders
ln -s $(pwd)/decoders/i2s-skew \
    ~/.local/share/libsigrokdecode/decoders/i2s-skew
```

Alternatively, copy the files directly:

```bash
cp -r decoders/i2s-skew ~/.local/share/libsigrokdecode/decoders/
```

Verify the install:

```bash
sigrokdecode --list-decoders | grep i2s_skew
```

Expected output:

```
i2s_skew: I2S skew
```

## Use in PulseView

1. Open your logic analyser capture
2. Select the `I2S skew` decoder
3. Assign channels in order: BCLK1, LRC1, DIN1, BCLK2, LRC2, DIN2
4. Set **Bit width** to your I2S slot width (16 or 24 typical), or `0` for auto-detect
5. View annotations on the timeline

## Outputs

| Row | Description |
|:---|:---|
| **Skew (ns)** | Frame-to-frame time delta in nanoseconds |
| **Skew (us)** | Same value in microseconds |
| **Sync / correlation** | Correlation coefficient + delta from first frame |
| **Frame info** | Frame index and metadata |
| **Errors** | Warnings (e.g., insufficient frames) |

## Options

| Option | Default | Description |
|:---|:---|:---|
| `bitwidth` | `16` | I2S slot width in bits (`0` = auto-detect from LRC spacing) |
| `fsamplerate` | `44100` | Audio sample rate for display calculations |

## From the original script

The full-featured analysis in `i2s-skew.py` goes further:

- **FFT-based cross-correlation** for robust frame matching
- **Rolling lag tracking** to detect mid-capture resyncs
- **Skew series** (per-frame, ~44 100 values/sec) for drift analysis
- **Rate difference** in ppm from the skew slope
- **Offset integral** check against firmware trim reports

This decoder captures the core measurement (sub-sample LRC edge delta)
and can serve as the real-time visualisation layer, with offline
analysis from `i2s-skew.py` providing the deeper statistics.
