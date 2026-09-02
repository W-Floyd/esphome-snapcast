#!/usr/bin/env bash
# Two-board group simulator. No hardware, no ESPHome.
set -euo pipefail
cd "$(dirname "$0")/../.."
out=$(mktemp -d)/sim
c++ -std=c++17 -O1 -Wall -Wextra -o "$out" \
    tests/group/sim_group.cpp components/snapclient/timing_engine.cpp
"$out"
