#!/usr/bin/env bash
# Resumable bench capture: one serial logger per board plus the skew analyser, all inside a
# tmux session that survives the terminal closing, an SSH drop and a laptop lid. Runs on
# macOS and Linux; see "portability" below for what that costs.
#
# WHY TMUX AND NOT nohup: the analyser's stdout MUST stay on a terminal. When it cannot claim
# the capture device (PulseView holding it, LIBUSB_ERROR_ACCESS) it does not exit -- it
# error-loops. Redirected to a file that wrote 143 GB and filled the disk (2026-08-28), which
# killed both live analyser instances mid-window and froze test.csv. A tmux pane is a real
# terminal with a bounded scrollback, so the same failure costs nothing. This script therefore
# never uses `pipe-pane` on the analyser, and neither should you.
#
# WHAT "RESUMABLE" MEANS HERE, precisely:
#   * serial-log.py already rides out a reboot or a reflash -- the CDC device vanishing is a
#     detach, not an exit -- and --out APPENDS, so a.log keeps its history across restarts.
#   * Re-running `start` is idempotent: live panes are left strictly alone, only dead ones are
#     respawned. It is safe to run after every flash without thinking about it.
#   * test.csv is NOT append-only. `--out test.csv` truncates, so a restart destroys the window
#     you just measured; the analyser is archived to archive-test-<stamp>.csv before it starts.
#
# Ports are resolved by board identity (USB serial = MAC), never by /dev path: cu.usbmodemNNN
# and ttyACMn are both assigned in enumeration order and move between boards across a replug,
# which would put board B's lines in a.log with nothing in the log to say so.
#
#   scripts/bench/bench-tmux.sh discover     # what is plugged in, and write a starter boards.conf
#   scripts/bench/bench-tmux.sh start        # start/repair everything, idempotent
#   scripts/bench/bench-tmux.sh status       # is it actually capturing?
#   scripts/bench/bench-tmux.sh pins         # probe pins <-> board roles, the sign of B−A
#   scripts/bench/bench-tmux.sh flash a b    # OTA those boards, batched, capture left running
#   scripts/bench/bench-tmux.sh attach
#   scripts/bench/bench-tmux.sh stop
#
# PORTABILITY: stock /bin/bash on macOS is 3.2 (2007), so no associative arrays and no ${x,,}
# -- roles are a fixed set, so plain per-role variables do the job. stat/df/ping differ between
# BSD and GNU and are wrapped below. Every one of those wrappers is a real incompatibility,
# not defensive noise: `df -g` is macOS-only, `ping -t` is a timeout on BSD and a TTL on GNU.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "${REPO_ROOT}"

SESSION="${BENCH_SESSION:-bench}"
CONF="${BENCH_CONF:-scripts/bench/boards.conf}"
MIN_FREE_GB="${BENCH_MIN_FREE_GB:-20}"
ROLES="a b observer"
WANT_ANALYZER=1
DRY_RUN=0
FLASH_ROLES=""

# The analyser invocation, from HANDOFF.md. --samples 200000 is load-bearing: a run that
# dropped it came back with rival = 0.942 on every row, i.e. whole-frame errors masquerading
# as findings. --plot-every/--plot-window are cosmetic; the rest is not.
SERVE_PORT="${BENCH_SERVE_PORT:-8000}"
ANALYZER_CMD="python3 scripts/i2s-skew.py --stream --interval 0 --count 0 \
--samplerate 12M --samples 200000 --plot-every 0.0167 --plot-window 45 \
--serve ${SERVE_PORT} --out test.csv --plot test.svg"

die() { echo "ERROR: $*" >&2; exit 1; }
note() { echo "==> $*"; }
lower() { tr '[:upper:]' '[:lower:]'; }

# ── BSD/GNU wrappers ──────────────────────────────────────────────────────────
# Decided ONCE by probing, not by try-BSD-then-GNU. `stat -f` exists in both and means
# different things: BSD reads it as a format string, GNU as --file-system. Given a file, GNU
# `stat -f %z a.log` PRINTS THE FILESYSTEM BLOCK to stdout and only then exits nonzero, so a
# `||` fallback still emits that text -- it reached `$(( ))` as "File: \"a.log\" ID: ..." and
# came back as `File: unbound variable`. A fallback whose wrong branch half-succeeds is worse
# than having none: it corrupts the output instead of failing over.
if stat --version >/dev/null 2>&1; then STAT=gnu; else STAT=bsd; fi

