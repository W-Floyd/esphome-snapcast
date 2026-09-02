#!/usr/bin/env bash
# Host test for the timing engine. Lives outside components/ because ESPHome compiles every .cpp
# in a component directory, and this one has a main().
set -euo pipefail
cd "$(dirname "$0")/../.."
c++ -std=c++17 -O0 -Wall -Wextra -o /tmp/timing_test \
    tests/timing/test_timing_engine.cpp components/snapclient/timing_engine.cpp
exec /tmp/timing_test
