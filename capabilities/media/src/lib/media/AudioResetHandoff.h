#pragma once

#include "lib/media/MediaDeviceArbiter.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>

namespace pbr {

/**
 * An audio rebuild handed to another thread and waited for with a budget (Android: SDL's main thread runs
 * SDL_QuitSubSystem + SDL_InitSubSystem of AUDIO, SdlAudioReset_Android.cpp). Portable so its rules are tested
 * everywhere:
 *  - the waiter gives up on a rebuild that has not started by the budget, and it then never runs (Timeout);
 *  - a rebuild that started is never abandoned: past the budget the waiter learns it is Stuck, and must keep every
 *    other user of the audio stack away until WaitUntilFinished returns — the stack may be half torn down;
 *  - the other thread checks `safe_now` right before rebuilding: false (an audio stream is open) refuses it (Busy).
 * One rebuild per object.
 */
class AudioResetHandoff {
public:
  /** The other thread. */
  void Run(const std::function<bool()>& safe_now, const std::function<AudioResetResult(std::string*)>& rebuild);
  /** The waiter, at most `budget`: the rebuild's result, Timeout (not started; never will) or Stuck (still running). */
  AudioResetResult Wait(std::chrono::milliseconds budget, std::string* error);
  /** After Stuck: until the running rebuild returns (no budget: it cannot be interrupted). Its result. */
  AudioResetResult WaitUntilFinished(std::string* error);

private:
  enum class Phase { Pending, Running, Done, Abandoned };
  std::mutex mu_;
  std::condition_variable cv_;
  Phase phase_ = Phase::Pending;
  AudioResetResult result_ = AudioResetResult::Failed;
  std::string error_;
};

} // namespace pbr
