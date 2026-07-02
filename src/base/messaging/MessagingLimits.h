#pragma once

#include <cstddef>

namespace pbr {

inline constexpr size_t kMaxComposeTextBytes = 64 * 1024;
inline constexpr size_t kMaxE2ePlaintextBytes = 128 * 1024;
inline constexpr size_t kMaxRelayEnvelopeBytes = 256 * 1024;
inline constexpr size_t kDefaultMessagesPageSize = 100;
inline constexpr size_t kMaxOpenThreadDbs = 16;

} // namespace pbr
