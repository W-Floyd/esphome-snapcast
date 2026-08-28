"""Snapcast client hub component, modeled on ESPHome's sendspin component."""

import esphome.codegen as cg
from esphome.components import (
    audio,
    clock_sync,
    i2s_audio,
    i2s_rate_lock,
    network,
    ota,
    socket,
    wifi,
)
from esphome.components.esp32 import only_on_variant
from esphome.components.esp32.const import VARIANT_ESP32S3
import esphome.config_validation as cv
from esphome.const import CONF_BUFFER_SIZE, CONF_ID, CONF_NAME, CONF_NEVER, CONF_PORT
from esphome.types import ConfigType

CODEOWNERS = ["@W-Floyd"]
DEPENDENCIES = ["network"]
# audio: micro decoder libraries (FLAC, Opus); json: ArduinoJson for hello/settings payloads;
# ring_buffer: the decoded-PCM buffer between the network and player tasks;
# mdns: server auto-discovery via _snapcast._tcp when no server is configured.
# clock_sync: the protocol-agnostic clock filter and TSF sync; i2s_rate_lock: the
# S3 hardware clock steering they are paired with,
# which this component drives but does not own
AUTO_LOAD = ["audio", "clock_sync", "i2s_rate_lock", "json", "mdns", "ring_buffer"]
DOMAIN = "snapclient"

CONF_SERVER = "server"
CONF_SNAPCLIENT_ID = "snapclient_id"
CONF_FLAC = "flac"
CONF_OPUS = "opus"
CONF_TIME_SYNC_INTERVAL = "time_sync_interval"
CONF_HARD_RESYNC_THRESHOLD = "hard_resync_threshold"
CONF_STREAM_IDLE_TIMEOUT = "stream_idle_timeout"
CONF_CHANNEL_MODE = "channel_mode"

snapclient_ns = cg.esphome_ns.namespace("snapclient")
SnapclientHub = snapclient_ns.class_("SnapclientHub", cg.Component)

ChannelMode = snapclient_ns.enum("ChannelMode", is_class=True)
CHANNEL_MODES = {
    "stereo": ChannelMode.STEREO,
    "left": ChannelMode.LEFT_ONLY,
    "right": ChannelMode.RIGHT_ONLY,
    "mono": ChannelMode.MONO,
}

PauseBehavior = snapclient_ns.enum("PauseBehavior", is_class=True)
PAUSE_BEHAVIORS = {
    "allow": PauseBehavior.ALLOW,
    "resume": PauseBehavior.RESUME,
    "ignore": PauseBehavior.IGNORE,
}

SyncResilience = snapclient_ns.enum("SyncResilience", is_class=True)
SYNC_RESILIENCES = {
    "mute_on_storm": SyncResilience.MUTE_ON_STORM,
    "play_through_storms": SyncResilience.PLAY_THROUGH_STORMS,
    "never_mute": SyncResilience.NEVER_MUTE,
}

PhaseMode = snapclient_ns.enum("PhaseMode", is_class=True)
PHASE_MODES = {
    "none": PhaseMode.NONE,
    "left": PhaseMode.LEFT,
    "right": PhaseMode.RIGHT,
    "both": PhaseMode.BOTH,
}
CONF_PHASE_INVERT = "phase_invert"
CONF_SYNC_DEADBAND = "sync_deadband"
CONF_CONVERGE_FINE = "converge_fine"
CONF_RATE_LOCK = "rate_lock"
CONF_I2S_AUDIO_ID = "i2s_audio_id"
CONF_TSF_SYNC = "tsf_sync"
CONF_TIMING_DIAGNOSTICS = "timing_diagnostics"
CONF_KEEPALIVE_HOLD = "keepalive_hold"
CONF_PAUSE_BEHAVIOR = "pause_behavior"
CONF_SYNC_RESILIENCE = "sync_resilience"
CONF_REANCHOR_AFTER_RECONNECT = "reanchor_after_reconnect"
CONF_FAST_SPLICE_THRESHOLD = "fast_splice_threshold"
CONF_RENDER_ALIGN_MAX = "render_align_max"
CONF_TSF_OBSERVER = "tsf_observer"


def _none_to_empty_dict(value):
    return {} if value is None else value