file_size() {
    if [ "${STAT}" = gnu ]; then stat -c %s "$1" 2>/dev/null || echo 0
    else stat -f %z "$1" 2>/dev/null || echo 0; fi
}

file_mtime() {
    if [ "${STAT}" = gnu ]; then stat -c %Y "$1" 2>/dev/null || echo 0
    else stat -f %m "$1" 2>/dev/null || echo 0; fi
}

file_time() {  # mtime as HH:MM:SS
    if [ "${STAT}" = gnu ]; then stat -c '%y' "$1" 2>/dev/null | cut -c12-19
    else stat -f '%Sm' -t '%H:%M:%S' "$1" 2>/dev/null; fi
}

# These logs span DAYS and carry no date, so a bare HH:MM:SS is not evidence of freshness --
# reading one as current when it was a previous day's build has already cost a session here.
# Print the age, which cannot be misread.
age_of() {
    local secs=$(( $(date +%s) - $(file_mtime "$1") ))
    if   [ "${secs}" -lt 90 ];    then printf '%ds ago' "${secs}"
    elif [ "${secs}" -lt 5400 ];  then printf '%dm ago' "$(( secs / 60 ))"
    elif [ "${secs}" -lt 172800 ]; then printf '%dh%dm ago' "$(( secs / 3600 ))" "$(( secs % 3600 / 60 ))"
    else printf '%dd ago — STALE' "$(( secs / 86400 ))"; fi
}

free_gb() { df -Pk . | awk 'NR==2 {printf "%d", $4/1048576}'; }  # -Pk is the portable form

# Is anything actually listening on this port? --serve failing to bind is deliberately
# non-fatal in the analyser (a busy port is no reason to lose a capture), so the flag being
# on the command line is NOT evidence that the plot is being served. Check the socket.
port_listening() {
    local port="$1"
    if command -v ss >/dev/null 2>&1; then
        ss -ltnH 2>/dev/null | grep -qE "[:.]${port}[[:space:]]"
    elif command -v lsof >/dev/null 2>&1; then
        lsof -nP -iTCP:"${port}" -sTCP:LISTEN >/dev/null 2>&1
    else
        return 2        # unknown, and say so rather than implying "no"
    fi
}

host_up() {  # BSD ping: -t <seconds>. GNU ping: -W <seconds>, and -t is a TTL.
    if [ "$(uname)" = Darwin ]; then ping -c 1 -t 2 "$1" >/dev/null 2>&1
    else ping -c 1 -W 2 "$1" >/dev/null 2>&1; fi
}

# ── board identity ────────────────────────────────────────────────────────────
# pyserial comes from esphome's own interpreter (it is an esphome dependency), so nothing
# extra needs installing; fall back to the system python3, which on Linux still works via
# ports.py's sysfs backend.
pyexe() {
    local p
    if command -v esphome >/dev/null 2>&1; then
        p=$(head -1 "$(command -v esphome)" | sed 's/^#!//')
        [ -x "${p}" ] && { echo "${p}"; return; }
    fi
    echo python3
}

# Per-role state, in plain variables so this runs under bash 3.2: SUF_a, LOG_a, PORT_a, ...
get() { eval "printf '%s' \"\${$1_$2:-}\""; }      # get SUF a
set_() { eval "$1_$2=\$3"; }                      # set_ SUF a e985e8

load_conf() {
    [ -f "${CONF}" ] || return 0
    local suffix role log cfg rest ok
    while read -r suffix role log cfg rest; do
        case "${suffix}" in ''|\#*) continue ;; esac
        [ -n "${role}" ] && [ -n "${log}" ] || die "${CONF}: bad line near '${suffix}'"
        ok=0
        for r in ${ROLES}; do [ "${role}" = "${r}" ] && ok=1; done
        [ "${ok}" = 1 ] || die "${CONF}: unknown role '${role}' (want one of: ${ROLES})"
        set_ SUF "${role}" "$(printf '%s' "${suffix}" | lower)"
        set_ LOG "${role}" "${log}"
        # Optional 4th field: the config THIS board runs. Named per board and never inferred
        # -- flashing the observer with a speaker config puts I2S back on its live DAC and
        # renames it, which is the documented way to destroy the observer. A board with no
        # config here simply cannot be flashed by this script.
        set_ CFG "${role}" "${cfg:-}"
    done < "${CONF}"
}

