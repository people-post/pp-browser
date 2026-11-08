#pragma once

#include "common/media/CallMediaHealth.h"

#include <cstdint>
#include <string>

namespace pbr {

/**
 * The words for "we send but nothing comes back yet" (#170). It reads as a hand-off in progress ("Connecting
 * voice…"), not as a symptom ("You can't hear them"), and only after kRetryAfterMs of it does it say the retry out
 * loud. The decision lives here, not in CallController: fed each poll, it times how long the one-way audio has run.
 */
class CallMediaHintClock {
public:
  /** One-way audio for longer than this reads "not through yet, retrying". */
  static constexpr int64_t kRetryAfterMs = 8'000;

  /** Notes this poll: whether the call is sending-only now. A new call or a recovered line restarts the clock. */
  void Update(const std::string& call_id, bool sending_only, int64_t now_ms);
  /** The locale key for the sending-only line of a voice or video call. */
  const char* SendingOnlyKey(bool video) const;
  /** `CallAudioAsymmetryHintKey` with the sending-only case replaced by SendingOnlyKey. */
  const char* KeyFor(CallAudioAsymmetry asymmetry, bool video) const;

private:
  std::string call_id_;
  int64_t since_ms_ = 0;
  int64_t now_ms_ = 0;
};

} // namespace pbr