# Hardware rate lock: steady-state sync corrections steer the I2S clock divider
# instead of splicing frames -- zero waveform discontinuities once locked. Opt-in
# and S3-only for now (the backend steers the S3's MCLK fractional-N divider); the
# frame-splice servo remains the automatic fallback everywhere else.
RATE_LOCK_SCHEMA = cv.All(
    _none_to_empty_dict,
    cv.Schema(
        {
            # The i2s_audio bus driving the speaker, BY ID. Not a port number: ESPHome
            # does not number buses in declaration order -- a PDM or internal-ADC
            # microphone forces its own bus to port 0 and pushes the others up -- so a
            # hand-written number silently steers the wrong clock in exactly the
            # smart-speaker layout this is most wanted in. The port is resolved from
            # i2s_audio's own assignment in to_code().
            cv.Required(CONF_I2S_AUDIO_ID): cv.use_id(i2s_audio.I2SAudioComponent),
        }
    ),
    only_on_variant(
        supported=[VARIANT_ESP32S3],
        msg_prefix="rate_lock (hardware I2S clock steering)",
    ),
)


def _validate_convergence_bands(config: ConfigType) -> ConfigType:
    """The three sync thresholds must stay ordered, or convergence cannot finish.

    Unmuting requires the median to hold inside 2x sync_deadband, and only the
    fine mechanism can hold it there -- the coarse splices limit-cycle at roughly
    +-800us. So if converge_fine drops to or below the unmute band, the handoff
    happens inside the band the servo is trying to satisfy and a muted client can
    oscillate forever (observed fleet-wide before the handoff existed). Likewise a
    converge_fine at or above hard_resync_threshold would let the hard-resync path
    fire before the fine mechanism ever engages.
    """
    unmute_band_us = 2 * config[CONF_SYNC_DEADBAND].total_microseconds
    fine_us = config[CONF_CONVERGE_FINE].total_microseconds
    hard_us = config[CONF_HARD_RESYNC_THRESHOLD].total_milliseconds * 1000
    if fine_us <= unmute_band_us:
        raise cv.Invalid(
            f"{CONF_CONVERGE_FINE} ({fine_us}us) must exceed twice "
            f"{CONF_SYNC_DEADBAND} ({unmute_band_us}us): the coarse splices cannot "
            f"settle inside the unmute band, so the client would stay muted",
            path=[CONF_CONVERGE_FINE],
        )
    if fine_us >= hard_us:
        raise cv.Invalid(
            f"{CONF_CONVERGE_FINE} ({fine_us}us) must be below "
            f"{CONF_HARD_RESYNC_THRESHOLD} ({hard_us}us), or the hard-resync path "
            f"fires before the fine mechanism engages",
            path=[CONF_CONVERGE_FINE],
        )
    return config


