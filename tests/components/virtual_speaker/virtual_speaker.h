#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32

#include "esphome/components/ring_buffer/ring_buffer.h"
#include "esphome/components/speaker/speaker.h"
#include "esphome/core/component.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <memory>

namespace esphome::virtual_speaker {

/// @brief Speaker with no hardware output for QEMU/emulation testing.
///
/// A consumer task drains the play() buffer at exactly the configured sample rate
/// (wall-clock governed, so it never drifts) and fires the audio_output_callback with
/// the frames consumed and the consumption timestamp — the same feedback contract an
/// I2S DAC speaker provides, which downstream components use for synchronization.
/// Logs consumption/underrun stats every few seconds.
class VirtualSpeaker final : public Component, public speaker::Speaker {
 public:
  void setup() override;
  void dump_config() override;

  void set_stream_params(uint32_t sample_rate, uint8_t channels) {
    this->sample_rate_ = sample_rate;
    this->channels_ = channels;
  }

  size_t play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) override;
  size_t play(const uint8_t *data, size_t length) override { return this->play(data, length, 0); }

  void start() override;
  void stop() override;
  bool has_buffered_data() const override { return this->ring_ != nullptr && this->ring_->available() > 0; }

 protected:
  static void consumer_task_trampoline(void *arg);
  void consumer_task_();
  /// Accumulates zero crossings and peak amplitude for the left channel.
  void measure_signal_(const uint8_t *data, uint32_t frames, uint32_t frame_bytes);

  uint32_t sample_rate_{48000};
  uint8_t channels_{2};

  std::unique_ptr<ring_buffer::RingBuffer> ring_;
  TaskHandle_t task_handle_{nullptr};
  std::atomic<bool> running_{false};

  // Stats (consumer task only)
  int64_t consumed_frames_{0};
  int64_t underrun_frames_{0};
  // Signal check over the current stats window. Frame counts alone cannot tell a
  // correct decode from a plausible-looking wrong one, so the discarded audio is
  // measured on the way past: zero crossings give the fundamental, and the peak
  // separates silence from signal. The test source is a 440 Hz sine.
  int16_t signal_prev_{0};
  int16_t signal_peak_{0};
  uint32_t signal_crossings_{0};
  int64_t signal_frames_{0};
};

}  // namespace esphome::virtual_speaker

#endif  // USE_ESP32
