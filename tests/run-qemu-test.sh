#!/usr/bin/env bash
# End-to-end integration test: real snapclient firmware on an emulated ESP32 (espressif
# QEMU + OpenCores ethernet) against a local snapserver streaming a FLAC-encoded sine.
#
# Requires: esphome, snapserver (brew install snapcast), ffmpeg, and espressif QEMU
# (installed automatically below via ESPHome's cached esp-idf tools on first run).
#
# Watch the serial log for:
#   [snapclient.client] Sync error: avg X us, peak Y us   <- should converge to ~1 ms avg
#   [virtual_speaker]   Consumed N frames, underruns 0
set -euo pipefail
cd "$(dirname "$0")"

WORK=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null || true; rm -rf "$WORK"' EXIT

echo "--- snapserver ---"
mkfifo "$WORK/snapfifo"
cat > "$WORK/snapserver.conf" <<EOF
[stream]
source = pipe://$WORK/snapfifo?name=test&sampleformat=48000:16:2&codec=flac
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
esphome compile qemu-test.yaml

echo "--- flash image ---"
cp .esphome/build/snapclient-qemu/build/firmware.factory.bin "$WORK/flash.bin"
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