# ESPHome's name_add_mac_suffix appends the last three MAC bytes, which is exactly the suffix
# this script keys on -- so the OTA hostname is derivable from the config rather than being a
# fifth thing to keep in sync by hand.
ota_host() {   # <role> -> hostname, or empty
    local role="$1" cfg name
    cfg="$(get CFG "${role}")"
    [ -n "${cfg}" ] && [ -f "${cfg}" ] || return 0
    name="$(sed -n 's/^  name:[[:space:]]*\([A-Za-z0-9_-]\{1,\}\).*/\1/p' "${cfg}" | head -1)"
    [ -n "${name}" ] || return 0
    echo "${name}-$(get SUF "${role}").local"
}

# Fills PORT_<role> for every attached board named in boards.conf, and reports the rest.
resolve_ports() {
    local device suffix serial product role matched out rc=0
    local unknown=""
    # Run the enumerator ONCE and check its exit status. Through a process substitution its
    # failure is invisible: zero rows come back and every board reads as "not attached",
    # which is indistinguishable from an unplugged bench. A broken instrument must not be
    # able to look like a quiet one.
    out="$("$(pyexe)" scripts/bench/ports.py)" || rc=$?
    [ "${rc}" = 0 ] || die "port enumeration failed (ports.py exit ${rc}) — not treating this as 'no boards'"
    while IFS="$(printf '\t')" read -r device suffix serial product; do
        [ -n "${device}" ] || continue
        if [ -z "${suffix}" ]; then
            unknown="${unknown}
      ${device} (no MAC in USB descriptor: ${product:-unknown board})"
            continue
        fi
        matched=""
        for role in ${ROLES}; do
            [ "$(get SUF "${role}")" = "${suffix}" ] && { set_ PORT "${role}" "${device}"; matched=1; }
        done
        [ -n "${matched}" ] || unknown="${unknown}
      ${device} suffix=${suffix} (not in ${CONF})"
    done <<EOF
${out}
EOF

    if [ -n "${unknown}" ]; then
        # Reported, never guessed. Assigning an unidentified board to a.log is precisely the
        # class of instrument error that makes every downstream number wrong and silent.
        echo "    unmapped ports (add them to ${CONF} to capture them):${unknown}" >&2
    fi
}

# ── tmux plumbing ─────────────────────────────────────────────────────────────
have_session() { tmux has-session -t "${SESSION}" 2>/dev/null; }

ensure_session() {
    have_session && return 0
    note "creating tmux session '${SESSION}'"
    # remain-on-exit keeps a dead pane and its last words on screen instead of collapsing the
    # window -- the difference between diagnosing a crash and finding an empty session.
    tmux new-session -d -s "${SESSION}" -n scratch -c "${REPO_ROOT}"
    tmux set-option -t "${SESSION}" history-limit 200000 >/dev/null
    tmux set-option -t "${SESSION}" -g pane-border-status top >/dev/null 2>&1 || true
    tmux set-option -t "${SESSION}" -g pane-border-format ' #{pane_title} ' >/dev/null 2>&1 || true
}

pane_id_for() {  # <window> <title> -> pane id, or empty
    local win="$1" title="$2" id t
    while IFS="$(printf '\t')" read -r id t; do
        [ "${t}" = "${title}" ] && { echo "${id}"; return; }
    done < <(tmux list-panes -t "${SESSION}:${win}" \
             -F "#{pane_id}$(printf '\t')#{pane_title}" 2>/dev/null)
    # "no such pane" is a normal answer, not a failure. Without this the loop falls out on the
    # failed read and returns 1, and under `set -e` the caller's id="$(pane_id_for ...)" takes
    # that status and kills the script -- silently, right after the first pane came up.
    return 0
}

pane_dead() { [ -n "$1" ] && [ "$(tmux display-message -p -t "$1" '#{pane_dead}' 2>/dev/null)" = 1 ]; }

