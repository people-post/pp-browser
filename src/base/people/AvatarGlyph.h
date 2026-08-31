#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace pbr {

/** Number of `.avatar-tone-N` classes in theme sheets (0 .. kAvatarToneCount-1). */
inline constexpr int kAvatarToneCount = 8;

struct AvatarGlyph {
  /** Single grapheme/codepoint (UTF-8), or "?" when nothing usable. */
  std::string letter;
  /** Index into the fixed avatar tone palette. */
  int tone = 0;
};

/**
 * Letter avatar for empty photo slots.
 * - Letter from display_name (first non-space UTF-8 codepoint; ASCII lower→upper).
 * - If name empty: first alphanumeric from stable_id (after a `prefix:` if present).
 * - Tone hashed from stable_id only (falls back to name, then 0) so renames don't reshuffle color.
 */
AvatarGlyph MakeAvatarGlyph(std::string_view display_name, std::string_view stable_id);

/** Prefer account → relay → peer → contact local id for the color/letter seed. */
std::string AvatarStableId(std::string_view account_id, std::string_view relay_id, std::string_view peer_id,
                           std::string_view contact_id = {});

} // namespace pbr
