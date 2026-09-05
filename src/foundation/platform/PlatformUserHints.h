#pragma once

#include <string_view>

namespace pbr {

/**
 * Catalog keys for OS-specific user tips. Never returns translated prose —
 * callers resolve with Tr(key, {{"product", kProductName}}).
 *
 * See docs/architecture/PLATFORM_CODE.md (PlatformUserHints convention).
 */
namespace PlatformUserHints {

/** P2P / LAN connect advice for the current OS (hints.network.*). */
std::string_view P2pNetworkHintKey();

/** Mic-privacy suffix when capture failed (hints.mic_blocked). */
std::string_view MicBlockedHintKey();

} // namespace PlatformUserHints

} // namespace pbr
