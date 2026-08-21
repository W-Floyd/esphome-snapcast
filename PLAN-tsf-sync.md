# Plan: TSF group sync — µs-class coherence for same-AP clients

> **Status (2026-08-21):** Implemented per this plan (`tsf_sync.{h,cpp}`, client
> wiring, `tsf_sync:` opt-in, enabled in the S3 example) including the TSF-unit
> drift publication, sandwiched TSF reads, age clamp, and plausibility gate.
> Awaiting two-device hardware validation.

## Goal

Pin the relative sync of all ESP snapclients on the same wifi AP to the TSF
timebase (~µs), instead of each device independently tracking the server through
its own wandering wifi time-sync estimate (~±100–300 µs each, uncorrelated).
The group still tracks the server through ONE member's Kalman estimate — its
wander becomes common-mode (the whole group drifts together, inaudible) instead
of differential (stereo image wander).

**Non-goals:** tightening sync to non-ESP snapclients (they can't read TSF);
cross-AP coherence (each BSS has its own TSF timer — islands stay islands);
replacing the snapcast time protocol (still needed for absolute server tracking
and as the universal fallback).

## Background

- Every station associated to a BSS syncs its TSF timer to the AP's beacons with
  µs-class precision. Verified against IDF 5.5.5:
  `esp_wifi_get_tsf_time(wifi_interface_t)` (esp_wifi.h:1376) returns the STA TSF
  in µs; `esp_wifi_sta_get_ap_info()` gives the current BSSID.
- Measured (Chen & Yang, "Understanding PTP in Today's Wi-Fi Networks", ATC '21,
  Table 9): client TSF tracks its AP with ~1 µs std and a fixed ~-4 µs bias,
  independent of CPU load, traffic, PHY rate, mobility, and signal strength.
  The client TSF is *stepped* at each beacon (102.4 ms default), not rate-
  disciplined — a ~2 µs sawtooth at 20 ppm crystal drift; longer beacon
  intervals grow it proportionally. The bias is a per-model timestamp offset:
  it cancels exactly between identical ESPs; a mixed fleet (S3 next to classic
  ESP32) may keep a fixed few-µs differential — negligible vs the 100–300 µs
  being eliminated.
- Because the multicast packet carries a TSF-anchored *mapping* rather than an
  event timestamp, accuracy is immune to delivery delay, power-save buffering,
  and multicast queueing — the entire δTx/δRx timestamping problem that
  dominates PTP-over-Wi-Fi error budgets does not apply here.
- A shared clock is NOT enough: each client's server→local mapping is its own
  Kalman estimate, and those wander independently — exactly the differential
  error we're eliminating. Coherence requires all clients to use the **same
  server→TSF mapping**, so one member (the leader) must publish its mapping and
  the rest must adopt it verbatim.
- The rate-lock servo (PLAN-rate-lock.md) already nulls whatever deadline error
  it is shown at Kp = 0.5 ppm/µs; feeding it a shared deadline is the only change
  the playback path needs.

## Design

### Mapping

The leader publishes `T(t) = tsf − server_us` as a linear function, so followers
extrapolate between beacons instead of seeing a per-beacon sawtooth:

- Packet fields: `tsf_base_us` (leader's TSF at publish), `tsf_minus_server_us`
  (at that instant, from its Kalman offset), `drift_ppm` **in TSF units**.
  The Kalman drift state is d(local − server)/dt against esp_timer, but the
  mapping extrapolates in TSF time; the missing d(tsf − local)/dt component is
  the leader-crystal-vs-AP-crystal difference (up to ~±40 ppm, usually larger
  than the Kalman drift itself). Publishing the raw Kalman drift would make the
  group's mapping sag up to ~40 µs over each 1 s publish interval and snap back
  at the next anchor — common-mode (image unaffected) but a ~1 Hz sawtooth the
  rate-lock servo would chase, and near the 5 s expiry the accumulated error
  (~200 µs) would inflate the fallback step. Fix: the leader measures its own
  TSF-vs-esp_timer rate from paired reads a few seconds apart (same sandwich
  machinery as below) and publishes
  `drift_ppm = kalman_drift + d(tsf − local)/dt`. Needs a `get_drift()`
  accessor on KalmanTimeFilter, gated on `use_drift_`.
- Follower/leader alike compute the effective offset in local time:
  `server_now = tsf_now − (tsf_minus_server_us + drift·(tsf_now − tsf_base))`,
  `shared_offset = server_now − local_now`, sampled fresh at each deadline
  computation (`chunk_deadline_us_` swaps its Kalman offset for this when a
  mapping is live). The leader uses its own published mapping, not its live
  Kalman — everyone quantizes identically, including the staleness.
- TSF↔esp_timer conversion happens at comparison time over a ~150–300 ms
  pipeline horizon, so TSF-vs-crystal drift (≤~30 ppm) contributes ≤~10 µs —
  and it's differential only to the extent two clients' crystals drift in
  opposite directions.
- TSF sampling: sandwich each TSF read between two esp_timer reads, take the
  midpoint, and discard samples whose sandwich width is an outlier (an
  interrupt landed mid-read). Applies to the leader's anchor and to each
  deadline-time sample.
- Mapping sanity guards (both fall back to own Kalman, logged once at DEBUG):
  - **Extrapolation age clamp:** expire a mapping if `tsf_now − tsf_base` is
    negative or exceeds ~10 s. An AP reboot resets the TSF to zero with the
    BSSID unchanged — every client's TSF follows the beacons down, the live
    `tsf_base_us` lands ~hours in the "future", and `server_now` is garbage
    until the leader re-anchors (≤1 s, or ≤5 s if the leader is mid-outage).
    Without the clamp that's a guaranteed hard-resync mute on every AP power
    cycle.
  - **Plausibility gate:** reject a mapping whose implied `shared_offset`
    disagrees with our own Kalman offset by more than a few ms (mirror the
    hard-resync threshold). Also covers a leader publishing garbage for any
    other reason.

### Election & transport

- One packet per second from the leader: `{magic, version, bssid[6],
  sender_mac[6], server_id_hash, tsf_base_us, tsf_minus_server_us, drift_ppm}`,
  UDP port 47083, delivered two ways:
  - **Unicast to the server-derived peer roster (primary).** Client-to-client
    multicast proved unreliable in the field (tri-band AP: leaders never heard
    each other despite sharing a BSSID). Every member fetches the connected-client
    list from the snapserver's control API (`Server.GetStatus`, port 1705 — the
    channel `Client.SetLatency` already uses) at session start and then only
    while no stream is active (the RPC blocks ~1 s), and the leader unicasts to
    each entry. Unicast client-to-client works wherever snapcast itself does.
  - Multicast to 239.255.83.84 TTL 1 (secondary; helps rosterless corners).
- **Lowest MAC wins.** Adopt a mapping only if `bssid` matches our own current
  BSSID and `server_id_hash` (FNV-1a of `host:port` of the active session)
  matches ours. If we're leading and hear a lower MAC, abdicate. If nothing
  valid is heard for ~3.5 s (+ per-MAC stagger to avoid takeover storms) and we
  have a Kalman estimate and are associated, start leading.
- Mapping expiry: no valid packet for ~5 s → fall back to own Kalman offset.
  BSSID change / disconnect / roam → drop mapping and leadership immediately
  (TSF re-syncs to an unrelated timer on roam).
- Serviced from the network task's `service_tx_()`, **only while a stream is
  active** (field finding: with modem power save on — i.e. whenever the hub isn't
  holding high-performance wifi for playback — TSF reads fail intermittently,
  producing sporadic beacons and "TSF unreadable" role flapping on an idle pair).
  Active streaming is exactly when deadlines are computed and when PS is off, so
  the gate costs nothing. Roles freeze across idle gaps; a resume grants the
  known leader a fresh timeout window before takeover is considered; stale
  mappings expire into the Kalman fallback on their own. Non-blocking socket,
  recv drained each tick. Election is decoupled from mapping sanity: any valid
  packet from an outranking sender holds followership — only *adoption* is gated
  (second field finding: gating election on the mapping checks flapped roles).

### Failure modes → behavior

| Condition | Behavior |
|---|---|
| Alone on the AP | Leads, adopts its own mapping ≡ today's Kalman path |
| AP client isolation blocks multicast | No packets heard → everyone falls back to own Kalman (today's behavior); document |
| Two leaders transiently | Lower MAC wins within ~1 s; the loser's followers step ≤ estimate difference (~100–300 µs), servo re-nulls in a few s |
| Leader leaves | ~3.5 s timeout → next-lowest MAC takes over; same bounded step |
| AP reboots (TSF resets, BSSID unchanged) | Age clamp / plausibility gate drop the mapping → Kalman fallback; re-adopt on the next fresh anchor |
| Ethernet clients / QEMU | No wifi → no TSF: compile under `USE_WIFI` guard, runtime-inactive, Kalman path untouched |
| Mixed servers (per-device selector) | `server_id_hash` mismatch → ignore each other's mappings |

### Step handling

Leader changes and mapping adoption cause a bounded deadline step (difference
between two Kalman estimates, ~≤300 µs). The rate-lock servo absorbs it at its
~2 s closed-loop pole; the splice servo absorbs it in a few chunks. Steps land
well inside the hard-resync threshold, so no re-mute.

### Config & diagnostics

- Opt-in on the hub: `tsf_sync: true` → `USE_SNAPCLIENT_TSF_SYNC` define; new
  `tsf_sync.{h,cpp}` alongside the client (class owned by SnapcastClient).
- Sync report gains `tsf=leader` / `tsf=follower(<age>s)` / absent when inactive.
- Rejected mapping / role transitions logged at DEBUG, once per transition.

## Validation

- QEMU: compiles with feature off (ethernet, no wifi) — regression only.
- Two-device hardware: enable on the stereo pair; sync reports should show one
  `tsf=leader`, one `tsf=follower`, both medians within a few tens of µs of
  zero (the residual is now servo tracking + TSF precision, not estimate
  wander). A/B the stereo image on pink noise: with TSF the image should stay
  pinned through wifi congestion that previously wandered it.
- Abuse: kill the leader (power cycle) mid-stream → follower takes over ≤5 s,
  no audible artifact; AP roam (if forceable) → clean fallback to Kalman;
  reboot the AP mid-stream → age clamp drops the mapping, no garbage deadlines,
  re-adoption after the leader re-anchors; different-server split via the
  selector → mappings ignored, no cross-talk.

## Risks

| Risk | Mitigation |
|---|---|
| Multicast filtered by AP/mesh | Automatic per-device Kalman fallback — never worse than today |
| TSF read jitter (API latency) | Read is a register access (~µs); sandwich reads with outlier rejection (see Mapping) |
| Election flapping on lossy wifi | 3.5 s timeout ≫ 1 s beacon; Huber-filtered estimates keep published mappings smooth; per-MAC stagger |
| Heavy channel interference | Multicast loss → staleness (≤5 s extrapolation, common-mode) and slightly larger beacon-step jumps; never a differential blowup — expiry → Kalman fallback |
| Long AP beacon interval (some mesh gear) | TSF step sawtooth grows with BI (~2 µs per 102.4 ms at 20 ppm) — still µs-class at any plausible BI |
| Unauthenticated packets (spoofable mapping) | Same trust model as snapcast itself on the LAN; magic+version+BSSID+server-hash gate accidents, not attackers; document |
| Leader's estimate is an outlier vs the group | Irrelevant for image (common-mode); absolute error stays bounded by that member's Kalman quality |

## Estimates

- tsf_sync class + protocol + election: ~half a day
- Client/hub/config wiring + diagnostics: ~2 h
- Hardware validation: pair session + soak

## Decision point

Ship opt-in. Promote toward default-on only after the pair soaks clean and the
takeover/roam abuse tests pass — and only for wifi devices (ethernet keeps the
Kalman path).
