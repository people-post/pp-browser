#include "foundation/platform/ui/DropCollector.h"

#include <algorithm>
#include <utility>

namespace pbr {

DropUiPoint DropPointToUi(const DropPoint point, const float pixel_density) {
  return DropUiPoint{static_cast<int>(point.x * pixel_density), static_cast<int>(point.y * pixel_density)};
}

void DropCollector::Reset() {
  position_.reset();
  hint_visible_ = false;
  files_.clear();
}

void DropCollector::OnBegin() {
  Reset();
}

void DropCollector::OnPosition(const float x, const float y, const int64_t now_ms) {
  if (!files_.empty()) {
    // Files of a drag that never completed are still held: this is another drag.
    Reset();
  }
  position_ = DropPoint{x, y};
  position_ms_ = now_ms;
  hint_visible_ = true;
}

void DropCollector::OnFile(std::string path) {
  files_.push_back(std::move(path));
}

DropCollector::Completed DropCollector::OnComplete() {
  Completed done;
  done.left = files_.empty();
  done.paths = std::move(files_);
  done.position = position_;
  Reset();
  return done;
}

bool DropCollector::OnFocusLost() {
  const bool was_visible = hint_visible_;
  Reset();
  return was_visible;
}

bool DropCollector::Tick(const int64_t now_ms) {
  if (hint_visible_ && now_ms - position_ms_ > kHintMs) {
    hint_visible_ = false;
    return true;
  }
  return false;
}

std::optional<int64_t> DropCollector::MsUntilHintHide(const int64_t now_ms) const {
  if (!hint_visible_) {
    return std::nullopt;
  }
  return std::max<int64_t>(0, position_ms_ + kHintMs + 1 - now_ms);
}

} // namespace pbr
