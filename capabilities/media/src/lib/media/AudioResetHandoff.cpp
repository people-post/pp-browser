#include "lib/media/AudioResetHandoff.h"

namespace pbr {

void AudioResetHandoff::Run(const std::function<bool()>& safe_now,
                            const std::function<AudioResetResult(std::string*)>& rebuild) {
  {
    std::lock_guard lock(mu_);
    if (phase_ != Phase::Pending) {
      return; // the waiter gave up: never rebuild late
    }
    phase_ = Phase::Running;
  }
  std::string error;
  AudioResetResult result = AudioResetResult::Busy;
  if (!safe_now || !safe_now()) {
    error = "an audio stream is still open";
  } else {
    result = rebuild ? rebuild(&error) : AudioResetResult::Failed;
  }
  {
    std::lock_guard lock(mu_);
    phase_ = Phase::Done;
    result_ = result;
    error_ = error;
  }
  cv_.notify_all();
}

AudioResetResult AudioResetHandoff::Wait(const std::chrono::milliseconds budget, std::string* error) {
  std::unique_lock lock(mu_);
  if (!cv_.wait_for(lock, budget, [this] { return phase_ == Phase::Done; })) {
    if (phase_ == Phase::Pending) {
      phase_ = Phase::Abandoned;
      if (error) {
        *error = "the rebuild did not start in time";
      }
      return AudioResetResult::Timeout;
    }
    if (error) {
      *error = "the rebuild is still running";
    }
    return AudioResetResult::Stuck;
  }
  if (error) {
    *error = error_;
  }
  return result_;
}

AudioResetResult AudioResetHandoff::WaitUntilFinished(std::string* error) {
  std::unique_lock lock(mu_);
  cv_.wait(lock, [this] { return phase_ == Phase::Done || phase_ == Phase::Abandoned; });
  if (phase_ == Phase::Abandoned) {
    if (error) {
      *error = "the rebuild never started";
    }
    return AudioResetResult::Timeout;
  }
  if (error) {
    *error = error_;
  }
  return result_;
}

} // namespace pbr
