#!/usr/bin/env bash
# ITERATION flash: the two wire-test speakers only -- one build, two OTAs, two membership
# changes instead of five. Use ./reflash.sh for fleet-wide changes (observer/stamps3 configs,
# or anything touching tsf consensus behaviour, where a mixed-build group would confound).
./scripts/flash.sh -c example/esp32-s3-supermini.yaml --docker --no-log -p \
	snapclient-supermini-e985e8.local snapclient-supermini-f04d74.local
