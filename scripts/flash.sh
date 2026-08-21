#!/usr/bin/env bash
# Build-and-flash helper, adapted from speakeasy's ota-flash-snapclient.sh for
# ESPHome firmware: builds with the host esphome (or the Docker image), then
# flashes over USB (auto-detected ESP32 port) or ESPHome OTA to one or more devices.
set -euo pipefail

usage() {
    cat <<EOF
Usage: $(basename "$0") [-c config.yaml] [--docker] <device> [device...]
       $(basename "$0") [-c config.yaml] [--docker] --usb [--port <port>]

Build the config, then flash it.

OTA mode (default):
  Flash via ESPHome OTA to one or more devices (hostname or IP).

USB mode (--usb):
  Flash via serial to a USB-connected device (port auto-detected by
  Espressif VID 0x303A if omitted).

Options:
  -c, --config <yaml>  config to build/flash (default: example/esp32-s3-supermini.yaml)
  --docker             build with ghcr.io/esphome/esphome instead of host esphome
                       (flashing still uses host esphome — Docker Desktop on macOS
                       cannot pass USB devices through)
  -p, --parallel       flash all OTA devices concurrently
  --usb                serial flash mode
  --port <port>        serial port for USB mode (auto-detected if omitted)
  --no-log             don't stream device logs after flashing (logs stream by
                       default when flashing a single device)
  -h, --help           show this help
EOF
    exit 1
}

CONFIG="example/esp32-s3-supermini.yaml"
DOCKER=0
USB=0
PORT=""
LOG=1
PARALLEL=0

ARGS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        -c|--config) CONFIG="${2:?--config requires an argument}"; shift 2 ;;
        --docker) DOCKER=1; shift ;;
        -p|--parallel) PARALLEL=1; shift ;;
        --usb) USB=1; shift ;;
        --port) PORT="${2:?--port requires an argument}"; shift 2 ;;
        --log) LOG=1; shift ;;  # kept for compatibility; logging is the default
        --no-log) LOG=0; shift ;;
        -h|--help) usage ;;
        --) shift; ARGS+=("$@"); break ;;
        -*) echo "ERROR: unknown option $1" >&2; usage ;;
        *) ARGS+=("$1"); shift ;;  # devices may be interleaved with options
    esac
done
set -- ${ARGS[@]+"${ARGS[@]}"}

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "${REPO_ROOT}"
[[ -f "${CONFIG}" ]] || { echo "ERROR: config not found: ${CONFIG}" >&2; exit 1; }

