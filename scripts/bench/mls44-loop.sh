#!/bin/sh
# Seamless MLS44 loop for the sync bench.
#
# The file stream hit EOF every 20 minutes (211678684 B / 176400 B/s = 1199.99 s) and snapserver
# transitioned MLS44 playing -> idle -> playing across a ~101 ms gap, which every client answered
# with a ~100 ms hard resync. Measured 2026-08-28 at :06:03/:26:03/:46:03 past the hour, on all
# three probed boards in the same second, with the server log showing:
#
#   10:06:02.614 [Error] (AsioStream) Error reading message in stream 'MLS44': End of file
#   10:06:02.615 [Info]  (PcmStream) State changed: MLS44, playing => idle
#   10:06:02.716 [Info]  (PcmStream) State changed: MLS44, idle => playing
#
# Padding the file to an exact chunk boundary would NOT help: the idle comes from read()->EOF, not
# from the 1316-byte partial tail. Only a source that never ends removes it, hence a pipe.
# `while cat ...; do :; done`, NOT `while :; do cat ...; done`. The second spins: when the reader
# closes the pipe, cat dies on SIGPIPE and the loop restarts it immediately, forever. Measured --
# a throwaway `| head -c 8` test left it burning 3.2% CPU inside the container until it was killed.
# This form loops on a CLEAN EOF (cat exits 0 -> continue) and exits on SIGPIPE or any read error,
# so removing the stream stops the script instead of leaving it spinning.
while cat /data/mls44.pcm; do :; done
