#pragma once

#include <algorithm>
#include <cstddef>

namespace pbr {

/** Pure rule for how much of the chats list the sidebar draws (no RmlUi): a window that grows near the bottom. */
inline constexpr size_t kSessionListStep = 20;
/** Distance from the bottom of the sidebar list, in dp, that counts as "near the bottom". */
inline constexpr float kSessionListNearBottomPx = 64.f;

inline bool SessionListNearBottom(const float scroll_top, const float scroll_height, const float client_height) {
  const float max_top = std::max(0.f, scroll_height - client_height);
  return max_top > 0.5f && (max_top - scroll_top) <= kSessionListNearBottomPx;
}

/** The first `kSessionListStep` rows, then `kSessionListStep` more each time the user reaches the bottom. */
class SessionListWindow {
public:
  /** Back to the first window (a different list, or a fresh profile). */
  void Reset() { shown_ = kSessionListStep; }

  /** How many of `total` rows are drawn. */
  size_t Visible(const size_t total) const { return std::min(shown_, total); }

  /** Rows remain beyond the window (the "Loading…" line). */
  bool More(const size_t total) const { return shown_ < total; }

  /** The window reaches row `index` (the open chat must be drawn wherever it sorts). True when it grew. */
  bool Include(const size_t index) {
    const size_t need = ((index / kSessionListStep) + 1) * kSessionListStep;
    if (need <= shown_) {
      return false;
    }
    shown_ = need;
    return true;
  }

  /** Grows the window when the user is near the bottom and rows remain. True when it grew. */
  bool ExtendIfNearBottom(const size_t total, const bool near_bottom) {
    if (!near_bottom || !More(total)) {
      return false;
    }
    shown_ += kSessionListStep;
    return true;
  }

private:
  size_t shown_ = kSessionListStep;
};

} // namespace pbr
