# PLAN — leaderless timebase by consensus averaging

Replaces "adopt the leader's server->TSF mapping" with "adopt the mean of everyone's". Election,
takeover, MAC ordering and the leader-silence timer all stop existing, and with them the class of
bug that dominated 2026-08-27/28: leadership changed six times in seventeen minutes, and every
quantity referenced to the leader moved with it.

## Why this is the right shape

TSF IS ALREADY LEADERLESS. The AP's counter is a shared timebase every station reads directly,
and the AP does not participate — essentially Reference Broadcast Synchronization, which removes
sender-side uncertainty by construction. The leader exists for ONE narrow job: publishing a
server<->TSF mapping so each device's own estimate of server time does not wander independently.
Sharing that number does not require electing anyone.

Consensus averaging is also better, not merely simpler:

  * noise averages down as sqrt(N) instead of being inherited wholesale from one device
  * nothing to hand over, so no reference discontinuity to correct around
  * a device rebooting shifts the mean slightly instead of collapsing the timebase
  * symmetric — an observer needs no special status, and `always_healthy` becomes unnecessary

## Steps

1. **Publish raw estimates.** Every device beacons its OWN server<->TSF estimate. The transport
   already exists (phase-only follower beacons, `a0f624c`); this adds the mapping fields back to
   a follower's beacon and drops the leader/follower distinction in what is sent.

2. **Adopt the mean of all raw estimates, including our own.** Replace `adopt_(leader mapping)`.
   MEAN, NOT MEDIAN — measured 2026-08-28, median-of-three hopped +-96 us while the underlying
   data sat at +-12, because a median over three values steps whenever the ordering changes. Use
   quality weighting for outlier robustness instead.

3. **Never publish the consensus.** Publish only the raw local estimate. Feeding the consensus
   back is positive feedback that lets the whole group drift together while every device agrees.
   This is the one failure mode that would look healthy from inside.

4. ~~**Rate-limit the adopted mapping.** A device joining or leaving moves the mean for everyone.
   A stepped timebase IS a hard resync, which is the thing being avoided. Slew, do not step.~~
   **WRONG, AND MEASURED WRONG (2026-08-28): the slew cost 2.7x on sd.** The adopted mapping must
   be a DETERMINISTIC function of the live estimate set, with no per-device history. What a leader
   got right is that every device computed deadlines from ONE IDENTICAL line, so the mapping's own
   error was exactly common-mode and cancelled between devices; any per-device path to the same
   target destroys that. Stepping is safe precisely BECAUSE it is deterministic -- every device
   holding the same set steps to the same value at once, and a common-mode step is inaudible,
   which is the same argument this plan makes for group-wide drift. The danger was never the step;
   it was the path-dependence. Removing the slew took sd 9.50 -> 4.32.

5. **Delete the election machinery** once (1)-(4) hold: `Role`, takeover, `last_rx_us_`,
   `LEAD_COOLDOWN_US`, `always_healthy`, and the leader-relative diagnostics that referenced it.

## What is kept

Stream scoping (`3c4356c`) — a mapping is only comparable between devices on one stream. The
phase pairing window (`b49ae48`) — averaging estimates sampled at different instants has exactly
the drift problem it fixes, and matters MORE here. Phase-only beaconing, CPU1 pinning.

## How it gets judged

Against the logic analyser, correction disabled, matched windows:

    now, leader-based, all fixes in     median +3.0 us  sd 16.9   (settling)
    best sustained today                median +4.5 us  sd  3.6
    target                              no worse on sd, and no leadership events at all

Success is NOT a better median — it is the same or better sd with the churn gone. If sd worsens,
the mean is being moved by membership or by a feedback path, and step 3 or 4 is wrong.

    RESULT: sd worsened to 9.50, and step 4 was wrong — see step 4. After removing the slew:
    median -3.75 us, sd 4.32, MAD 2.19, zero churn, three devices. This criterion did its job.
    It named the two suspects before the measurement existed, and one of them was it.

## Risks

  * **Group-wide drift** if the consensus is ever fed back (step 3). Detectable only from outside:
    every device would agree with every other while all of them walk. The observer, or the
    analyser against server time, is the check.
  * **Membership churn** replaces leadership churn if the mean is not rate-limited (step 4).
  * **Convergence** — all devices adopting simultaneously is stable; staggered adoption during a
    join needs the slew limit to avoid a transient split.
