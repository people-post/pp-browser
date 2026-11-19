#pragma once

#include <chrono>
#include <optional>

namespace pbr {

/**
 * B230 safety net (a hypothesis, not confirmed on a device): something other than the call — on iOS, SDL closing a
 * ringtone stream, which sets its own category — may rewrite the audio session just after the call's voice-processing
 * unit started, leaving echo cancellation without the session it was built for. A category change that leaves the
 * session out of the call's configuration within kWindow of the unit's start asks for one reopen (the session configured
 * again, the unit rebuilt); at most once per call. A change that leaves the session as the call set it (our own
 * configuration, the speaker toggle) asks for nothing. Pure: the caller passes the clock and serialises the calls.
 */
class VoipCategoryGuard {
public:
  using Clock = std::chrono::steady_clock;
  static constexpr std::chrono::milliseconds kWindow{1000};

  /** The call's live session ended: the next call starts with its reopen unused. */
  void Reset() {
    started_.reset();
    used_ = false;
  }
  /** The voice-processing unit started at `at` (again after a reopen: the window moves, the used reopen stays used). */
  void OnVoiceProcessingStarted(const Clock::time_point at) { started_ = at; }
  /**
   * A category change seen at `at`; `ours`: the session is still in the call's category and mode. True: reopen now
   * (and never again in this call).
   */
  bool OnCategoryChange(const Clock::time_point at, const bool ours) {
    if (ours || used_ || !started_ || at < *started_ || at - *started_ > kWindow) {
      return false;
    }
    used_ = true;
    return true;
  }

private:
  std::optional<Clock::time_point> started_;
  bool used_ = false;
};

} // namespace pbr
