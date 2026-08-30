#pragma once

namespace pbr {

/**
 * TEMP dogfood gate for 1:1 Opus-over-ADP (A008/A011).
 * Flip to enable LAN testing without config/settings. When Opus-over-ADP is proven,
 * delete this header and the `kCallMediaAdpOpusDogfood` checks — path becomes default-on.
 */
inline constexpr bool kCallMediaAdpOpusDogfood = true;

} // namespace pbr
