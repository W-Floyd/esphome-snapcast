#!/usr/bin/env bash
# End-to-end integration test: real snapclient firmware on an emulated ESP32 (espressif
# QEMU + OpenCores ethernet) against a local snapserver streaming an encoded sine.
#
# Requires: esphome, snapserver (brew install snapcast), ffmpeg, and espressif QEMU
# (installed automatically below via ESPHome's cached esp-idf tools on first run).
#
#   ./run-qemu-test.sh             FLAC (snapserver's default codec)
#   CODEC=pcm ./run-qemu-test.sh   uncompressed
#   CODEC=opus ./run-qemu-test.sh  Opus; builds the separate opus config, which turns on
#                                  QEMU's emulated PSRAM because libopus needs it
#
# Watch the serial log for:
#   [virtual_speaker]   Consumed N frames, underruns 0, signal ~440 Hz
#
# Sync accuracy is NOT what this validates -- the median error wanders several ms under
# QEMU and does not converge, identically for every codec, because the emulated timebase
# and virtual DAC feedback are not the thing being modelled. Use real hardware and
# scripts/raw-sync.py for that.
#
# The signal line is the decode check: the source is a 440 Hz sine, so a codec that
# decodes to plausible-looking garbage still fails it where a frame count would not.
#
# Give QEMU a quiet host. Under load (a parallel esphome compile will do it) the guest
# takes an interrupt-watchdog panic with both cores parked in the idle task, and on the
# PSRAM config it then boot-loops rather than recovering. That is the emulator missing
# its deadline, not a firmware bug -- rerun it idle before believing a crash.
set -euo pipefail
cd "$(dirname "$0")"

CODEC=${CODEC:-flac}
case "$CODEC" in
  opus) CONFIG=qemu-opus-test.yaml; NAME=snapclient-qemu-opus ;;
  flac|pcm) CONFIG=qemu-test.yaml; NAME=snapclient-qemu ;;
  *) echo "unknown CODEC '$CODEC' (flac, pcm, opus)" >&2; exit 1 ;;
esac

WORK=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null || true; rm -rf "$WORK"' EXIT

echo "--- snapserver ---"
mkfifo "$WORK/snapfifo"
cat > "$WORK/snapserver.conf" <<EOF
[stream]
source = pipe://$WORK/snapfifo?name=test&sampleformat=48000:16:2&codec=$CODEC
buffer = 1000
[http]
enabled = false
[tcp]
enabled = false
EOF
snapserver -c "$WORK/snapserver.conf" > "$WORK/snapserver.log" 2>&1 &
ffmpeg -re -f lavfi -i "sine=frequency=440:sample_rate=48000" -ac 2 -f s16le -t 3600 - \
  > "$WORK/snapfifo" 2>/dev/null &

echo "--- firmware ---"
esphome compile "$CONFIG"

echo "--- flash image ---"
cp ".esphome/build/$NAME/build/firmware.factory.bin" "$WORK/flash.bin"
python3 - "$WORK/flash.bin" <<'EOF'
import sys
with open(sys.argv[1], "r+b") as f:
    f.seek(0, 2)
    f.write(b"\xff" * (4 * 1024 * 1024 - f.tell()))
EOF

echo "--- qemu ---"
IDF_CACHE=~/Library/Caches/esphome/idf
QEMU=$(find "$IDF_CACHE/tools/qemu-xtensa" -name qemu-system-xtensa -type f 2>/dev/null | head -1)
if [ -z "$QEMU" ]; then
  IDF_TOOLS_PATH=$IDF_CACHE python3 "$IDF_CACHE"/frameworks/*/tools/idf_tools.py install qemu-xtensa
  QEMU=$(find "$IDF_CACHE/tools/qemu-xtensa" -name qemu-system-xtensa -type f | head -1)
fi

echo "Serial output follows (ctrl-a x to quit qemu):"
"$QEMU" -nographic -M esp32 -m 4M \
  -drive "file=$WORK/flash.bin,if=mtd,format=raw" \
  -nic user,model=open_eth \
  -monitor null -serial stdio
