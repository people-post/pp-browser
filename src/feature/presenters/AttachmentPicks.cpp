#include "feature/presenters/AttachmentPicks.h"

namespace pbr {

uint64_t AttachmentPicks::Arm(const std::string& thread_id) {
  state_ = PickState{};
  state_.serial = ++next_;
  state_.thread_id = thread_id;
  return state_.serial;
}

void AttachmentPicks::Show(const uint64_t serial, const int64_t now_ms) {
  if (!IsCurrent(serial) || state_.showing) {
    return;
  }
  state_.showing = true;
  state_.shown_at_ms = now_ms;
}

bool AttachmentPicks::IsCurrent(const uint64_t serial) const {
  return serial != 0 && serial == state_.serial;
}

void AttachmentPicks::End(const uint64_t serial) {
  if (IsCurrent(serial)) {
    state_ = PickState{};
  }
}

void AttachmentPicks::EndCurrent() {
  state_ = PickState{};
}

std::optional<std::string> AttachmentPicks::TakeResult(const uint64_t serial, const std::string& open_thread,
                                                       const std::vector<std::string>& files,
                                                       const std::function<void(const std::string& path)>& release) {
  if (IsCurrent(serial) && !state_.thread_id.empty() && state_.thread_id == open_thread) {
    return state_.thread_id;
  }
  if (IsCurrent(serial)) {
    state_ = PickState{}; // its chat was left: the pick is over
  }
  for (const std::string& file : files) {
    if (!file.empty() && release) {
      release(file);
    }
  }
  return std::nullopt;
}

bool AttachmentPicks::LeftChat(const uint64_t serial, const std::string& open_thread) const {
  return IsCurrent(serial) && state_.thread_id != open_thread;
}

bool OfflineHold::Update(const bool offline, const int64_t now_ms) {
  if (!offline) {
    offline_since_ms_ = -1;
    return false;
  }
  if (offline_since_ms_ < 0) {
    offline_since_ms_ = now_ms;
  }
  return now_ms - offline_since_ms_ >= kOfflineStopMs;
}

bool AttachmentPicks::Accept(const uint64_t serial, const bool busy) {
  if (!busy) {
    return true;
  }
  End(serial);
  return false;
}

PickOverlay PickOverlayOf(const PickState& pick, const std::string& open_thread, const bool choice_shown,
                          const bool bubble_built, const int64_t now_ms) {
  if (pick.serial == 0 || !pick.showing || choice_shown || bubble_built || pick.thread_id != open_thread) {
    return PickOverlay::Hidden;
  }
  return now_ms - pick.shown_at_ms >= kPickSlowMs ? PickOverlay::TwoLines : PickOverlay::OneLine;
}

} // namespace pbr
