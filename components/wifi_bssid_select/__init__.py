"""Preferred-AP (BSSID) picker.

Nothing here is Snapcast-specific. Pinning an AP removes mid-stream roams -- a roam scan
stalls the radio long enough to starve synchronized audio, and a synchronized group wants
every member on one AP so they share a TSF timebase -- but the behaviour helps anything
that suffers from roaming.

The important design point is that the pin is a PREFERENCE, not a requirement.
wifi's set_bssid() refuses to associate with anything else, so a hard pin on an AP
that is gone, overloaded, or out of range leaves the device unable to connect at all
rather than merely degraded (observed in the field: a client stuck Unavailable after
its preferred AP was pinned). This entity therefore drops the constraint after a
timeout and re-arms it on the next disconnect, so a working link is never sacrificed
to chase the preferred AP.
"""

CODEOWNERS = ["@W-Floyd"]
DEPENDENCIES = ["wifi"]
