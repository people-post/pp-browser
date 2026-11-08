#include "common/chat/RelayStreamKey.h"

#include "common/Serialize.hpp"
#include "crypto/SodiumUtil.h"

#include <sodium.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <sstream>

namespace pbr {

namespace {

const std::string kGroupStreamTag = "pp-browser:group-stream-v1";
constexpr size_t kGroupStreamHashBytes = 16; // BLAKE2b-128

} // namespace

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

std::string BuildDeviceRelayStreamKey(const std::string& contact_id_a, const std::string& contact_id_b,
                                      const ThreadChannel channel, const std::string& sender_device) {
  std::string lo = contact_id_a;
  std::string hi = contact_id_b;
  if (lo > hi) {
    std::swap(lo, hi);
  }
  return "v2:" + ThreadChannelToString(channel) + ":" + lo + ":" + hi + ":from:" + sender_device;
}

std::string BuildGroupRelayStreamKey(const std::string& group_id, const std::string& account_a,
                                     const std::string& account_b, const std::string& sender_device) {
  const std::string& lo = std::min(account_a, account_b);
  const std::string& hi = std::max(account_a, account_b);
  std::ostringstream oss;
  {
    ::pp::OutputArchive ar(oss);
    for (const std::string* field : {&kGroupStreamTag, &group_id, &lo, &hi}) {
      ::pp::WireLenUtf8 value{*field};
      ar & value;
    }
  }
  const std::string input = oss.str();
  std::array<unsigned char, kGroupStreamHashBytes> digest{};
  ::pp::EnsureSodiumInit();
  crypto_generichash(digest.data(), digest.size(), reinterpret_cast<const unsigned char*>(input.data()), input.size(),
                     nullptr, 0);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string hex;
  hex.reserve(digest.size() * 2);
  for (const unsigned char byte : digest) {
    hex.push_back(kHex[byte >> 4]);
    hex.push_back(kHex[byte & 0x0f]);
  }
  return "v1:g:" + hex + ":from:" + sender_device;
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
