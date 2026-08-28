# CLAUDE.md — instrumentation and diagnostics

Written 2026-08-28 after a session in which **every false finding came from the measuring
apparatus, not from the audio path**. Three findings were reported confidently and retracted; five
hypotheses about one symptom died. The bugs were real but none were where they were looked for.

This file is about not repeating that. It is not general project context — `HANDOFF.md` and
`TODO.md` carry that.

## The one rule

**Suspect the instrument first.** In this project the audio path is heavily instrumented and much
studied; the instrumentation is not. When a measurement is surprising, the cheapest hypothesis is
that the measurement is wrong, and on this bench that hypothesis has been right more often than not.

Before reporting a number, be able to answer: *what would make this number wrong, and have I checked
it?*

## Log lines

**The formatting ceiling is 256 bytes of message (~311 including the prefix).** Anything past it is
cut mid-token, silently.

* Measured: the combined `Sync:` line hit exactly 311 on **140 of 144 reports**. Of the lines
  carrying a `trim` field, **3 of 142 on one board and 0 of 141 on the other** had a complete
  parenthetical.
* Consequence: every count taken from a field near the end was really a count of *"did the line
  happen to fit"*. Reported as findings and retracted: `split-hold = 0`, `gate = 0`,
  `railed 0/167`, and an entire fabricated asymmetry ("board A trims 167×, B only 18×").

Therefore:

* **A load-bearing line must have no variable-length tail.** Splitting the line is not enough —
  a bounded-looking field plus unbounded numbers still reached the ceiling as soon as the error
  values widened. Verified live at 14:40:26, eleven seconds after the "fix".
* **Put diagnostics on their own short line** (`TRIMDBG`, `SYNCX`, `DELAYBLK`), not on the end of a
  report. `HANDOFF.md` said this already: *"Add a short dedicated line instead."*
* **Check line lengths before trusting any count derived from a log field:**
  `awk '{print length($0)}' x.log | sort -n | uniq -c | tail`

## Parsers

* **Never require a trailing field in a regex.** `SYNC_RE` required `trim … ppm`, so a truncated
  line failed to match and was dropped *whole*, taking that report's frame corrections, hard
  resyncs, medians and pipeline steps with it. A formatting limit became silent data loss and the
  plot simply showed fewer events and looked healthy.
* **Verify parsers against real captured lines before flashing**, including the pathological ones
  (truncated mid-token, field absent entirely). Cheap, and it caught two mistakes.
* **Parse every line the firmware emits.** A signal that exists but is unparsed is worse than one
  that does not exist: it looks like the event never happened.

## Sentinels and absent values

* **Never print a sentinel as a number.** `RENDER_PHASE_UNKNOWN` is `INT64_MIN`; printed raw, any
  consumer that subtracts it gets 2⁶³ of overflow *that looks like a measurement*. It produced a
  `gap = -9223361679831351445` that was briefly taken seriously. Print `unknown`.
* **Distinguish "signal absent" from "signal zero".** `tags=0` is a configuration answer (resampler
  in path, mixer blending), not a fault. Design the field so the two cannot be confused.
* **Prefer a signal that reports its own validity** over one always present and occasionally wrong.
  This is why the freshness gate publishes `RENDER_PHASE_UNKNOWN` rather than a stale phase, and it
  is the correct trade every time.

## Accumulators and counters

* **A reset must not be conditional on an unrelated success.** The delay accumulator's reset sat
  inside `if (raw_tsf_sample(...))`, so a failed sample left it accumulating: `n` reached 668, 1002,
  1336, 1673 — exact multiples of the per-report ~334 — with `sd` inflated by the extra drift.
  Those statistics described a window of unknown, unreported length.
* **Report `n` alongside every statistic.** It is what made the above visible at all.
* Reset on **every** path out, or reset at a single point that always executes.

## Statistics

* **`sd` is destroyed by outliers; use median/MAD** when transients are present. A window containing
  a reboot gave `sd = 30343 µs` against a median of −18.5 µs.