# ensure_pane <window> <title> <cmd>: create it, or respawn it if it died, or leave a live
# one strictly alone. Leaving live panes alone is what makes `start` safe to re-run.
ensure_pane() {
    local win="$1" title="$2" cmd="$3" id
    if [ "${DRY_RUN}" = 1 ]; then echo "    would run [${win}/${title}]: ${cmd}"; return; fi

    if ! tmux list-windows -t "${SESSION}" -F '#{window_name}' | grep -qx "${win}"; then
        tmux new-window -d -t "${SESSION}" -n "${win}" -c "${REPO_ROOT}" "${cmd}"
        tmux select-pane -t "${SESSION}:${win}" -T "${title}"
        # remain-on-exit is a WINDOW option: setting it on the session target does not reach
        # windows made later, and a pane that died was then destroyed rather than held with
        # its last words on screen -- which is the whole reason for wanting it.
        tmux set-option -w -t "${SESSION}:${win}" remain-on-exit on >/dev/null
        note "started ${win}/${title}"
        return
    fi
    id="$(pane_id_for "${win}" "${title}")"
    if [ -z "${id}" ]; then
        id="$(tmux split-window -d -P -F '#{pane_id}' -t "${SESSION}:${win}" -c "${REPO_ROOT}" "${cmd}")"
        tmux select-pane -t "${id}" -T "${title}"
        tmux select-layout -t "${SESSION}:${win}" tiled >/dev/null
        note "started ${win}/${title}"
    elif pane_dead "${id}"; then
        tmux respawn-pane -k -t "${id}" -c "${REPO_ROOT}" "${cmd}"
        tmux select-pane -t "${id}" -T "${title}"
        note "respawned ${win}/${title} (it had died)"
    else
        note "${win}/${title} already running — left alone"
    fi
}

# ── the two capture kinds ─────────────────────────────────────────────────────
start_serial() {
    local role port log started=0
    for role in ${ROLES}; do
        port="$(get PORT "${role}")"
        log="$(get LOG "${role}")"
        [ -n "${log}" ] || continue
        if [ -z "${port}" ]; then
            echo "    ${role}: board $(get SUF "${role}") not attached — skipping ${log}" >&2
            continue
        fi
        # Two readers on one tty interleave partial lines into both logs, which reads as
        # corruption in the firmware rather than in the capture. Refuse rather than race.
        if pgrep -f "serial-log\.py .*${port##*/}" >/dev/null 2>&1 \
           && [ -z "$(pane_id_for logs "${role}")" ]; then
            echo "    ${role}: ${port} already held by a serial-log.py outside this session" >&2
            continue
        fi
        ensure_pane logs "${role}" "python3 scripts/serial-log.py ${port} --out ${log}"
        started=1
    done
    [ "${started}" = 1 ] || echo "    no serial loggers started" >&2
}

analyzer_preflight() {
    # Every one of these is a failure that has actually happened on this bench.
    pgrep -i -x pulseview >/dev/null 2>&1 && \
        die "PulseView is running; it holds the capture device and the analyser will error-loop"
    local free; free="$(free_gb)"
    [ "${free}" -ge "${MIN_FREE_GB}" ] || \
        die "only ${free} GB free (want ${MIN_FREE_GB}); a wedged analyser fills a disk fast"
    if pgrep -f 'i2s-skew\.py .*--stream' >/dev/null 2>&1 \
       && [ -z "$(pane_id_for analyzer analyzer)" ]; then
        die "an i2s-skew.py --stream is already running outside this session; two instances fight over the device"
    fi
}

start_analyzer() {
    local role log annotate="" id archive
    if [ -z "$(get LOG a)" ] || [ -z "$(get LOG b)" ]; then
        echo "    no A/B pair configured — skipping the analyser (it measures B−A)" >&2
        return
    fi
    # ORDER IS LOAD-BEARING: --annotate takes the first two logs as board A and board B, in
    # that order; anything after is parsed for events only. observer.log goes THIRD.
    # Every CONFIGURED log is annotated, whether or not it exists yet -- on a fresh host the
    # serial loggers have not written a byte at this point, and filtering on -f silently
    # handed the analyser `--annotate a.log` alone. It would then have run a whole session
    # with board B and the observer unparsed, which looks exactly like two boards that never
    # logged an event. Create the files instead (append-open, never truncating).
    for role in ${ROLES}; do
        log="$(get LOG "${role}")"
        [ -n "${log}" ] || continue
        [ "${DRY_RUN}" = 1 ] || [ -f "${log}" ] || : >> "${log}"
        annotate="${annotate} ${log}"
    done
    analyzer_preflight

    # Only archive when we are actually about to truncate: a live analyser is left alone by
    # ensure_pane, and archiving then would just litter.
    id="$(pane_id_for analyzer analyzer)"
    if { [ -z "${id}" ] || pane_dead "${id}"; } && [ -s test.csv ] && [ "${DRY_RUN}" != 1 ]; then
        archive="archive-test-$(date +%Y%m%d-%H%M%S).csv"
        cp test.csv "${archive}"
        note "archived test.csv -> ${archive} ($(wc -l < "${archive}" | tr -d ' ') rows); --out truncates"
    fi
    ensure_pane analyzer analyzer "${ANALYZER_CMD} --annotate${annotate}"
}