def _request_networking(config: ConfigType) -> ConfigType:
    """Request the networking features synchronized streaming needs."""
    network.require_high_performance_networking()
    socket.consume_sockets(1, "snapclient")(config)
    # The hub requests high performance + roaming suppression while a stream is
    # playing and releases both after (a roam scan stalls the radio long enough to
    # starve playback)
    wifi.enable_runtime_power_save_control()
    wifi.enable_runtime_roaming_suppression()
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(SnapclientHub),
            # Omit to discover the server via mDNS (_snapcast._tcp)
            cv.Optional(CONF_SERVER): cv.string_strict,
            cv.Optional(CONF_PORT, default=1704): cv.port,
            # Hello HostName; the server derives the default display name from it.
            cv.Optional(CONF_NAME): cv.string,
            # Decoded PCM buffer between the network and player tasks; PSRAM-preferred.
            # Sized for >1 s of 48 kHz/16-bit stereo by default.
            cv.Optional(CONF_BUFFER_SIZE, default=524288): cv.int_range(min=65536),
            cv.Optional(CONF_FLAC, default=True): cv.boolean,
            # Opt-in: libopus costs ~200 KB flash and, at runtime, a 120 KB
            # pseudostack plus ~40 KB of decoder state. snapserver's Opus streams
            # are always 48 kHz stereo (it resamples), so a mono or 44.1 kHz
            # pipeline has to be configured for that. Tune the memory placement
            # under the top-level `audio:` component's `codecs: opus:` key.
            cv.Optional(CONF_OPUS, default=False): cv.boolean,
            # Cadence while a stream is active; idle clients sync at max(this, 2s)
            cv.Optional(CONF_TIME_SYNC_INTERVAL, default="250ms"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(
                    min=cv.TimePeriod(milliseconds=100),
                    max=cv.TimePeriod(seconds=60),
                ),
            ),
            # Median sync error at which the steering servo engages (disengaging at
            # half). The reference esp32 snapclient uses 128us; steering trims one
            # frame per chunk, holding a stereo pair's image pinned. Raise on very
            # jittery links if corrections chatter.
            # The 100us floor is deliberate and tightening it does not help. Tried at 40us:
            # each device's OWN median improved (lock at 4us and -2us, against 53us and 81us),
            # but the inter-device skew on a logic analyser did not -- 52.4us mean before,
            # 40.8us after, with the spread WORSE (sd 14.9 -> 16.0us, range 52 -> 68us) and
            # the trim working harder for it (rate-difference sd 3.8 -> 12.2 ppm).
            #
            # That is the expected result, not a surprise: the median is measured against the
            # device's OWN prediction, so this band controls how tightly a device tracks
            # itself. A residual offset BETWEEN two devices' predictions is invisible to it.
            # Closing that needs the shared timebase to agree more precisely; it is not a
            # servo-gain question.
            cv.Optional(CONF_SYNC_DEADBAND, default="128us"): cv.All(
                cv.positive_time_period_microseconds,
                cv.Range(
                    min=cv.TimePeriod(microseconds=100),
                    max=cv.TimePeriod(milliseconds=20),
                ),
            ),
            # Coarse->fine handoff for muted convergence. Above this median error
            # the servo uses hard multi-frame splices (fast, but they limit-cycle
            # at ~+-800us with the measurement lag); below it the PI trim / single
            # frames take over and actually settle inside the deadband. This is
            # also the boundary that decides whether a hard resync re-mutes: an
            # excursion that stays inside it is trim-only, so it is inaudible and
            # does not force a re-lock. Lower it to spend longer on the fast
            # mechanism (quicker lock, but risks the splice limit cycle never
            # satisfying the unmute gate); raise it to hand off earlier.
            cv.Optional(CONF_CONVERGE_FINE, default="2ms"): cv.All(
                cv.positive_time_period_microseconds,
                cv.Range(
                    min=cv.TimePeriod(microseconds=250),
                    max=cv.TimePeriod(milliseconds=50),
                ),
            ),
            cv.Optional(CONF_HARD_RESYNC_THRESHOLD, default="50ms"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(
                    min=cv.TimePeriod(milliseconds=10),
                    max=cv.TimePeriod(milliseconds=1000),
                ),
            ),
            # Boot defaults; the snapclient select entities override them
            cv.Optional(CONF_CHANNEL_MODE, default="stereo"): cv.enum(
                CHANNEL_MODES, lower=True
            ),
            cv.Optional(CONF_PHASE_INVERT, default="none"): cv.enum(
                PHASE_MODES, lower=True
            ),
            cv.Optional(CONF_RATE_LOCK): RATE_LOCK_SCHEMA,
            # TSF group sync: same-AP clients share one
            # server->TSF mapping so their mutual sync is us-class; wifi-only,
            # silently inactive elsewhere (Kalman fallback)
            cv.Optional(CONF_TSF_SYNC, default=False): cv.boolean,
            # Per-chunk raw timing samples for scripts/raw-sync.py, which measures true
            # inter-device rendering from direct observations. Deliberately separate from
            # the log level: it emits ~38 lines/s/device, and the situation where DEBUG
            # logs are most wanted -- chasing dropouts -- is the one where that extra
            # traffic on a congested channel does the most harm. Off by default.
            cv.Optional(CONF_TIMING_DIAGNOSTICS, default=False): cv.boolean,
            # What a local PAUSE or STOP does. A fixed multiroom speaker and a desk
            # speaker want opposite answers, so this is configurable:
            #
            #   allow   honoured; the client stays silent until something plays it again
            #   resume  honoured, then undone once audio is flowing again -- survives a
            #           "stop everything" automation, at the cost of a real audible gap
            #   ignore  refused outright, so there is no gap at all; the media player
            #           keeps reporting PLAYING and the transport button does nothing.
            #           A deliberate stop is refused too, so anything that relies on
            #           stopping this player will not work.
            cv.Optional(CONF_PAUSE_BEHAVIOR, default="allow"): cv.enum(
                PAUSE_BEHAVIORS, lower=True
            ),
            # How much AUDIBLE DISRUPTION to accept rather than go silent while the
            # servo re-locks. The measured trigger for a resync storm is a ~2 s supply
            # outage upstream of this firmware, and what the listener hears afterwards
            # is the re-lock, not the outage -- so which artifact is worse depends on
            # what the speaker carries, not on the build:
            #
            #   mute_on_storm        mute once a storm is established, or past the
            #                        server's buffer. The historical behaviour.
            #   play_through_storms  ride out storms; mute only past the server's
            #                        buffer, where the DEADLINE is wrong rather than
            #                        the clock and playing toward it is meaningless
            #   never_mute           never mute after the first lock, whatever the
            #                        error. A gap is the worst outcome; a mess is not.
            #
            # None of them changes the FIRST lock after a start or reconnect: before
            # that there is no established timeline, so playing is not tolerating an
            # artifact, it is emitting audio at an unknown offset.
            cv.Optional(CONF_SYNC_RESILIENCE, default="mute_on_storm"): cv.enum(
                SYNC_RESILIENCES, lower=True
            ),
            # Force one accounting repair cycle ~10 s after each session start.
            #
            # A reconnect rebuilds the pipeline and re-anchors the playout accounting, and the
            # per-device error in that anchor becomes a permanent static offset no on-device
            # field can see: measured twice as ~1.3-1.4 ms of wire offset planted by an
            # outage's reconnect, with every metric reading healthy either side of it. A
            # repair removes ~2/3 of whatever standing offset the device carries (n=12), so
            # forcing one is the cheapest way to spend it on the offset just planted.
            #
            # OFF by default because the EFFECT is measured and the MECHANISM is not -- the
            # standing offset does not appear in the drift the repair reads, so this forces
            # the cycle by biasing the accounting rather than by re-deriving anything. Turn it
            # on to grade it against a lone reconnect; if it does not reproduce, the cost is
            # one trim hold and its +-50 us per reconnect.
            cv.Optional(CONF_REANCHOR_AFTER_RECONNECT, default=False): cv.boolean,
            # Standing offset at which POSITION correction engages while converged. 0 disables.
            #
            # The servo steers rate only, so an offset is integrated away over ~40 s (tau ~14 s at
            # KP = 0.25), and the gain cannot be raised: the feedback pivot puts loop gain at
            # KP x 3.15 = 0.79 already, and trim noise -- audible skew -- scales linearly with KP.
            # Authority is not the limit, though: a single-frame splice is ~23 us and inaudible, so
            # a 1 ms offset is ~43 frames, about a second at one per chunk. The splice path is
            # simply off whenever the rate lock is programming.
            #
            # Engages only WELL ABOVE the band (the PI owns everything below, and splices
            # limit-cycle around the deadband -- that is on record), one frame per chunk, with the
            # corrections already in flight subtracted from the median so it cannot overshoot an
            # error it has already fixed. 1ms is 8x converge_fine and above every steady-state
            # excursion measured. Off by default: this puts the splice path back in the loop while
            # unmuted, which wants measuring per install.
            # Cap on the follower-side correction for the inter-device offset. 0 disables it,
            # which is the default: the servo nulls each device against server time and nothing
            # nulls the DIFFERENCE, so this is the only thing that can, but it is a second loop
            # on the same audio and it should be switched on deliberately.
            # TSF OBSERVER: never report unhealthy, so this device holds leadership through
            # upsets that would disqualify a speaker, and log the group's phase inputs. ONLY for
            # a board driving no DAC -- on a speaker it defeats the guard that stops a device
            # whose own playout has diverged from publishing the group's timebase.
            cv.Optional(CONF_TSF_OBSERVER, default=False): cv.boolean,
            cv.Optional(CONF_RENDER_ALIGN_MAX, default="0ms"): cv.All(
                cv.positive_time_period_microseconds,
                cv.Range(max=cv.TimePeriod(milliseconds=20)),
            ),
            cv.Optional(CONF_FAST_SPLICE_THRESHOLD, default="0ms"): cv.All(
                cv.positive_time_period_microseconds
            ),
            # How long a chunk gap is bridged with keepalive silence before the
            # stream is allowed to end. Ending it tears the audio pipeline down, and
            # rebuilding playout phase costs a mute plus 7-16 s of re-lock -- so an
            # ordinary inter-track gap (17-18 s measured) is worth bridging.
            #
            # Defaults to `never`: hold the pipeline for the whole session so the
            # speaker stays ready to play in sync. The cost is a continuously fed DAC,
            # the radio pinned in high-performance mode and nonstop TSF beaconing --
            # irrelevant on a mains-powered speaker, which is what this component is
            # for, but set a duration on anything battery-powered.
            #
            # Note what `never` does NOT buy: it is not sufficient on its own for
            # instant resumption. A stream that resumed after 7.5 h idle still came
            # back with a 6.9 h stale deadline and took 9.6-16 s to settle, with the
            # pipeline held the entire time. The residual cost there was the servo
            # settling plus a TSF re-election, not the teardown this prevents.
            cv.Optional(CONF_KEEPALIVE_HOLD, default=CONF_NEVER): cv.Any(
                cv.positive_time_period_milliseconds,
                cv.one_of(CONF_NEVER, lower=True),
            ),
            cv.Optional(CONF_STREAM_IDLE_TIMEOUT, default="3s"): cv.All(
                cv.positive_time_period_milliseconds,
                cv.Range(
                    min=cv.TimePeriod(milliseconds=500),
                    max=cv.TimePeriod(seconds=30),
                ),
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
    _validate_convergence_bands,
    _request_networking,
)


