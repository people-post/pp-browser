#pragma once

#include "feature/presenters/TwoFingerGesture.h"

#include <cstdint>
#include <vector>

namespace pbr {

/** Where a finger event goes: kept by the crop page (the UI library never sees it) or passed on to the page's UI. */
enum class CropFingerRoute { Pass, Own };

/**
 * Which finger events the avatar crop page takes before the UI library sees them (PLAN-avatar-edit item 9; the UI
 * library turns every finger into a mouse press, so a second finger would click or scroll the page). No GUI or SDL
 * type: the caller says whether a Down landed inside the picture's area.
 * 1. Nothing tracked: a Down inside the picture area is owned, and so are that finger's later Move / Up; a Down
 *    outside (title bar, the buttons) passes and the buttons work.
 * 2. While any finger is tracked, every new Down is owned wherever it lands (a finger brushing the bottom bar during a
 *    drag must not press a button). The gesture uses the first two; the rest are still owned so they never reach the
 *    UI library half-way.
 * 3. Cancel (any id): every tracked finger is forgotten (the Cancel itself is owned when its finger was). Focus lost,
 *    the app going to the background and the page closing call Clear().
 * 4. Paused (a PIN pad, a confirm box or a menu is over the page): everything passes; pausing forgets the tracked
 *    fingers.
 */
class CropFingerOwnership {
public:
  CropFingerRoute OnFinger(FingerEventKind kind, int64_t finger_id, bool inside_picture_area);
  void Clear();
  void SetPaused(bool paused);
  bool paused() const { return paused_; }
  bool Tracking() const { return !owned_.empty(); }

private:
  bool Owns(int64_t finger_id) const;

  bool paused_ = false;
  std::vector<int64_t> owned_;
};

} // namespace pbr
