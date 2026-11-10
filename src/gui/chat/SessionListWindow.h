#pragma once

#include <algorithm>
#include <cstddef>

namespace pbr {

/** Pure rule for how much of the chats list the sidebar draws (no RmlUi): a window that grows near the bottom. */
inline constexpr size_t kSessionListStep = 20;
/** Distance from the bottom of the sidebar list, in dp, that counts as "near the bottom". */
inline constexpr float kSessionListNearBottomPx = 64.f;

/**
 * The drawn rows do not fill the list's viewport (a phone with few rows, a tall desktop window): nothing can
 * scroll, so no scroll event will ever ask for more. A list that is not laid out yet (client height 0) is left alone.
 */
inline bool SessionListNeedsFill(const float scroll_height, const float client_height) {
  return client_height > 0.5f && scroll_height <= client_height + 0.5f;
}

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

  /** Grows the window by one step when rows remain (a tap on the loading line, a viewport not yet filled). */
  bool Extend(const size_t total) {
    if (!More(total)) {
      return false;
    }
    shown_ += kSessionListStep;
    return true;
  }

  /** Grows the window when the user is near the bottom and rows remain. True when it grew. */
  bool ExtendIfNearBottom(const size_t total, const bool near_bottom) { return near_bottom && Extend(total); }

private:
  size_t shown_ = kSessionListStep;
};

} // namespace pbr
