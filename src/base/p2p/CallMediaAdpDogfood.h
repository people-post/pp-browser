#pragma once

namespace pbr {

/**
 * TEMP dogfood gate for legacy 1:1 Opus-over-ADP side-path (A008/A011).
 * Disabled: product call-media uses Amp CallMediaLegCoordinator when MeshHost Amp is up ([A020]).
 * Delete this header with CallMediaAdpPath once TCP call-media is fully retired (D9 step 7).
 */
inline constexpr bool kCallMediaAdpOpusDogfood = false;

} // namespace pbr
