#include "feature/presenters/CropFingerOwnership.h"

#include <algorithm>

namespace pbr {

bool CropFingerOwnership::Owns(const int64_t finger_id) const {
  return std::find(owned_.begin(), owned_.end(), finger_id) != owned_.end();
}

void CropFingerOwnership::Clear() {
  owned_.clear();
}

void CropFingerOwnership::SetPaused(const bool paused) {
  paused_ = paused;
  if (paused) {
    Clear();
  }
}

CropFingerRoute CropFingerOwnership::OnFinger(const FingerEventKind kind, const int64_t finger_id,
                                              const bool inside_picture_area) {
  if (paused_) {
    return CropFingerRoute::Pass;
  }
  switch (kind) {
    case FingerEventKind::Down:
      if (Owns(finger_id)) {
        return CropFingerRoute::Own;
      }
      if (!owned_.empty() || inside_picture_area) {
        owned_.push_back(finger_id);
        return CropFingerRoute::Own;
      }
      return CropFingerRoute::Pass;
    case FingerEventKind::Move:
      return Owns(finger_id) ? CropFingerRoute::Own : CropFingerRoute::Pass;
    case FingerEventKind::Up:
      if (Owns(finger_id)) {
        owned_.erase(std::find(owned_.begin(), owned_.end(), finger_id));
        return CropFingerRoute::Own;
      }
      return CropFingerRoute::Pass;
    case FingerEventKind::Cancel: {
      const bool owned = Owns(finger_id);
      Clear();
      return owned ? CropFingerRoute::Own : CropFingerRoute::Pass;
    }
  }
  return CropFingerRoute::Pass;
}

} // namespace pbr
