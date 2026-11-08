#include "feature/presenters/CallMediaHintClock.h"

namespace pbr {

void CallMediaHintClock::Update(const std::string& call_id, const bool sending_only, const int64_t now_ms) {
  now_ms_ = now_ms;
  if (call_id != call_id_) {
    call_id_ = call_id;
    since_ms_ = 0;
  }
  if (!sending_only) {
    since_ms_ = 0;
  } else if (since_ms_ == 0) {
    since_ms_ = now_ms;
  }
}

const char* CallMediaHintClock::SendingOnlyKey(const bool video) const {
  const bool retrying = since_ms_ > 0 && now_ms_ - since_ms_ > kRetryAfterMs;
  if (retrying) {
    return video ? "call.quality.hint.retrying_video" : "call.quality.hint.retrying_voice";
  }
  return video ? "call.quality.hint.connecting_video" : "call.quality.hint.connecting_voice";
}

const char* CallMediaHintClock::KeyFor(const CallAudioAsymmetry asymmetry, const bool video) const {
  if (asymmetry == CallAudioAsymmetry::SendingOnly) {
    return SendingOnlyKey(video);
  }
  return CallAudioAsymmetryHintKey(asymmetry);
}

} // namespace pbr