# ── subcommands ───────────────────────────────────────────────────────────────
cmd_discover() {
    local out device suffix serial product n=0
    note "attached ESP32 ports (VID 0x303A)"
    out="$("$(pyexe)" scripts/bench/ports.py)" || die "port enumeration failed (ports.py exit $?)"
    if [ -z "${out}" ]; then
        echo "    none — nothing is plugged in" >&2
        return 1
    fi
    printf '    %-28s %-8s %s\n' DEVICE SUFFIX SERIAL
    printf '%s\n' "${out}" | while IFS="$(printf '\t')" read -r device suffix serial product; do
        printf '    %-28s %-8s %s %s\n' "${device}" "${suffix:-?}" "${serial:-–}" "${product}"
    done

    # HOW A NEW BOARD GETS A ROLE. Only one of the three roles is knowable from software:
    #
    #   observer  -- it answers to snapclient-observer-<suffix>.local. That is evidence (the
    #                board was flashed with observer-supermini.yaml and says so), so it is
    #                written live.
    #   a / b     -- NOT knowable. Which board is A is a fact about which logic-analyser clip
    #                is on which board; USB and mDNS cannot see a probe wire. Getting it
    #                backwards silently negates every B−A number the wire produces, and
    #                nothing downstream would contradict it. So these are proposed COMMENTED
    #                OUT and you confirm them against the clips.
    #
    # Existing lines are never rewritten -- the roles you already confirmed are the one thing
    # in here that was checked against physical reality.
    [ -f "${CONF}" ] || printf '%s\n%s\n' \
        "# <mac-suffix>  <role: a|b|observer>  <logfile>" \
        "# role a is logic-analyser channel A; observer drives no DAC and emits PHASEIN." \
        > "${CONF}"

    local added=0 host
    printf '%s\n' "${out}" | while IFS="$(printf '\t')" read -r device suffix serial product; do
        [ -n "${suffix}" ] || { echo "    ${device}: no MAC in descriptor — pin it by hand in ${CONF}" >&2; continue; }
        grep -qiE "^[[:space:]]*#?[[:space:]]*${suffix}[[:space:]]" "${CONF}" && continue
        if host_up "snapclient-observer-${suffix}.local"; then
            if grep -qE '^[[:space:]]*[0-9a-fA-F]+[[:space:]]+observer[[:space:]]' "${CONF}"; then
                echo "# ${suffix}  observer  observer.log   # a second observer? one is already assigned" >> "${CONF}"
                echo "    ${suffix}: answers as an observer, but ${CONF} already has one — added commented" >&2
            else
                echo "${suffix}  observer  observer.log" >> "${CONF}"
                echo "    ${suffix}: observer (confirmed by mDNS) — assigned" >&2
            fi
        else
            host=""
            host_up "snapclient-supermini-${suffix}.local" && host=" # answers as snapclient-supermini-${suffix}"
            echo "# ${suffix}  a  a.log   # or b/b.log — CONFIRM against the analyser clips${host}" >> "${CONF}"
            echo "    ${suffix}: speaker — added COMMENTED; A vs B is a wiring fact, uncomment the right one" >&2
        fi
        added=1
    done

    echo
    note "${CONF}"
    cat "${CONF}"
    echo "Board A must be the one on logic-analyser channel A, or the wire sign flips." >&2
}

cmd_start() {
    [ -f "${CONF}" ] || die "no ${CONF}; run '$0 discover' first"
    load_conf
    resolve_ports
    ensure_session
    start_serial
    [ "${WANT_ANALYZER}" = 1 ] && start_analyzer
    echo
    cmd_status
    echo "    attach with: tmux attach -t ${SESSION}"
}

