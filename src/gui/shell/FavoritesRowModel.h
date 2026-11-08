#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace pbr {

/**
 * Home's 关注联系人 row (Kenneth 2026-11-05, PLAN-ui-refresh-impl 原话 75-77; mockup 11-favorites-row option A without
 * the half-peek): exactly four people across the card, each a quarter of the row, the avatar sized from that quarter.
 * More than four scroll sideways freely (momentum, several people per swipe, no paging); once the scroll comes to rest
 * it eases to the nearest person, so four whole avatars show and none is cut at either edge. The decisions, without any
 * DOM.
 */
struct FavoritesRowModel {
  static constexpr size_t kVisible = 4;
  /** The card's 16dp screen margins and 14dp padding on both sides (components.rcss .home-grid / .home-w). */
  static constexpr float kRowInsetDp = 2.f * 16.f + 2.f * 14.f;
  /** The avatar against its quarter of the row: about 64dp on a 393dp-wide phone. */
  static constexpr float kAvatarShareOfCell = 0.77f;
  static constexpr float kAvatarMinDp = 56.f;
  static constexpr float kAvatarMaxDp = 72.f;
  /** The scroll counts as at rest once it has not moved for this long (the end of a coast). */
  static constexpr double kRestMs = 90.0;

  /** The avatar's diameter for a window `window_width_dp` wide, in whole dp. */
  static int AvatarDp(const float window_width_dp) {
    const float cell = std::max(0.f, window_width_dp - kRowInsetDp) / static_cast<float>(kVisible);
    return static_cast<int>(std::lround(std::clamp(cell * kAvatarShareOfCell, kAvatarMinDp, kAvatarMaxDp)));
  }

  /**
   * Where a row at rest at `offset` (its scroll-left) eases to: the nearest person boundary within the scroll range.
   * `cell`: one person's width (a quarter of the row); `count`: how many people. Four or fewer do not scroll: 0.
   */
  static float SnapOffset(const float offset, const float cell, const size_t count) {
    if (count <= kVisible || cell <= 0.f) {
      return 0.f;
    }
    const float max_offset = static_cast<float>(count - kVisible) * cell;
    const float nearest = std::round(offset / cell) * cell;
    return std::clamp(nearest, 0.f, max_offset);
  }
};

} // namespace pbr
