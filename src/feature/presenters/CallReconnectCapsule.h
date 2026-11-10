#pragma once

#include <cstdint>
#include <string>

namespace pbr {

/**
 * The reconnect capsule of a running call (#171): the call page stays where it is (no way back to the connecting
 * page) and a capsule above the button bar says "Reconnecting…", then "Restored" for kRestoredShowMs, or, when the
 * line stays down for kOfflineAfterMs, "They may be offline". The call timer greys while it shows and keeps running.
 * Decided here (no GUI); CallMediaFrame steps it each poll and CallController words the view.
 */
enum class CallReconnectKind { None, Reconnecting, Restored, Offline };

struct CallReconnectView {
  CallReconnectKind kind = CallReconnectKind::None;
  /** The "Restored" capsule is on its way out. */
  bool fading = false;
  /** The timer under the name greys while the line is down. */
  bool timer_dim = false;

  bool operator==(const CallReconnectView&) const = default;
};

class CallReconnectCapsule {
public:
  static constexpr int64_t kOfflineAfterMs = 20'000;
  static constexpr int64_t kRestoredShowMs = 2'000;
  static constexpr int64_t kFadeMs = 300;

  /** `reconnecting`: the line is down now. An empty `call_id` (no call) resets. */
  const CallReconnectView& Step(const std::string& call_id, bool reconnecting, int64_t now_ms);
  const CallReconnectView& View() const { return view_; }

private:
  CallReconnectView view_;
  std::string call_id_;
  int64_t since_ms_ = 0;
  int64_t restored_ms_ = 0;
};

/** The capsule's words (empty for None). */
const char* CallReconnectKey(CallReconnectKind kind);

} // namespace pbr
