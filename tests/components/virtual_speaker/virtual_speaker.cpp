#include "virtual_speaker.h"

#ifdef USE_ESP32

#include "esphome/core/log.h"

#include <esp_timer.h>

#include <algorithm>
#include <cinttypes>
#include <cstring>

namespace esphome::virtual_speaker {

static const char *const TAG = "virtual_speaker";

// 20 ms consumption granularity; stats every ~5 s
static constexpr uint32_t TICK_MS = 20;
static constexpr uint32_t STATS_EVERY_TICKS = 250;

void VirtualSpeaker::setup() {
  this->audio_stream_info_ = audio::AudioStreamInfo(16, this->channels_, this->sample_rate_);
  // 500 ms of buffer, matching a typical I2S speaker configuration
  const size_t buffer_size = this->audio_stream_info_.ms_to_bytes(500);
  this->ring_ = ring_buffer::RingBuffer::create(buffer_size);
  if (this->ring_ == nullptr) {
    this->mark_failed();
    return;
  }
  if (xTaskCreate(VirtualSpeaker::consumer_task_trampoline, "vspeaker", 4096, this, 10, &this->task_handle_) !=
      pdPASS) {
    this->mark_failed();
  }
}

void VirtualSpeaker::dump_config() {
  ESP_LOGCONFIG(TAG, "Virtual Speaker: %" PRIu32 " Hz, 16 bit, %u ch", this->sample_rate_, this->channels_);
}

size_t VirtualSpeaker::play(const uint8_t *data, size_t length, TickType_t ticks_to_wait) {
  if (this->ring_ == nullptr) {
    return 0;
  }
  if (!this->running_.load(std::memory_order_relaxed)) {
    this->start();
  }
  return this->ring_->write_without_replacement(data, length, ticks_to_wait);
}

void VirtualSpeaker::start() {
  this->running_.store(true, std::memory_order_relaxed);
  this->state_ = speaker::STATE_RUNNING;
}

void VirtualSpeaker::stop() {
  this->running_.store(false, std::memory_order_relaxed);
  this->state_ = speaker::STATE_STOPPED;
  if (this->ring_ != nullptr) {
    this->ring_->reset();
  }
}

void VirtualSpeaker::consumer_task_trampoline(void *arg) { static_cast<VirtualSpeaker *>(arg)->consumer_task_(); }

void VirtualSpeaker::consumer_task_() {
  const uint32_t frame_bytes = this->channels_ * 2;
  uint8_t discard[2048];
  int64_t epoch_start_us = 0;
  int64_t epoch_consumed_frames = 0;
  uint32_t ticks = 0;

  while (true) {
    vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    const int64_t now = esp_timer_get_time();

    if (!this->running_.load(std::memory_order_relaxed)) {
      epoch_start_us = 0;
      continue;
    }
    if (epoch_start_us == 0) {
      epoch_start_us = now;
      epoch_consumed_frames = 0;
      continue;
    }

    // Wall-clock-governed consumption: exactly sample_rate frames per second since
    // the epoch started, so the virtual DAC never drifts against esp_timer.
    const int64_t due_total = (now - epoch_start_us) * this->sample_rate_ / 1000000;
    int64_t due = due_total - epoch_consumed_frames;
    uint32_t consumed_now = 0;

    while (due > 0) {
      const size_t want = std::min<size_t>(due * frame_bytes, sizeof(discard));
      const size_t got = this->ring_->read(discard, want, 0);
      if (got == 0) {
        // Underrun: a real DAC would emit silence for these frames
        this->underrun_frames_ += due;
        epoch_consumed_frames += due;
        break;
      }
      const uint32_t got_frames = got / frame_bytes;
      consumed_now += got_frames;
      epoch_consumed_frames += got_frames;
      due -= got_frames;
    }

    if (consumed_now > 0) {
      this->consumed_frames_ += consumed_now;
      this->audio_output_callback_.call(consumed_now, now);
    }

    if (++ticks >= STATS_EVERY_TICKS) {
      ticks = 0;
      ESP_LOGD(TAG, "Consumed %" PRId64 " frames, underruns %" PRId64 " frames, buffered %zu bytes",
               this->consumed_frames_, this->underrun_frames_, this->ring_->available());
    }
  }
}

}  // namespace esphome::virtual_speaker

#endif  // USE_ESP32
