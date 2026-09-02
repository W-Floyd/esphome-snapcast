#!/usr/bin/env bash
# Host test for the MCLK fraction arithmetic and the dither. Outside components/ because ESPHome
# compiles every .cpp in a component directory and this one has a main().
set -euo pipefail
cd "$(dirname "$0")/../.."
c++ -std=c++17 -O2 -Wall -Wextra -o /tmp/frac_test tests/rate_lock/test_frac_select.cpp
exec /tmp/frac_test