cmd_status() {
    have_session || { echo "session '${SESSION}' is not running"; return 1; }
    load_conf
    note "session '${SESSION}'"
    local win id title dead cmd role log f
    while IFS="$(printf '\t')" read -r win id title dead cmd; do
        printf '    %-9s %-10s %-8s %s\n' "${win}" "${title}" \
            "$([ "${dead}" = 1 ] && echo DEAD || echo live)" "$(printf '%.72s' "${cmd}")"
    done < <(tmux list-panes -s -t "${SESSION}" -F \
        "#{window_name}$(printf '\t')#{pane_id}$(printf '\t')#{pane_title}$(printf '\t')#{pane_dead}$(printf '\t')#{pane_start_command}")

    # Panes can be live while nothing is being captured -- a detached USB device, or an
    # analyser gone quiet on LIBUSB_ERROR_NO_DEVICE. Freshness is the real signal, so print
    # it rather than a green light that only proves a process exists.
    echo
    note "capture freshness (now $(date +%H:%M:%S))"
    for role in ${ROLES}; do
        log="$(get LOG "${role}")"; [ -n "${log}" ] || continue
        if [ -f "${log}" ]; then
            printf '    %-12s %7s MB  last line %s (%s)\n' "${log}" \
                "$(( $(file_size "${log}") / 1048576 ))" "$(file_time "${log}")" "$(age_of "${log}")"
        else
            printf '    %-12s %s\n' "${log}" "MISSING"
        fi
    done
    for f in test.csv test.svg; do
        [ -f "${f}" ] && printf '    %-12s %7s rows  last write %s (%s)\n' "${f}" \
            "$(wc -l < "${f}" | tr -d ' ')" "$(file_time "${f}")" "$(age_of "${f}")"
    done
    printf '    %-12s %s GB free\n' disk "$(free_gb)"

    # The live plot, reported from the RUNNING command rather than from this script's
    # defaults -- an analyser started before --serve existed, or with a different port, must
    # not be described by what we would launch today.
    local started port rc
    started="$(tmux list-panes -s -t "${SESSION}" -F '#{pane_title} #{pane_start_command}' \
               2>/dev/null | grep '^analyzer ' || true)"
    echo
    if [ -z "${started}" ]; then
        note "web plotter: no analyser pane"
        return 0
    fi
    port="$(printf '%s' "${started}" | sed -n 's/.*--serve[= ]\{1,\}\([0-9]\{1,\}\).*/\1/p')"
    if [ -z "${port}" ]; then
        note "web plotter: NOT served — the running analyser has no --serve"
        echo "    restart it to pick up the default: ${0##*/} stop && ${0##*/} start" >&2
        return 0
    fi
    # The analyser binds the port a few seconds into startup (it primes log baselines
    # first), so a single check right after `start` reports a bind failure that is really a
    # race. Give it a moment before saying so -- a wrong alarm here sends you to read the
    # pane for a fault that does not exist.
    rc=0; port_listening "${port}" || rc=$?
    if [ "${rc}" = 1 ]; then
        local i=0
        while [ "${i}" -lt 6 ]; do
            sleep 1; i=$((i + 1))
            rc=0; port_listening "${port}" || rc=$?
            [ "${rc}" = 1 ] || break
        done
    fi
    case "${rc}" in
        0) note "web plotter: http://127.0.0.1:${port}/  (listening)" ;;
        2) note "web plotter: --serve ${port} (no ss/lsof here — could not confirm it bound)" ;;
        *) note "web plotter: --serve ${port} but NOTHING IS LISTENING"
           echo "    the analyser warns and continues when the port is busy; check its pane" >&2 ;;
    esac
    # The server binds 127.0.0.1 only, so it is not reachable across the network by design.
    # Address taken from SSH_CONNECTION's server field (the address you actually reached
    # this box on), not from `hostname -f`, which returned "debian-work.debian-work" here.
    [ "${rc}" = 0 ] && [ -n "${SSH_CONNECTION:-}" ] && \
        echo "    from your workstation: ssh -N -L ${port}:127.0.0.1:${port} $(id -un)@$(echo "${SSH_CONNECTION}" | awk '{print $3}')"
    return 0
}

