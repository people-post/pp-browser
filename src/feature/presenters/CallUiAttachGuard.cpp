#include "feature/presenters/CallUiAttachGuard.h"

#include "common/PbrCompat.h"

namespace pbr {

bool EngineHasOpenCall(const bool available, const std::string& active_call_id, const CallPhase phase) {
  if (!available || active_call_id.empty()) {
    return false;
  }
  switch (phase) {
    case CallPhase::OutboundCalling:
    case CallPhase::JoinedLocal:
    case CallPhase::MediaPending:
    case CallPhase::MediaConnecting:
    case CallPhase::InCall:
      return true;
    case CallPhase::Idle:
    case CallPhase::Ringing:
    case CallPhase::Accepting:
    case CallPhase::ConnectFailed:
      return false;
  }
  return false;
}

EngineCall EngineCallOf(const CallUiState& state) {
  EngineCall call;
  call.call_id = state.active_call_id;
  call.phase = state.phase;
  call.open = EngineHasOpenCall(state.available, state.active_call_id, state.phase);
  return call;
}

bool OngoingCallWanted(const bool presenter_has_call, const bool engine_open) {
  return presenter_has_call || engine_open;
}

void CallUiAttachGuard::ResetLost() {
  lost_call_id_.clear();
  lost_ticks_ = 0;
  lost_ms_ = 0;
  last_lost_ms_ = 0;
}

CallUiAttachGuard::Step CallUiAttachGuard::Feed(const Input& in) {
  Step step;
  // A call hung up here: until the engine lets go of it, its media is stopped again now and then.
  if (!hung_up_call_id_.empty()) {
    // Let go: the engine closed it or moved on — or the presenter no longer leaves it (the user joined that call
    // again, or the latch ran out): a call shown again is never stopped from here.
    if (!in.engine_open || in.engine_call_id != hung_up_call_id_ || in.leaving_call_id != hung_up_call_id_) {
      hung_up_call_id_.clear();
      stop_retries_ = 0;
    } else {
      if (stop_retries_ < kMaxStopRetries && in.steady_ms - last_stop_ms_ >= kStopRetryMs) {
        ++stop_retries_;
        last_stop_ms_ = in.steady_ms;
        step.stop_again = true;
        step.call_id = hung_up_call_id_;
      }
      return step; // never hung up twice
    }
  }
  if (in.engine_open && !in.leaving_call_id.empty() && in.engine_call_id == in.leaving_call_id) {
    // The user hung it up: the screen let go on purpose. Only its media is stopped again if the engine keeps it.
    hung_up_call_id_ = in.engine_call_id;
    last_stop_ms_ = in.steady_ms;
    stop_retries_ = 0;
    ResetLost();
    return step;
  }

  const bool lost = in.engine_open && in.ui_call_id != in.engine_call_id;
  if (!lost || in.engine_call_id != lost_call_id_) {
    ResetLost(); // found again, no open call, or another call: counted afresh
  }
  if (!lost) {
    return step;
  }
  if (lost_call_id_.empty()) {
    lost_call_id_ = in.engine_call_id;
    lost_ticks_ = 1;
  } else if (const int64_t gap = in.steady_ms - last_lost_ms_; gap > kMaxGapMs) {
    lost_ticks_ = 1; // paused: the gap is no time on screen, and the count starts again
    lost_ms_ = 0;
  } else {
    ++lost_ticks_;
    lost_ms_ += gap;
  }
  last_lost_ms_ = in.steady_ms;
  step.call_id = in.engine_call_id;
  if (reported_call_id_ != in.engine_call_id) {
    reported_call_id_ = in.engine_call_id;
    step.report_detached = true;
  }
  if (lost_ticks_ >= kHangUpTicks && lost_ms_ >= kHangUpMs) {
    step.hang_up = true;
    hung_up_call_id_ = in.engine_call_id;
    last_stop_ms_ = in.steady_ms;
    stop_retries_ = 0;
    ResetLost();
  }
  return step;
}

} // namespace pbr
