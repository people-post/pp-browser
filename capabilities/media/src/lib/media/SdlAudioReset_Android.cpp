#include "lib/media/SdlAudioReset.h"

#include "lib/media/AudioResetHandoff.h"
#include "lib/media/SdlAudioBootstrap.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <memory>
#include <utility>

namespace pbr {
namespace {

/**
 * A rebuild started on the main thread and never finished in time: SDL audio may be half torn down. No rebuild is
 * tried again in this process (it would queue behind the stuck one on the main thread).
 */
std::atomic<bool> g_rebuild_stuck{false};

struct Job {
  std::shared_ptr<AudioResetHandoff> handoff;
  std::function<bool()> safe_now;
};

/** SDL's main thread. */
void SDLCALL RunReset(void* userdata) {
  const std::unique_ptr<Job> job(static_cast<Job*>(userdata));
  job->handoff->Run(job->safe_now, [](std::string* error) {
    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
      return AudioResetResult::Ok; // never started (or already quit): the next open initialises it fresh
    }
    // AUDIO is initialised only by EnsureSdlAudioSubsystem (once, guarded by SDL_WasInit): one quit really quits.
    // SDL 3.4.16 on Android: SDL_QuitAudio closes the devices (none is open here), unregisters the Java audio device
    // callback (one JNI call) and unloads libaaudio — nothing known to block, but an OEM AAudio close can, so the
    // waiter never waits on it unbounded.
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    if (!EnsureSdlAudioSubsystem()) {
      *error = std::string("SDL_InitSubSystem(AUDIO) failed: ") + SDL_GetError();
      return AudioResetResult::Failed;
    }
    return AudioResetResult::Ok;
  });
}

} // namespace

bool SdlAudioResetSupported() {
  return !g_rebuild_stuck.load(std::memory_order_acquire);
}

AudioResetResult ResetSdlAudioOnMainThread(std::chrono::milliseconds budget, std::function<bool()> safe_now,
                                           std::string* error) {
  if (g_rebuild_stuck.load(std::memory_order_acquire)) {
    if (error) {
      *error = "an earlier rebuild never finished";
    }
    return AudioResetResult::Failed;
  }
  auto handoff = std::make_shared<AudioResetHandoff>();
  auto* job = new Job{handoff, std::move(safe_now)};
  // Not wait_complete: SDL's wait has no timeout, and the main thread may itself be waiting on this audio device
  // thread (a bounded acquire).
  if (!SDL_RunOnMainThread(RunReset, job, false)) {
    delete job;
    if (error) {
      *error = std::string("SDL_RunOnMainThread failed: ") + SDL_GetError();
    }
    return AudioResetResult::Failed;
  }
  const AudioResetResult result = handoff->Wait(budget, error);
  if (result != AudioResetResult::Stuck) {
    return result;
  }
  g_rebuild_stuck.store(true, std::memory_order_release);
  // Invariant: nothing touches SDL audio while a rebuild may still be running on the main thread. Every SDL audio
  // stream is opened and closed on this thread (the arbiter's audio device thread), so this thread stays here until
  // the rebuild returns. Its callers have stopped waiting (bounded); meanwhile the arbiter counts the audio devices
  // as not responding and refuses acquires instead of queueing them behind this.
  std::string late_error;
  const AudioResetResult late = handoff->WaitUntilFinished(&late_error);
  SDL_Log("SdlAudioReset: a rebuild past its budget finished late: %s %s", AudioResetResultName(late),
          late_error.c_str());
  return AudioResetResult::Stuck;
}

} // namespace pbr
