#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace pbr {

/**
 * In-call audio-fault toast policy (Kenneth, device test 2026-09-28: toasts fired on every dip).
 * A fault — can't hear the peer, or our mic isn't sending — must persist kAudioFaultToastMs before
 * it toasts, at most once per call; a new call restarts the clock. Network quality alone never
 * toasts (the signal bars show it). UI thread only.
 */
class CallAudioFaultToastGate {
public:
  static constexpr int64_t kAudioFaultToastMs = 10'000;

  /** Feed the current state each UI tick; true exactly when a toast should be shown now. */
  bool Update(const std::string& call_id, bool fault, int64_t now_ms) {
    if (call_id != fault_call_id_) {
      fault_call_id_ = call_id;
      fault_since_ms_.reset();
    }
    if (!fault) {
      fault_since_ms_.reset();
      return false;
    }
    if (!fault_since_ms_) {
      fault_since_ms_ = now_ms;
    }
    if (now_ms - *fault_since_ms_ < kAudioFaultToastMs || warned_call_id_ == call_id) {
      return false;
    }
    warned_call_id_ = call_id;
    return true;
  }

  /** Forget a pending fault (call chrome cleared). The once-per-call memory is kept. */
  void Reset() {
    fault_call_id_.clear();
    fault_since_ms_.reset();
  }

private:
  std::string fault_call_id_;
  std::optional<int64_t> fault_since_ms_;
  std::string warned_call_id_;
};

} // namespace pbr