def _resolve_i2s_port(rate_lock_config: ConfigType) -> int:
    """Maps the configured i2s_audio id to the port number ESPHome assigned it.

    i2s_audio computes this in its own final validation (`_assign_ports`), which has
    run by the time any to_code does, and stores it keyed by id. Reading that map is
    the only way to be right: it reorders buses so a PDM/internal-ADC microphone owns
    port 0. There is no public accessor, so this reaches into the module -- a rename
    upstream breaks the build loudly, which is the failure mode we want, rather than
    the silent wrong-clock a hand-written port number gives.
    """
    bus_id = rate_lock_config[CONF_I2S_AUDIO_ID]
    port_map = i2s_audio._get_data().port_map  # noqa: SLF001
    port = port_map.get(str(bus_id))
    if port is None:
        raise cv.Invalid(
            f"rate_lock: could not determine the I2S port for '{bus_id}'. "
            f"Known buses: {', '.join(sorted(port_map)) or '(none)'}",
            path=[CONF_RATE_LOCK, CONF_I2S_AUDIO_ID],
        )
    return port


async def to_code(config: ConfigType) -> None:
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # Pause the stream during an OTA: audio and the firmware transfer share the
    # radio (no-op when no ota component is configured)
    ota.request_ota_state_listeners()

    cg.add(var.set_server(config.get(CONF_SERVER, ""), config[CONF_PORT]))
    if CONF_NAME in config:
        cg.add(var.set_client_name(config[CONF_NAME]))
    cg.add(var.set_buffer_size(config[CONF_BUFFER_SIZE]))
    cg.add(
        var.set_time_sync_interval(
            config[CONF_TIME_SYNC_INTERVAL].total_milliseconds
        )
    )
    cg.add(var.set_sync_deadband(config[CONF_SYNC_DEADBAND].total_microseconds))
    cg.add(var.set_converge_fine(config[CONF_CONVERGE_FINE].total_microseconds))
    cg.add(
        var.set_hard_resync_threshold(
            config[CONF_HARD_RESYNC_THRESHOLD].total_milliseconds
        )
    )
    cg.add(
        var.set_stream_idle_timeout(
            config[CONF_STREAM_IDLE_TIMEOUT].total_milliseconds
        )
    )
    # 0 is the "never release while connected" sentinel on the C++ side
    cg.add(var.set_pause_behavior(config[CONF_PAUSE_BEHAVIOR]))
    cg.add(var.set_sync_resilience(config[CONF_SYNC_RESILIENCE]))
    cg.add(var.set_reanchor_after_reconnect(config[CONF_REANCHOR_AFTER_RECONNECT]))
    cg.add(var.set_fast_splice_threshold(config[CONF_FAST_SPLICE_THRESHOLD].total_microseconds))
    cg.add(var.set_render_align_max(config[CONF_RENDER_ALIGN_MAX].total_microseconds))
    cg.add(var.set_tsf_observer(config[CONF_TSF_OBSERVER]))
    hold = config[CONF_KEEPALIVE_HOLD]
    cg.add(var.set_keepalive_hold(0 if hold == CONF_NEVER else hold.total_milliseconds))

    cg.add(var.set_channel_mode(config[CONF_CHANNEL_MODE]))
    cg.add(var.set_phase_mode(config[CONF_PHASE_INVERT]))

    if CONF_RATE_LOCK in config:
        i2s_rate_lock.request_rate_lock()
        cg.add(var.set_rate_lock_port(_resolve_i2s_port(config[CONF_RATE_LOCK])))

    if config[CONF_TSF_SYNC]:
        clock_sync.request_tsf_sync()
    if config[CONF_TIMING_DIAGNOSTICS]:
        cg.add_define("USE_SNAPCLIENT_TIMING_DIAG", True)

    if config[CONF_FLAC]:
        # snapserver's default stream codec; pulls micro_flac from esp-audio-libs
        audio.request_flac_support()
        cg.add_define("USE_SNAPCLIENT_FLAC", True)

    if config[CONF_OPUS]:
        # Pulls micro-opus (libopus). Snapcast frames Opus raw -- one packet per wire
        # chunk, no Ogg -- so the decode path calls libopus directly rather than going
        # through esp-audio-libs' Ogg-expecting decoder.
        audio.request_opus_support()
        cg.add_define("USE_SNAPCLIENT_OPUS", True)
