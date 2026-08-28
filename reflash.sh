#!/usr/bin/env bash
./scripts/flash.sh -c example/esp32-s3-supermini.yaml --docker --no-log -p \
	snapclient-supermini-e985e8.local snapclient-supermini-f04d74.local
# e99574 is NOT here: it is the TSF observer and is flashed below with observer-supermini.yaml.
# Flashing it as a speaker puts I2S back on GPIO4/5/6 into its live DAC, drops tsf_observer, and
# renames it -- after which the observer line below cannot resolve it.
./scripts/flash.sh -c example/esp32-s3-supermini.yaml --docker --no-log -p \
	snapclient-supermini-f049c8.local
./scripts/flash.sh -c example/m5stamps3-bat.yaml --docker --no-log \
	snapclient-stamps3-a56b60.local
./scripts/flash.sh -c example/observer-supermini.yaml --docker --no-log \
	snapclient-observer-e99574.local

exit
