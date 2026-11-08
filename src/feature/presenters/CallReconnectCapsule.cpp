#include "feature/presenters/CallReconnectCapsule.h"

namespace pbr {

const CallReconnectView& CallReconnectCapsule::Step(const std::string& call_id, const bool reconnecting,
                                                    const int64_t now_ms) {
  if (call_id != call_id_) {
    *this = CallReconnectCapsule{};
    call_id_ = call_id;
  }
  using Kind = CallReconnectKind;
  if (call_id.empty()) {
    return view_;
  }
  if (reconnecting) {
    if (view_.kind != Kind::Reconnecting && view_.kind != Kind::Offline) {
      view_.kind = Kind::Reconnecting;
      since_ms_ = now_ms;
    }
    if (now_ms - since_ms_ >= kOfflineAfterMs) {
      view_.kind = Kind::Offline;
    }
    view_.fading = false;
  } else if (view_.kind == Kind::Reconnecting || view_.kind == Kind::Offline) {
    view_.kind = Kind::Restored;
    view_.fading = false;
    restored_ms_ = now_ms;
  } else if (view_.kind == Kind::Restored) {
    const int64_t shown_for = now_ms - restored_ms_;
    view_.fading = shown_for >= kRestoredShowMs;
    if (shown_for >= kRestoredShowMs + kFadeMs) {
      view_.kind = Kind::None;
      view_.fading = false;
    }
  }
  view_.timer_dim = view_.kind == Kind::Reconnecting || view_.kind == Kind::Offline;
  return view_;
}

const char* CallReconnectKey(const CallReconnectKind kind) {
  switch (kind) {
  case CallReconnectKind::Reconnecting:
    return "call.status.reconnecting";
  case CallReconnectKind::Restored:
    return "call.reconnect.restored";
  case CallReconnectKind::Offline:
    return "call.reconnect.offline";
  case CallReconnectKind::None:
    break;
  }
  return "";
}

} // namespace pbr
