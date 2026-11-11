#include "lib/media/SdlAudioReset.h"

#include "lib/media/SdlAudioBootstrap.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <memory>
#include <thread>
#include <utility>

namespace pbr {
namespace {

enum Phase : int { kPending, kRunning, kDone, kAbandoned };

struct ResetState {
  std::atomic<int> phase{kPending};
  std::function<bool()> safe_now;
  /** Written by the main thread before `phase` turns kDone. */
  AudioResetResult result = AudioResetResult::Failed;
  std::string error;
};

/** SDL's main thread. Owns a reference to the state (the waiter may have gone). */
void SDLCALL RunReset(void* userdata) {
  const std::unique_ptr<std::shared_ptr<ResetState>> holder(static_cast<std::shared_ptr<ResetState>*>(userdata));
  ResetState& state = **holder;
  int expected = kPending;
  if (!state.phase.compare_exchange_strong(expected, kRunning, std::memory_order_acq_rel)) {
    return; // the waiter gave up: never rebuild late
  }
  if (!state.safe_now || !state.safe_now()) {
    state.result = AudioResetResult::Busy;
    state.error = "an SDL audio stream is still open";
  } else if (!SDL_WasInit(SDL_INIT_AUDIO)) {
    // Never started (or already quit): the next open initialises it fresh.
    state.result = AudioResetResult::Ok;
  } else {
    // AUDIO is initialised only by EnsureSdlAudioSubsystem (once, guarded by SDL_WasInit): one quit really quits.
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    if (EnsureSdlAudioSubsystem()) {
      state.result = AudioResetResult::Ok;
    } else {
      state.result = AudioResetResult::Failed;
      state.error = std::string("SDL_InitSubSystem(AUDIO) failed: ") + SDL_GetError();
    }
  }
  state.phase.store(kDone, std::memory_order_release);
}

} // namespace

bool SdlAudioResetSupported() {
  return true;
}

AudioResetResult ResetSdlAudioOnMainThread(std::chrono::milliseconds budget, std::function<bool()> safe_now,
                                           std::string* error) {
  auto state = std::make_shared<ResetState>();
  state->safe_now = std::move(safe_now);
  auto* holder = new std::shared_ptr<ResetState>(state);
  // Not wait_complete: SDL's wait has no timeout, and the main thread may itself be waiting on the audio device
  // thread this runs on.
  if (!SDL_RunOnMainThread(RunReset, holder, false)) {
    delete holder;
    if (error) {
      *error = std::string("SDL_RunOnMainThread failed: ") + SDL_GetError();
    }
    return AudioResetResult::Failed;
  }
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (state->phase.load(std::memory_order_acquire) != kDone) {
    if (std::chrono::steady_clock::now() >= deadline) {
      int expected = kPending;
      if (state->phase.compare_exchange_strong(expected, kAbandoned, std::memory_order_acq_rel)) {
        if (error) {
          *error = "the main thread did not run it in time";
        }
        return AudioResetResult::Timeout;
      }
      // Already running on the main thread: it is quick, wait for it.
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (error) {
    *error = state->error;
  }
  return state->result;
}

} // namespace pbr