# The two bindings that decide the sign of every wire number, printed side by side because
# they are set in different files and NOTHING checks that they agree:
#
#   probe -> board   scripts/logic-analyzer.pvs, the _ONE group is board A and _TWO is B.
#                    The analyser reads this same file, so the pin map cannot drift from
#                    what it decodes -- but the file says which PIN, not which BOARD. Which
#                    board the _ONE clips are actually on is a fact about your bench.
#   board -> log     boards.conf role a/b, which sets the --annotate order, whose first log
#                    the analyser takes as board A.
#
# Get them inconsistent and B−A is negated: every offset, every skew, every sign in the
# plots. No log line contradicts it, because both halves are internally consistent.
cmd_pins() {
    local pvs="${BENCH_PVS:-scripts/logic-analyzer.pvs}" role suffix log
    [ -f "${pvs}" ] || die "no ${pvs}"
    load_conf

    note "probe pins (from ${pvs}, the same file the analyser reads)"
    # Mirrors parse_pvs() in i2s-skew.py: [Dn] section, then a name= line.
    awk '
        /^\[D[0-9]+\]$/ { ch = substr($0, 3, length($0) - 3); next }
        ch != "" && /^name=/ { name = substr($0, 6); map[name] = ch; ch = "" }
        END {
            split("DIN_ONE BCLK_ONE LRC_ONE DIN_TWO BCLK_TWO LRC_TWO", want, " ")
            for (i = 1; i <= 6; i++) {
                n = want[i]
                grp = (n ~ /_ONE$/) ? "board A" : "board B"
                if (n in map) printf "    %-10s D%-3s %s\n", n, map[n], grp
                else          printf "    %-10s %-4s %s  <-- MISSING; the analyser will refuse to start\n", n, "?", grp
            }
        }' "${pvs}"

    echo
    note "board roles (from ${CONF})"
    for role in a b observer; do
        suffix="$(get SUF "${role}")"; log="$(get LOG "${role}")"
        if [ -z "${suffix}" ]; then
            printf '    %-9s %s\n' "${role}" "unassigned"
        else
            printf '    %-9s %-8s -> %-13s %s\n' "${role}" "${suffix}" "${log}" \
                "$([ "${role}" = observer ] && echo '(no DAC, not probed)' || echo "probed on the _$([ "${role}" = a ] && echo ONE || echo TWO) pins")"
        fi
    done

    cat >&2 <<'EOF'

    Board A must be the board carrying the _ONE clips. Nothing here can verify that -- to
    confirm it, perturb ONE board and watch which half reacts: reboot the board you believe
    is A and its _ONE channels lose their bus for a second or two, while _TWO keeps running.
    Do it once, at setup; a reboot costs a consensus membership change, so not during a window.
EOF
}

