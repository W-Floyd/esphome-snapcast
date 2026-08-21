#!/usr/bin/env bash
# Build-and-flash helper, adapted from speakeasy's ota-flash-snapclient.sh for
# ESPHome firmware: builds with the host esphome (or the Docker image), then
# flashes over USB (auto-detected ESP32 port) or ESPHome OTA to one or more devices.
set -euo pipefail

usage() {
    cat <<EOF
Usage: $(basename "$0") [-c config.yaml] [--docker] <device> [device...]
       $(basename "$0") [-c config.yaml] [--docker] --usb [--port <port>] [--log]

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
  --log                stream device logs after flashing
  -h, --help           show this help
EOF
    exit 1
}

CONFIG="example/esp32-s3-supermini.yaml"
DOCKER=0
USB=0
PORT=""
LOG=0
PARALLEL=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        -c|--config) CONFIG="${2:?--config requires an argument}"; shift 2 ;;
        --docker) DOCKER=1; shift ;;
        -p|--parallel) PARALLEL=1; shift ;;
        --usb) USB=1; shift ;;
        --port) PORT="${2:?--port requires an argument}"; shift 2 ;;
        --log) LOG=1; shift ;;
        -h|--help) usage ;;
        -*) echo "ERROR: unknown option $1" >&2; usage ;;
        *) break ;;
    esac
done

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "${REPO_ROOT}"
[[ -f "${CONFIG}" ]] || { echo "ERROR: config not found: ${CONFIG}" >&2; exit 1; }

if [[ "${USB}" -eq 0 ]]; then
    [[ $# -lt 1 ]] && usage
    DEVICES=("$@")
fi

command -v esphome >/dev/null || { echo "ERROR: esphome not found on PATH" >&2; exit 1; }

# Detect the first ESP32 USB serial port by Espressif VID (0x303A), from speakeasy.
detect_port() {
    python3 - <<'PYEOF'
import sys
try:
    import serial.tools.list_ports
except ImportError:
    sys.exit("ERROR: pyserial not installed — run: pip install pyserial")
ports = [p for p in serial.tools.list_ports.comports() if p.vid == 0x303A]
if not ports:
    sys.exit("ERROR: no ESP32 device found (VID 0x303A) — connect via USB or use --port")
if len(ports) > 1:
    names = ', '.join(p.device for p in ports)
    print(f"WARNING: multiple ESP32 devices found ({names}), using first", file=sys.stderr)
print(ports[0].device)
PYEOF
}

# ── Build ─────────────────────────────────────────────────────────────────────

echo "==> Building ${CONFIG}..."
if [[ "${DOCKER}" -eq 1 ]]; then
    # Mount the repo root so `external_components: path: ../components` resolves
    docker run --rm -v "${REPO_ROOT}":/config ghcr.io/esphome/esphome compile "${CONFIG}"
else
    esphome compile "${CONFIG}"
fi

# ── Flash ─────────────────────────────────────────────────────────────────────

if [[ "${USB}" -eq 1 ]]; then
    if [[ -z "${PORT}" ]]; then
        echo "==> Auto-detecting ESP32 USB port..."
        PORT=$(detect_port)
        echo "    found: ${PORT}"
    fi
    echo "==> Flashing via ${PORT}..."
    esphome upload "${CONFIG}" --device "${PORT}"
    if [[ "${LOG}" -eq 1 ]]; then
        exec esphome logs "${CONFIG}" --device "${PORT}"
    fi
    exit 0
fi

flash_device() {
    local device="$1"
    echo ""
    echo "══ ${device} ════════════════════════════════════════════"
    esphome upload "${CONFIG}" --device "${device}"
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
