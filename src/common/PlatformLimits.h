#pragma once

#include <cstddef>

namespace pbr {

inline constexpr size_t kMaxHttpClientBodyBytes = 8 * 1024 * 1024;
inline constexpr size_t kMaxLlmResponseBytes = 8 * 1024 * 1024;
inline constexpr size_t kMaxLlmRequestBytes = 2 * 1024 * 1024;
inline constexpr size_t kMaxProfileJsonFileBytes = 4 * 1024 * 1024;
/**
 * A content block on disk (CAS, a pending attachment): the largest attachment the large-transfer lane carries
 * (100 MB, `kLargeAttachmentMaxBytes` in lib/messaging/AttachmentLimits.h) plus its framing, with room.
 */
inline constexpr size_t kMaxCasBlockFileBytes = 128 * 1024 * 1024;
inline constexpr size_t kMaxMcpToolResultBytes = 1 * 1024 * 1024;
inline constexpr size_t kMaxStructuredParserOutputBytes = 512 * 1024;
inline constexpr size_t kMaxUserPayloadBytes = 64 * 1024;

} // namespace pbr