# OTA the named boards, in ONE batch, without disturbing the capture.
#
# OTA rather than USB even though every board is on a tty here: serial flashing needs the port,
# so the logger holding it would have to be killed, and esptool drives DTR/RTS to force ROM
# download mode. Over OTA the serial loggers stay attached and ride the reboot as a
# detach/attach, so the logs keep their continuity across the flash -- which matters, because a
# capture gap is indistinguishable from a firmware that stopped emitting, and it has already
# turned a clean DECIDE reconciliation into a false MISMATCH on this bench.
#
# Roles must be named explicitly, or --all given. A bench flash costs ~5 consensus membership
# changes per board (|median error| 154 us within 15 s of one, against 93 us elsewhere), so
# flashing the fleet must be something you asked for, never a default.
cmd_flash() {
    local role host cfg configs="" hosts before after rc=0 any=0
    load_conf
    [ -n "${FLASH_ROLES}" ] || die "name the roles to flash (e.g. '$0 flash a b') or pass --all"

    # Resolve and check everything BEFORE flashing anything: a batch that dies half way leaves
    # the bench running two firmware eras, which looks like an inter-board difference.
    for role in ${FLASH_ROLES}; do
        cfg="$(get CFG "${role}")"
        [ -n "${cfg}" ] || die "${role}: no config in ${CONF} (4th field) — refusing to guess which firmware this board runs"
        [ -f "${cfg}" ] || die "${role}: config not found: ${cfg}"
        host="$(ota_host "${role}")"
        [ -n "${host}" ] || die "${role}: cannot derive a hostname from ${cfg} (no 'name:' substitution)"
        host_up "${host}" || die "${role}: ${host} does not answer — flash it over USB or fix the network first"
        set_ HOST "${role}" "${host}"
        any=1
    done
    [ "${any}" = 1 ] || die "nothing to flash"

    # The witness, before. compilation_time is what proves which build answered afterwards --
    # an OTA log saying "successful" does not, since a replug inside the first minute rolls
    # both halves back and the log still reads clean.
    for role in ${FLASH_ROLES}; do
        before="$("$(pyexe)" scripts/bench/device-info.py "$(get HOST "${role}")" 2>/dev/null | cut -f2)"
        set_ WAS "${role}" "${before:-unknown}"
        printf '    %-9s %-34s running build %s\n' "${role}" "$(get HOST "${role}")" "${before:-UNREACHABLE (API)}"
    done
    if [ "${DRY_RUN}" = 1 ]; then
        for role in ${FLASH_ROLES}; do
            echo "    would flash ${role}: $(get CFG "${role}") -> $(get HOST "${role}")"
        done
        return 0
    fi

    # One build per distinct config, all of that config's boards in a single parallel run.
    for role in ${FLASH_ROLES}; do
        cfg="$(get CFG "${role}")"
        case " ${configs} " in *" ${cfg} "*) continue ;; esac
        configs="${configs} ${cfg}"
    done
    for cfg in ${configs}; do
        hosts=""
        for role in ${FLASH_ROLES}; do
            [ "$(get CFG "${role}")" = "${cfg}" ] && hosts="${hosts} $(get HOST "${role}")"
        done
        note "flashing${hosts} with ${cfg}"
        # --no-log: the serial loggers are already capturing, and a second log stream over the
        # API is airtime spent on the thing under measurement.
        ./scripts/flash.sh -c "${cfg}" --docker --no-log -p ${hosts} || rc=$?
        [ "${rc}" = 0 ] || die "flash.sh failed for ${cfg} (exit ${rc}); the bench may now be running two firmware eras"
    done

    # The witness, after. Wait for each board to answer again rather than sleeping blind.
    echo
    note "verifying the running build (device_info compilation_time)"
    for role in ${FLASH_ROLES}; do
        host="$(get HOST "${role}")"
        after=""
        local i=0
        while [ "${i}" -lt 30 ]; do
            after="$("$(pyexe)" scripts/bench/device-info.py "${host}" 2>/dev/null | cut -f2)"
            [ -n "${after}" ] && break
            i=$((i + 1)); sleep 2
        done
        if [ -z "${after}" ]; then
            printf '    %-9s %-34s NO ANSWER — verify by hand before trusting any measurement\n' "${role}" "${host}"
            rc=1
        elif [ "${after}" = "$(get WAS "${role}")" ]; then
            printf '    %-9s %-34s UNCHANGED (%s) — the OTA did not take\n' "${role}" "${host}" "${after}"
            rc=1
        else
            printf '    %-9s %-34s now %s\n' "${role}" "${host}" "${after}"
        fi
    done
    cat >&2 <<'EOF'

    Do NOT replug a board for the next minute: a replug that soon after the OTA reboot rolls
    both halves back to the previous firmware while the OTA log still says "successful".
    Every board just rebooted, which is ~5 consensus membership changes each -- expect
    elevated error for ~15 s and do not grade a window that overlaps it.
EOF
    return "${rc}"
}

cmd_stop() {
    have_session || { echo "session '${SESSION}' is not running"; return 0; }
    tmux kill-session -t "${SESSION}"
    note "killed '${SESSION}'; logs are intact and append on the next start"
}

cmd_attach() { have_session || die "session '${SESSION}' is not running"; exec tmux attach -t "${SESSION}"; }

command -v tmux >/dev/null || die "tmux not on PATH"

CMD=start
while [ $# -gt 0 ]; do
    case "$1" in
        start|status|stop|attach|discover|pins) CMD="$1"; shift ;;
        flash) CMD=flash; shift ;;
        --all) FLASH_ROLES="${ROLES}"; shift ;;
        a|b|observer) FLASH_ROLES="${FLASH_ROLES} $1"; shift ;;
        --no-analyzer) WANT_ANALYZER=0; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        --session) SESSION="${2:?--session requires an argument}"; shift 2 ;;
        -h|--help) sed -n '2,32p' "$0"; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done

"cmd_${CMD}"
