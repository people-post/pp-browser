#include "domain/media/CallRingtone.h"
#include "domain/media/CallAudioSession.h"
#include "domain/media/Ringback.h"
#include "domain/media/SdlAudioBootstrap.h"

#include "foundation/platform/IAssetLocator.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <utility>

namespace pbr {
namespace {

std::atomic<int> g_ringtone_playback_device_holders{0};

bool LoadRingWav(std::vector<unsigned char>& pcm, int& freq, int& channels) {
  const std::string path = IAssetLocator::Instance().Resolve("sounds/call_ring.wav");
  if (path.empty()) {
    SDL_Log("CallRingtone: empty path for sounds/call_ring.wav");
    return false;
  }
  SDL_AudioSpec spec{};
  Uint8* buf = nullptr;
  Uint32 len = 0;
  if (!SDL_LoadWAV(path.c_str(), &spec, &buf, &len) || !buf || len == 0) {
    SDL_Log("CallRingtone: SDL_LoadWAV failed path=%s err=%s", path.c_str(), SDL_GetError());
    if (buf) {
      SDL_free(buf);
    }
    return false;
  }
  freq = spec.freq > 0 ? spec.freq : 24000;
  channels = spec.channels > 0 ? static_cast<int>(spec.channels) : 1;

  if (spec.format == SDL_AUDIO_S16 && spec.channels == 1) {
    pcm.assign(buf, buf + len);
    SDL_free(buf);
    return true;
  }

  SDL_AudioSpec dst = spec;
  dst.format = SDL_AUDIO_S16;
  dst.channels = 1;
  Uint8* converted = nullptr;
  int converted_len = 0;
  const bool ok =
      SDL_ConvertAudioSamples(&spec, buf, static_cast<int>(len), &dst, &converted, &converted_len);
  SDL_free(buf);
  if (!ok || !converted || converted_len <= 0) {
    SDL_Log("CallRingtone: convert failed err=%s", SDL_GetError());
    if (converted) {
      SDL_free(converted);
    }
    return false;
  }
  pcm.assign(converted, converted + converted_len);
  SDL_free(converted);
  freq = dst.freq > 0 ? dst.freq : freq;
  channels = 1;
  return true;
}

} // namespace

CallRingtone::CallRingtone(Tone tone) : tone_(tone) {}

CallRingtone::~CallRingtone() {
  StopAndJoin();
}

bool CallRingtone::PlaybackDeviceHeld() {
  return g_ringtone_playback_device_holders.load(std::memory_order_acquire) > 0;
}

void CallRingtone::WaitUntilPlaybackDeviceReleased(const int timeout_ms) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(0, timeout_ms));
  while (PlaybackDeviceHeld() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

void CallRingtone::Start() {
  {
    std::lock_guard lock(mutex_);
    if (playing_.load()) {
      return;
    }
  }
  // A prior worker may still be tearing down audio. Never join it on the UI/SDL
  // thread — device close can wait on that thread (Samsung Accept hang).
  RequestStop(/*wait=*/false);

  std::lock_guard lock(mutex_);
  if (playing_.load()) {
    return;
  }
  if (wav_pcm_.empty()) {
    if (tone_ == Tone::OutgoingRingback) {
      const std::vector<int16_t> pcm = MakeRingbackCycle(wav_freq_);
      const auto* bytes = reinterpret_cast<const unsigned char*>(pcm.data());
      wav_pcm_.assign(bytes, bytes + pcm.size() * sizeof(int16_t));
    } else if (!LoadRingWav(wav_pcm_, wav_freq_, wav_channels_)) {
      return;
    }
  }
  // Mint a fresh release flag + completion signal for this generation — never reused
  // across Start() calls — and hand the new worker the PREVIOUS generation's completion
  // signal (null on the first Start()) so it can wait for that worker to fully release
  // the session before touching it itself. See RunLoop / SetReleaseSessionOnStop docs.
  const std::shared_ptr<std::atomic<bool>> previous_done = current_done_;
  current_release_ = std::make_shared<std::atomic<bool>>(true);
  current_done_ = std::make_shared<std::atomic<bool>>(false);
  stop_ = false;
  playing_ = true;
  thread_ = std::thread(
      [this, previous_done, release = current_release_, done = current_done_]() {
        RunLoop(previous_done, release, done);
      });
}

void CallRingtone::RequestStop(const bool wait) {
  stop_ = true;
  std::thread finishing;
  {
    std::lock_guard lock(mutex_);
    playing_ = false;
    if (thread_.joinable()) {
      finishing = std::move(thread_);
    }
  }

  if (wait) {
    if (finishing.joinable()) {
      finishing.join();
    }
    std::thread joiner;
    {
      std::lock_guard lock(mutex_);
      if (joiner_.joinable()) {
        joiner = std::move(joiner_);
      }
    }
    if (joiner.joinable()) {
      joiner.join();
    }
    return;
  }

  if (!finishing.joinable()) {
    return;
  }
  // Chain onto joiner_ so StopAndJoin / destructor can still wait — never bare .detach().
  std::thread previous;
  {
    std::lock_guard lock(mutex_);
    if (joiner_.joinable()) {
      previous = std::move(joiner_);
    }
    joiner_ = std::thread([previous = std::move(previous), finishing = std::move(finishing)]() mutable {
      if (previous.joinable()) {
        previous.join();
      }
      if (finishing.joinable()) {
        finishing.join();
      }
    });
  }
}

void CallRingtone::SetReleaseSessionOnStop(const bool release) {
  // Writes through current_release_ (the CURRENT generation's own flag) — never a
  // member shared across generations — so a still-tearing-down older worker can never
  // pick up a decision meant for the generation this call is actually about.
  std::shared_ptr<std::atomic<bool>> flag;
  {
    std::lock_guard lock(mutex_);
    flag = current_release_;
  }
  if (flag) {
    flag->store(release, std::memory_order_relaxed);
  }
}

void CallRingtone::Stop() {
  RequestStop(/*wait=*/false);
}

void CallRingtone::StopAndJoin() {
  RequestStop(/*wait=*/true);
}

bool CallRingtone::StopAndJoin(std::chrono::milliseconds budget) {
  RequestStop(/*wait=*/false);

  std::thread to_join;
  {
    std::lock_guard lock(mutex_);
    if (joiner_.joinable()) {
      to_join = std::move(joiner_);
    } else if (thread_.joinable()) {
      // RequestStop with empty joiner left playback on thread_ (no prior async Stop).
      to_join = std::move(thread_);
    }
  }
  if (!to_join.joinable()) {
    return true;
  }

  auto finished = std::make_shared<std::atomic<bool>>(false);
  std::thread waiter([finishing = std::move(to_join), finished]() mutable {
    if (finishing.joinable()) {
      finishing.join();
    }
    finished->store(true, std::memory_order_release);
  });

  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (!finished->load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  if (finished->load(std::memory_order_acquire)) {
    if (waiter.joinable()) {
      waiter.join();
    }
    return true;
  }

  SDL_Log("CallRingtone::StopAndJoin: still live after %lldms — detaching (process exit must follow)",
          static_cast<long long>(budget.count()));
  waiter.detach();
  return false;
}

void CallRingtone::RunLoop(std::shared_ptr<std::atomic<bool>> previous_done,
                           std::shared_ptr<std::atomic<bool>> release_on_stop,
                           std::shared_ptr<std::atomic<bool>> done) {
  // Cross-generation ordering rules for OutgoingRingback (a fast cancel-then-redial can
  // launch this worker before the PREVIOUS one has finished tearing down; the audio
  // session must never see two generations' activate/release calls interleave):
  //  1. Before activating, wait (bounded, 2000ms) for `previous_done` — set by the prior
  //     worker as its very last step, after its own Deactivate()/holder decrement — so
  //     we can never activate before an old worker's late release kills our session.
  //  2. `release_on_stop` belongs to this generation alone (minted fresh in Start()),
  //     never shared/reused, so SetReleaseSessionOnStop() can only ever reach the
  //     generation it was called for.
  //  3. `done` is set as literally the last step on every exit path (including early
  //     failures), so whichever generation starts next — if any — has something to wait
  //     on and is never blocked by one that ran but never played.
  const auto signal_done = [&done]() {
    if (done) {
      done->store(true, std::memory_order_release);
    }
  };

  if (!EnsureSdlAudioSubsystem()) {
    SDL_Log("CallRingtone: SDL_InitSubSystem(AUDIO) failed: %s", SDL_GetError());
    playing_ = false;
    signal_done();
    return;
  }
  SDL_AudioSpec want{};
  want.freq = wav_freq_;
  want.format = SDL_AUDIO_S16;
  want.channels = static_cast<Uint8>(wav_channels_);
  SDL_Log("CallRingtone: opening playback driver=%s", SDL_GetCurrentAudioDriver());
  SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want, nullptr, nullptr);
  if (!stream) {
    SDL_Log("CallRingtone: playback open failed: %s", SDL_GetError());
    playing_ = false;
    signal_done();
    return;
  }
  g_ringtone_playback_device_holders.fetch_add(1, std::memory_order_acq_rel);
  const SDL_AudioDeviceID device = SDL_GetAudioStreamDevice(stream);
  (void)SDL_ResumeAudioDevice(device);
  // Only this worker activates/releases the session for OutgoingRingback (never the UI
  // thread) so a fast cancel can never activate after the controller already decided to
  // release it. Skip the activate entirely if a stop already landed while we were still
  // opening the device — otherwise we'd activate a session nobody will release.
  bool activated = false;
  if (tone_ == Tone::OutgoingRingback) {
    if (previous_done) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
      while (!previous_done->load(std::memory_order_acquire) &&
             std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      if (!previous_done->load(std::memory_order_acquire)) {
        SDL_Log("CallRingtone: previous ringback generation did not signal done within "
                "2000ms — activating anyway");
      }
    }
    if (!stop_.load()) {
      // SDL rewrites the AVAudioSession when it opens a device, so activate the VoIP
      // session after opening the stream (routes phones to the earpiece).
      CallAudioSession::ActivateForVoipCall();
      activated = true;
    }
  }
  SDL_Log("CallRingtone: playing loop freq=%d ch=%d bytes=%zu", wav_freq_, wav_channels_,
          wav_pcm_.size());

  while (!stop_.load()) {
    const int queued = SDL_GetAudioStreamQueued(stream);
    if (queued < want.freq * static_cast<int>(sizeof(int16_t)) / 2) {
      (void)SDL_PutAudioStreamData(stream, wav_pcm_.data(), static_cast<int>(wav_pcm_.size()));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
  }

  SDL_ClearAudioStream(stream);
  // Destroying a stream from SDL_OpenAudioDeviceStream also closes the device — do not
  // SDL_CloseAudioDevice(device) afterward (double-close can hang quit on Android).
  SDL_DestroyAudioStream(stream);
  if (activated && release_on_stop && release_on_stop->load(std::memory_order_relaxed)) {
    // Release before dropping our holder count so CallMediaEngine::OpenAudioDevices —
    // which waits for holders == 0 before ActivateForVoipCall()'ing its own session —
    // never observes the device free while our session is still active.
    CallAudioSession::Deactivate();
  }
  g_ringtone_playback_device_holders.fetch_sub(1, std::memory_order_acq_rel);
  playing_ = false;
  signal_done();
}

} // namespace pbr
