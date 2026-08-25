#!/usr/bin/env bash
./scripts/flash.sh -c example/esp32-s3-supermini.yaml --docker --no-log -p \
	snapclient-supermini-e985e8.local snapclient-supermini-f04d74.local
./scripts/flash.sh -c example/m5stamps3-bat.yaml --docker --no-log \
	snapclient-stamps3-a56b60.local
exit
