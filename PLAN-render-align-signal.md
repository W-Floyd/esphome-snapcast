# PLAN — a render_align signal independent of the local frame ledger

`render_align` cannot work with the signal it has, for a structural reason rather than a tuning one.
This is the replacement signal, specified to the point where building it is mechanical.

## Why the present signal cannot work

    render_phase = (played_ts + (tsf − tsf_local)) − (s_ts − (pushed − played)·1e6/rate)
                    └── observed ──┘                 └──── INFERRED from the ledger ────┘

`pushed` and `played` are running counts. A device cannot detect that its own counter is biased by
consulting that counter — and after the servo repairs a ledger bias, the bias and the resulting
audio displacement are **equal and opposite inside that subtraction** and cancel exactly.

Measured 2026-08-28, against a known displacement:

    perturbation          class                                   on-device response
    inject_split(+1000)   frame-ledger bias, servo repairs it            0.003
    resync residual       re-baseline of the ledger, same shape          0.13
    latency +500 ms       externally planted, ledger untouched           1.000

No gain, reference choice, mean-vs-median or filter setting fixes an identity problem with
arithmetic on quantities. `render_align`'s whole measurement history — the 1150 → 118 → 58 µs
progression — was taken on a signal blind to the class of offset it exists to remove.

## The unifying diagnosis

Both real bugs found on 2026-08-28 have the same shape: **the API returns the right number of the
wrong KIND.**

    DMA span         returned CAPACITY   needed REMAINING    fixed: fork 56601e6bc6, 2213e7b521
    output callback  returns  QUANTITY   needs  IDENTITY     this document

`CallbackManager<void(uint32_t, int64_t)>` — frames and a timestamp. Never *which* audio.

## Alternatives measured and eliminated

Both were cheaper and both are dead on resolution. This is what justifies an API change.

* **`RECON drift` as the ledger-error signal** (no new API — the cross-check already exists). With
  the coherence gate live the floor is median +22 µs / MAD 23 on A and +44 / MAD 45 on B.
  Correcting a tens-of-µs error with a 23–45 µs MAD signal injects noise of its own size.
  *Side finding worth chasing separately:* those per-board medians differ **systematically** by
  ~22 µs, which is a candidate for a systematic component of the standing offset, unlike the walk.
* **`min(r_push)`** — `r_push = our pushed − the SOURCE's independent received count`, so at zero
  in-flight their difference *is* the bias. Dead on quantisation: pushes happen a chunk at a time,
  so values are `k·1152 + 128` frames and the per-block floor has sd 512 frames (11.6 ms). It
  cannot resolve one chunk, let alone the 44 frames of a 1 ms bias. Also only meaningful *within* a
  session — `dbg_pushed` and `dbg_src_received` have different epochs, so across a reconnect the
  difference reads in the tens of millions of frames.

## The signal

Let the caller attach an opaque tag to audio it hands over; the sink returns that tag when **that**
audio completes. A **captured** pair, not an inferred one.

    phase = TSF(adjusted_ts − real_frames·1e6/rate) − (tag.server_ts + tag.offset·1e6/rate)
            └─ when the descriptor's FIRST real frame rendered ─┘   └─ its server time ─┘

`pushed` and `played` never appear. A ledger bias is then visible *in* the measurement rather than
invisible *to* it — which is the right way round.

## Geometry, because it decides the API

    descriptor   441 frames = 10.0 ms      (I2SDBG geometry: frames_per_dma_buffer=441, count=5)
    chunk       1152 frames = 26.1 ms

A chunk spans 2.6 descriptors, so **descriptors straddle chunks**. A bare chunk id is therefore not
enough: the tag must carry `(server_ts, frame_offset_into_that_chunk)` for the descriptor's first
real frame.

`adjusted_ts` already exists and already does the hard part — it subtracts the trailing-silence
duration so it marks when the *real* audio finished, not when the buffer completed.

## API additions

A **separate** callback, not a signature change. Five components register or forward the existing
one (`speaker_source`, `spdif`, `i2s std`, `resampler`, `mixer`); breaking all of them in an
upstream-bound tree for this is not acceptable.

    // speaker::Speaker
    struct RenderTag { uint64_t server_ts; uint32_t offset_frames; };
    void set_next_render_tag(const RenderTag &);           // applies to the next play() payload
    void add_tagged_output_callback(std::function<void(uint32_t frames, int64_t adjusted_ts,
                                                      RenderTag tag)>);
    bool supports_render_tags() const;                     // false by default

`write_records_queue_` already holds one record per descriptor and is already maintained in lockstep
with completion events (there is an `ERR_LOCKSTEP_DESYNC` bit guarding that invariant), so extending
`uint32_t` → `{uint32_t real_frames; RenderTag tag;}` rides existing structure rather than adding
any.

## Semantics that must be decided, not left open

* **Mixer, more than one active source.** A tag from source X is meaningless in mixed output. v1
  answer: `supports_render_tags()` returns **false** whenever more than one source is active, and
  the tagged callback does not fire. Silence is correct here; a wrong tag is not.
* **Resampler in the path.** It changes frame counts, so offsets would need scaling. v1 answer:
  `supports_render_tags()` returns **false** through a resampler.
* **A descriptor with no real frames** (pure silence padding). No tag, no callback — same rule the
  existing callback already follows (`if (real_frames > 0)`).

## How it gets judged

One test, already tooled:

    inject_split(+1000 µs) on one board, measure the on-device response against the wire.
    present signal:  ratio 0.003
    this signal:     ratio ~1.0  <- pass

If it does not read ~1.0, the design is wrong and no amount of gain tuning downstream will save it.
Secondary check: the resync-residual ratio should move from 0.13 toward 1.0 as well.

## Risks

* It changes the load-bearing path for audio timing, so a bug presents as a **timing anomaly** —
  the exact class of defect this project has spent sessions chasing. Build it on a quiet bench with
  nothing else in flight, and verify with the ratio test before trusting any downstream number.
* `set_next_render_tag()` is stateful and therefore racy if anything else calls `play()` on the same
  speaker. Acceptable while snapclient is the sole writer; document it rather than design around it.
* It does not fix the *planting*, only the *seeing*. Offsets will still be planted by re-baselines;
  this is what finally lets something correct them.
