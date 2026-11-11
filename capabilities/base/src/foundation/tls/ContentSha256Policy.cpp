#include "foundation/tls/ContentSha256Policy.h"

#include <atomic>

namespace pbr {

namespace {
std::atomic<bool> g_send_content_sha256{false};
}

void SetSendContentSha256(const bool enabled) {
  g_send_content_sha256.store(enabled, std::memory_order_relaxed);
}

bool SendContentSha256() {
  return g_send_content_sha256.load(std::memory_order_relaxed);
}

} // namespace pbr
