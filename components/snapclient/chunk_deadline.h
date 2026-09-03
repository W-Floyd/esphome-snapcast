#pragma once

#include <cstdint>

namespace esphome::snapclient {

/// @brief The local-clock deadline for a chunk, or nothing.
///
/// Pure so it can be tested on the host: the deadline is where the timebase becomes audible, and
/// the rule it enforces has no other home. `have_offset` says whether ANY server<->local offset
/// exists -- the shared TSF mapping or a seeded Kalman. When it does not, this returns false and
/// leaves `deadline_us` untouched.
///
/// ABSENT IS NOT ZERO. Answering with offset 0 does not mean "the clocks agree"; it means the
/// server timestamp is compared against the local clock with no domain conversion at all, so the
/// deadline is wrong by the WHOLE difference between the two domains. On the bench that was
/// -162083994757 us (-45 h, equal to the board's own TSF base) with 64118 frames corrected in one
/// report as the coarse path acted on it.
///
/// The window is every reconnect: connect_socket_() resets the Kalman, so no estimate exists until
/// the first Time reply. It stayed invisible because the shared mapping normally answers first --
/// the board that reconnected MOST often never saw it. That is the trap: the masking fallback is
/// gone exactly when the timebase is already in trouble, which is when the deadline matters most.
inline bool chunk_deadline(int64_t server_ts_us, int64_t buffer_us, bool have_offset, int64_t offset_us,
                           int64_t bias_us, int64_t &deadline_us) {
  if (!have_offset) {
    return false;
  }
  deadline_us = server_ts_us + buffer_us - offset_us + bias_us;
  return true;
}

}  // namespace esphome::snapclient
