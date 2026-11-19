#include "lib/media/AudioSession.h"

#include <atomic>
#include <thread>

namespace pbr {
namespace {

std::atomic<int> g_tone_holds{0};

} // namespace

namespace AudioSession {

void BeginToneHold() {
  g_tone_holds.fetch_add(1, std::memory_order_acq_rel);
}

void EndToneHold() {
  g_tone_holds.fetch_sub(1, std::memory_order_acq_rel);
}

bool ToneHoldsSession() {
  return g_tone_holds.load(std::memory_order_acquire) > 0;
}

void WaitUntilToneHoldsReleased(const std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (ToneHoldsSession() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

} // namespace AudioSession

} // namespace pbr
