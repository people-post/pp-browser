#include "common/chat/RelayStreamKey.h"

#include <algorithm>
#include <atomic>
#include <chrono>

namespace pbr {

std::string BuildCanonicalRelayStreamKey(const std::string& contact_id_a, const std::string& contact_id_b,
                                         ThreadChannel channel, uint32_t session_epoch) {
  std::string lo = contact_id_a;
  std::string hi = contact_id_b;
  if (lo > hi) {
    std::swap(lo, hi);
  }
  return "v1:" + ThreadChannelToString(channel) + ":" + std::to_string(session_epoch) + ":" + lo + ":" + hi;
}

std::string BuildControlRelayStreamKey(const std::string& contact_id_a, const std::string& contact_id_b) {
  std::string lo = contact_id_a;
  std::string hi = contact_id_b;
  if (lo > hi) {
    std::swap(lo, hi);
  }
  return "v1:control:" + lo + ":" + hi;
}

std::string BuildGroupRelayStreamKey(const std::string& group_id, const uint32_t session_epoch,
                                     const std::string& member) {
  return "v1:group:" + group_id + ":" + std::to_string(session_epoch) + ":" + member;
}

std::string BuildDeviceRelayStreamKey(const std::string& contact_id_a, const std::string& contact_id_b,
                                      const ThreadChannel channel, const std::string& sender_device) {
  std::string lo = contact_id_a;
  std::string hi = contact_id_b;
  if (lo > hi) {
    std::swap(lo, hi);
  }
  return "v2:" + ThreadChannelToString(channel) + ":" + lo + ":" + hi + ":from:" + sender_device;
}

std::string BuildGroupDeviceRelayStreamKey(const std::string& group_id, const std::string& member,
                                           const std::string& sender_device) {
  return "v2:group:" + group_id + ":" + member + ":from:" + sender_device;
}

uint64_t NextRelayOrderKey() {
  static std::atomic<uint64_t> last{0};
  const uint64_t floor = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                   std::chrono::system_clock::now().time_since_epoch())
                                                   .count()) *
                         1000;
  uint64_t current = last.load(std::memory_order_relaxed);
  uint64_t next = 0;
  do {
    next = std::max(current + 1, floor);
  } while (!last.compare_exchange_weak(current, next, std::memory_order_relaxed));
  return next;
}

} // namespace pbr