* **`sem = sd/√n` assumes independence.** Consecutive samples from one pipeline usually are not
  independent, so it is a **lower bound**, not the standard error. Measure the effective sample size
  (block-means variance sweep) rather than assuming it.
* **Interleaving (odd/even) cancels drift *and* the correlated component**, so it can detect
  correlation but never size it. And positive autocorrelation makes the halves *more* alike, so its
  ratio falls **below** the independence expectation — the opposite of what intuition suggests.
  Getting that sense backwards was reported as a finding.
* **Compare like with like.** A "before" statistic computed after discarding 40% of samples as
  outliers is conditioned on being outside a disturbance; an "after" statistic that discards nothing
  is not comparable to it. That produced an apparent MAD regression that was an artefact of the
  filtering.

## Reading logs on this bench

* **`a.log`/`b.log`/`observer.log` span days and carry no date.** A timestamp grep matches a previous
  day's build — it produced a mixed extract of two firmware eras that was briefly analysed as one.
  **Anchor on byte offset** (`grep -abo`, `tail -c +N`), never on `[HH:MM:SS]`.
* **Tail size is a silent correctness parameter.** `--log-tail-mb 4` reached back only ~12 minutes of
  `observer.log` and reported **zero** events for a window containing a 36-second group-wide burst.
  An under-read looks exactly like "nothing happened". Use `--log-tail-mb 60` for `--replot`.
* **`observer.log` is the most under-used source.** The observer drives no DAC, so a disturbance it
  sees equally is in the timebase, not in playout. It is also the only board that emits `PHASEIN`
  (the group-consensus *inputs*, naming which peer moved) — the group delta's output cannot diagnose
  itself.

## Perturbing and measuring

* **Every reflash costs five consensus membership changes.** Measured: `|median error|` 154 µs within
  15 s of a membership change against 93 µs elsewhere, p90 674 vs 286. Thirteen reflashes in one
  session made the operator the dominant disturbance on the bench, and most of the "events" chased
  were self-inflicted. Batch changes; flash once; then leave it alone.
* **Do not compile on the analyser's host during a measurement.** At load 5.0 the capture stalled for
  10–12 s three times in one run.
  *But*: per-capture quantities (`offset_ns`, `fs_a_hz`) are computed **within** a single buffer, so
  a gap removes points without corrupting the survivors. Do not over-claim contamination — only
  cross-capture derivatives (`d(skew)/dt`) span a gap.
* **Absolute values carry the rig's own error.** Both boards read `+44 ppm` high because the fx2lafw
  samples ~44 ppm slow; the *differential* was `+0.27 ppm` over 106,480 captures. Common-mode
  instrument error cancels in a difference and is irrelevant to skew — do not chase it.
* **Check `rival` before trusting any skew number.** MLS44 gives ~0.03; a run at 0.94 means whole-frame
  errors are masquerading as findings. One run had `rival = 0.942` on 100% of rows because
  `--samples 200000` was dropped from the command line.

## Before proposing a mechanism

* **Read the relevant code first.** Twice in one session a mechanism was proposed that the codebase
  had already implemented, measured and documented — the split-pending trim hold, and holding the
  *integral* rather than the last output while it is pending (with `−101.5 µs` measured for the
  version without it). The files are dense with prior work and its evidence; reaching for a
  hypothesis is faster than reading, and worse.
* **Test the property that matters, not the convenient one.** A tag-derived error signal was compared
  against the ledger-derived one on *noise* (MAD 72 vs 83, r = −0.001) and came back null — but noise
  was never the point. The property that mattered was **immunity to ledger perturbation**, testable
  with `inject_split`, and it was not tested.
* **Shadow before rebuilding.** Log the proposed signal beside the live one, act on nothing, and let
  it fail cheaply. It did fail, which is the point — the alternative was restructuring load-bearing
  timing code on an argument the data did not support.

## Retractions

State them plainly and early. Five hypotheses about one symptom were retracted in a session; the
ones that cost the most time were those held past the first contradicting measurement. A finding
built on a field that was truncated, stale, or sentinel-valued is not a weak finding — it is not a
finding at all, and saying so immediately is cheaper than defending it.
