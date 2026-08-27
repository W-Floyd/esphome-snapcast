"""Package entry point libsigrokdecode requires.

The decoder DIRECTORY is imported as a package and the loader looks for `Decoder` on it, so
without this file the decoder silently never appears in PulseView's list -- which is exactly
how it presented: no error in the UI, just absent. `sigrok-cli -L` is what surfaces the real
message ("no 'Decoder' attribute in imported module").
"""

from .pd import Decoder
