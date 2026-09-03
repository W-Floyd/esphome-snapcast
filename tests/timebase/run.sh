#!/usr/bin/env bash
# Host test for the chunk deadline's timebase rule. Lives outside components/ because ESPHome
# compiles every .cpp in a component directory, and this one has a main().
set -euo pipefail
cd "$(dirname "$0")/../.."
c++ -std=c++17 -O0 -Wall -Wextra -o /tmp/timebase_test tests/timebase/test_chunk_deadline.cpp
exec /tmp/timebase_test
