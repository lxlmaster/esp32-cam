#pragma once
#include <stddef.h>
#include <stdint.h>

// No camera/network dependencies: replace update() to plug in your own algorithm.
// Input is camera RGB565 (high byte first), before software colour correction.
struct MotionConfig {
  bool enabled = true;
  uint32_t sampleIntervalMs = 200;
  uint32_t cooldownMs = 15000;
  uint8_t pixelThreshold = 24;       // luminance difference, 0..255
  uint8_t changedPercent = 12;       // changed sample points, 1..100
  uint8_t confirmations = 2;
};

class MotionDetector {
 public:
  explicit MotionDetector(MotionConfig config = MotionConfig()) : config_(config) {}
  void reset() { ready_ = false; streak_ = 0; score_ = 0; }
  uint8_t score() const { return score_; }
  bool update(const uint8_t *rgb, size_t len, size_t width, size_t height, uint32_t now) {
    if (!config_.enabled || !rgb || width < 32 || height < 24 ||
        height > SIZE_MAX / width / 2 || len < width * height * 2) return false;
    if (ready_ && width == width_ && height == height_ &&
        uint32_t(now - lastSample_) < config_.sampleIntervalMs) return false;
    if (width != width_ || height != height_) reset();
    width_ = width; height_ = height; lastSample_ = now;
    uint8_t samples[32 * 24];
    int sum = 0, previousSum = 0;
    for (size_t y = 0; y < 24; ++y) {
      for (size_t x = 0; x < 32; ++x) {
        size_t offset = (((2 * y + 1) * height / 48) * width +
                         (2 * x + 1) * width / 64) * 2;
        uint16_t pixel = (uint16_t(rgb[offset]) << 8) | rgb[offset + 1];
        int r = ((pixel >> 11) & 31) * 255 / 31;
        int g = ((pixel >> 5) & 63) * 255 / 63;
        int b = (pixel & 31) * 255 / 31;
        size_t i = y * 32 + x;
        samples[i] = (77 * r + 150 * g + 29 * b) >> 8;
        sum += samples[i]; previousSum += previous_[i];
      }
    }
    // Compensate uniform exposure changes to reduce light-switch false alarms.
    int shift = (sum - previousSum) / (32 * 24);
    unsigned changed = 0;
    for (size_t i = 0; i < 32 * 24; ++i) {
      int delta = int(samples[i]) - previous_[i] - shift;
      if (delta < 0) delta = -delta;
      if (delta >= config_.pixelThreshold) ++changed;
      previous_[i] = samples[i];
    }
    if (!ready_) { ready_ = true; return false; }
    score_ = changed * 100 / (32 * 24);
    if (score_ < config_.changedPercent) { streak_ = 0; return false; }
    if (streak_ < 255) ++streak_;
    if (streak_ < config_.confirmations ||
        (triggered_ && uint32_t(now - lastEvent_) < config_.cooldownMs)) return false;
    streak_ = 0; triggered_ = true; lastEvent_ = now;
    return true;
  }
 private:
  MotionConfig config_;
  uint8_t previous_[32 * 24] = {};
  size_t width_ = 0, height_ = 0;
  uint32_t lastSample_ = 0, lastEvent_ = 0;
  uint8_t streak_ = 0, score_ = 0;
  bool ready_ = false, triggered_ = false;
};