if [[ "${USB}" -eq 0 ]]; then
    [[ $# -lt 1 ]] && usage
    DEVICES=("$@")
fi

command -v esphome >/dev/null || { echo "ERROR: esphome not found on PATH" >&2; exit 1; }

# Detect the first ESP32 USB serial port by Espressif VID (0x303A), from speakeasy.
# pyserial comes from esphome's own interpreter (it's an esphome dependency), so no
# extra install is needed; falls back to system python3, then to a device-name glob.
detect_port() {
    local esphome_py rc result
    esphome_py=$(head -1 "$(command -v esphome)" | sed 's/^#!//')
    for py in "${esphome_py}" python3; do
        [[ -x "${py}" ]] || command -v "${py}" >/dev/null 2>&1 || continue
        rc=0
        result=$("${py}" - <<'PYEOF'
import sys
try:
    import serial.tools.list_ports
except ImportError:
    sys.exit(3)
ports = [p for p in serial.tools.list_ports.comports() if p.vid == 0x303A]
if not ports:
    sys.exit("ERROR: no ESP32 device found (VID 0x303A) — connect via USB or use --port")
if len(ports) > 1:
    names = ', '.join(p.device for p in ports)
    print(f"WARNING: multiple ESP32 devices found ({names}), using first", file=sys.stderr)
print(ports[0].device)
PYEOF
        ) || rc=$?
        if [[ ${rc} -eq 0 ]]; then
            echo "${result}"
            return 0
        elif [[ ${rc} -ne 3 ]]; then
            # pyserial was present but detection genuinely failed (error already on stderr)
            return 1
        fi
    done
    # No pyserial anywhere: fall back to common ESP device names
    local dev
    for dev in /dev/cu.usbmodem* /dev/ttyACM* /dev/ttyUSB*; do
        [[ -e "${dev}" ]] && { echo "${dev}"; return 0; }
    done
    echo "ERROR: no serial device found — connect via USB or use --port" >&2
    return 1
}

# ── Build ─────────────────────────────────────────────────────────────────────

echo "==> Building ${CONFIG}..."
if [[ "${DOCKER}" -eq 1 ]]; then
    # Mount the repo root so `external_components: path: ../components` resolves
    docker run --rm -v "${REPO_ROOT}":/config ghcr.io/esphome/esphome compile "${CONFIG}"
else
    esphome compile "${CONFIG}"
fi

# ── Resolve the firmware binary ───────────────────────────────────────────────

# A Docker build records container-side paths (/config/...) in esphome's storage
# JSON, and the host `esphome upload` reads the firmware path straight out of it.
# Translate that prefix back to the repo root so host flashing never chases a
# /config path that only exists inside the image.
#
# CRITICAL: the transport needs DIFFERENT artifacts, and firmware_bin_path is the
# app-only image. Serial flashing writes at offset 0x0 and therefore needs
# firmware.factory.bin (bootloader + partition table + app); handing it the
# app-only image writes an app where the bootloader belongs, and the ROM then
# loads it, fails the image SHA-256 check, "boots anyway" and watchdog-loops
# forever (~1.3 s per cycle). OTA takes the app image.
resolve_bin() {
    local storage="$(dirname "${CONFIG}")/.esphome/storage/$(basename "${CONFIG}").json"
    [[ -f "${storage}" ]] || return 1
    python3 - "${storage}" "${REPO_ROOT}" <<'PYEOF'
import json, sys
storage, root = sys.argv[1], sys.argv[2]
path = json.load(open(storage)).get("firmware_bin_path", "")
if path.startswith("/config/"):
    path = root + path[len("/config"):]
if not path:
    sys.exit(1)
print(path)
PYEOF
}

# OTA takes the app image, which is exactly firmware_bin_path (path-translated).
OTA_FILE_ARG=()
if OTA_BIN=$(resolve_bin) && [[ -f "${OTA_BIN}" ]]; then
    OTA_FILE_ARG=(--file "${OTA_BIN}")
else
    echo "WARNING: could not resolve the OTA image; letting esphome pick it" >&2
fi

# Serial takes the factory image from the same build directory.
USB_FILE_ARG=()
if [[ -n "${OTA_BIN:-}" ]]; then
    FACTORY_BIN="$(dirname "${OTA_BIN}")/firmware.factory.bin"
    if [[ -f "${FACTORY_BIN}" ]]; then
        USB_FILE_ARG=(--file "${FACTORY_BIN}")
    else
        echo "WARNING: ${FACTORY_BIN} not found; letting esphome pick the serial image" >&2
    fi
fi

# ── Flash ─────────────────────────────────────────────────────────────────────

if [[ "${USB}" -eq 1 ]]; then
    if [[ -z "${PORT}" ]]; then
        echo "==> Auto-detecting ESP32 USB port..."
        PORT=$(detect_port)
        echo "    found: ${PORT}"
    fi
    echo "==> Flashing via ${PORT}..."
    # Serial writes at 0x0: factory image only, never the app-only one
    esphome upload "${CONFIG}" --device "${PORT}" ${USB_FILE_ARG[@]+"${USB_FILE_ARG[@]}"}
    if [[ "${LOG}" -eq 1 ]]; then
        exec esphome logs "${CONFIG}" --device "${PORT}"
    fi
    exit 0
fi

flash_device() {
    local device="$1"
    echo ""
    echo "══ ${device} ════════════════════════════════════════════"
    esphome upload "${CONFIG}" --device "${device}" ${OTA_FILE_ARG[@]+"${OTA_FILE_ARG[@]}"}
}

FAILED=()
if [[ "${PARALLEL}" -eq 1 && ${#DEVICES[@]} -gt 1 ]]; then
    PIDS=()
    OUT_FILES=()
    for device in "${DEVICES[@]}"; do
        outfile=$(mktemp)
        OUT_FILES+=("${outfile}")
        flash_device "${device}" >"${outfile}" 2>&1 &
        PIDS+=($!)
    done
    for i in "${!DEVICES[@]}"; do
        wait "${PIDS[i]}" && rc=0 || rc=$?
        cat "${OUT_FILES[i]}"
        rm -f "${OUT_FILES[i]}"
        [[ ${rc} -ne 0 ]] && FAILED+=("${DEVICES[i]}")
    done
else
    for device in "${DEVICES[@]}"; do
        flash_device "${device}" || FAILED+=("${device}")
    done
fi

echo ""
if [[ ${#FAILED[@]} -eq 0 ]]; then
    echo "==> All devices flashed successfully."
else
    echo "==> Completed with failures:" >&2
    for d in "${FAILED[@]}"; do echo "    ${d}" >&2; done
    exit 1
fi

if [[ "${LOG}" -eq 1 && ${#DEVICES[@]} -eq 1 ]]; then
    exec esphome logs "${CONFIG}" --device "${DEVICES[0]}"
fi
