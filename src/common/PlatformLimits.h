#pragma once

#include <cstddef>

namespace pbr {

inline constexpr size_t kMaxHttpClientBodyBytes = 8 * 1024 * 1024;
inline constexpr size_t kMaxLlmResponseBytes = 8 * 1024 * 1024;
inline constexpr size_t kMaxLlmRequestBytes = 2 * 1024 * 1024;
inline constexpr size_t kMaxProfileJsonFileBytes = 4 * 1024 * 1024;
inline constexpr size_t kMaxMcpToolResultBytes = 1 * 1024 * 1024;
inline constexpr size_t kMaxStructuredParserOutputBytes = 512 * 1024;
inline constexpr size_t kMaxUserPayloadBytes = 64 * 1024;

} // namespace pbr
