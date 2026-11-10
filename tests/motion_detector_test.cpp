#include "../firmware/esp32_cam_final/motion_detector.h"
#include <assert.h>
#include <stdio.h>
#include <vector>

static std::vector<uint8_t> frame(bool pattern, bool invert = false) {
  std::vector<uint8_t> bytes(32 * 24 * 2, 0);
  for (size_t i = 0; i < 32 * 24; ++i) {
    if (pattern && ((i % 2 == 0) != invert)) {
      bytes[i * 2] = 255; bytes[i * 2 + 1] = 255;
    }
  }
  return bytes;
}

int main() {
  auto black = frame(false), a = frame(true), b = frame(true, true);
  MotionDetector detector;
  auto feed = [&](const std::vector<uint8_t> &f, uint32_t now) {
    return detector.update(f.data(), f.size(), 32, 24, now);
  };
  assert(!feed(black, 0));                    // baseline
  assert(!feed(black, 200));                  // static scene
  assert(!feed(a, 400));                     // first changed sample
  assert(!feed(b, 450));                     // throttled; must not count
  assert(feed(b, 600));                      // two changed samples
  assert(!feed(a, 800));
  assert(!feed(b, 1000));                     // cooldown
  assert(feed(a, 15600));                    // cooldown expires
  detector.reset();
  assert(!feed(a, 15800));                    // reconfiguration baseline
  assert(!detector.update(nullptr, 0, 32, 24, 16000));
  assert(!detector.update(a.data(), 1, 32, 24, 16000));
  assert(!detector.update(a.data(), a.size(), SIZE_MAX, 24, 16000));

  MotionDetector light;
  std::vector<uint8_t> white(black.size(), 255);
  assert(!light.update(black.data(), black.size(), 32, 24, 0));
  assert(!light.update(white.data(), white.size(), 32, 24, 200));
  assert(light.score() == 0);                 // uniform exposure change

  MotionConfig disabled;
  disabled.enabled = false;
  MotionDetector off(disabled);
  assert(!off.update(a.data(), a.size(), 32, 24, 0));
  assert(!off.update(b.data(), b.size(), 32, 24, 200));

  MotionConfig wrapConfig;
  wrapConfig.confirmations = 1;
  wrapConfig.cooldownMs = 1000;
  MotionDetector wrap(wrapConfig);
  assert(!wrap.update(black.data(), black.size(), 32, 24, UINT32_MAX - 600));
  assert(wrap.update(a.data(), a.size(), 32, 24, UINT32_MAX - 400));
  assert(!wrap.update(b.data(), b.size(), 32, 24, 100));
  assert(wrap.update(a.data(), a.size(), 32, 24, 700));
  puts("motion detector: baseline/static/confirmation/throttle/cooldown/reset/invalid/light/disabled/rollover passed");
}
