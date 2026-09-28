#include "domain/media/CallRingtone.h"
#include "domain/media/CallAudioSession.h"
#include "domain/media/Ringback.h"

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

/** Ringback workers that activated the phone audio session and have not released / handed it over. */
std::atomic<int> g_ringback_session_holders{0};

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

CallRingtone::CallRingtone(Tone tone, MediaDeviceArbiter& devices) : devices_(devices), tone_(tone) {}

CallRingtone::~CallRingtone() {
  StopAndJoin();
}

bool CallRingtone::RingbackSessionHeld() {
  return g_ringback_session_holders.load(std::memory_order_acquire) > 0;
}

void CallRingtone::WaitUntilRingbackSessionReleased(const std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (RingbackSessionHeld() && std::chrono::steady_clock::now() < deadline) {
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
  // Mint a fresh stop flag + release flag + completion signal for this generation — never reused
  // across Start() calls — and hand the new worker the PREVIOUS generation's completion signal
  // (null on the first Start()) so it can wait for that worker to fully release the session
  // before touching it itself. The previous generation's own stop flag was already set by
  // RequestStop above and is never reset, so it cannot be revived by this Start().
  const std::shared_ptr<std::atomic<bool>> previous_done = current_done_;
  current_stop_ = std::make_shared<std::atomic<bool>>(false);
  current_release_ = std::make_shared<std::atomic<bool>>(true);
  current_done_ = std::make_shared<std::atomic<bool>>(false);
  playing_ = true;
  thread_ = std::thread([this, previous_done, stop = current_stop_, release = current_release_,
                         done = current_done_]() { RunLoop(previous_done, stop, release, done); });
}

void CallRingtone::RequestStop(const bool wait) {
  std::thread finishing;
  {
    std::lock_guard lock(mutex_);
    // Only the CURRENT generation needs signalling: every older one had its own flag set when it
    // was superseded (Start() always goes through here first).
    if (current_stop_) {
      current_stop_->store(true);
    }
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
  // Through current_release_ (the CURRENT generation's own flag) — never a member shared across
  // generations — so an older worker can never pick up a decision meant for this one.
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

void CallRingtone::RunLoop(std::shared_ptr<std::atomic<bool>> previous_done, std::shared_ptr<std::atomic<bool>> stop,
                           std::shared_ptr<std::atomic<bool>> release_on_stop,
                           std::shared_ptr<std::atomic<bool>> done) {
  // Cross-generation ordering rules for OutgoingRingback (a fast cancel-then-redial can launch
  // this worker before the PREVIOUS one has finished tearing down; the audio session must never
  // see two generations' activate / release calls interleave):
  //  1. Before activating, wait (bounded, 2000 ms) for `previous_done` — set by the prior worker
  //     as its very last step, after its own Deactivate() — so we never activate before an old
  //     worker's late release kills our session.
  //  2. `release_on_stop` belongs to this generation alone, so SetReleaseSessionOnStop() only
  //     ever reaches the generation it was called for.
  //  3. `done` is set as the last step on every exit path (early failures included), and only
  //     after a bounded wait for `previous_done` — so `done` transitively means "every earlier
  //     generation has finished too".
  //  4. `stop` belongs to this generation alone (never reset): a later Start() cannot un-stop us.
  //     Stopped while the speaker was still opening, we skip the activate and the refill loop.
  //  5. `playing_` is shared and owned by the UI side (Start sets it, RequestStop clears it). A
  //     worker only clears it on an early failure while it is still the current generation —
  //     under mutex_ — so an old worker exiting never reports false for a newer, playing one.
  const auto clear_playing_if_current = [this, &stop]() {
    std::lock_guard lock(mutex_);
    if (!stop->load()) {
      playing_ = false;
    }
  };
  const auto wait_for_previous = [&previous_done, &stop](const bool honor_stop) {
    if (!previous_done) {
      return;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
    while (!previous_done->load(std::memory_order_acquire) && !(honor_stop && stop->load()) &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  };
  const auto signal_done = [this, &done, &wait_for_previous]() {
    if (tone_ == Tone::OutgoingRingback) {
      wait_for_previous(/*honor_stop=*/false);
    }
    if (done) {
      done->store(true, std::memory_order_release);
    }
  };

  AudioLeaseRequest request;
  request.kind = MediaDeviceKind::Speaker;
  request.holder = tone_ == Tone::OutgoingRingback ? "ringback" : "ringtone";
  request.format = AudioDeviceFormat{wav_freq_, wav_channels_};
  request.still_wanted = [stop]() { return !stop->load(); };
  // The speaker opens before (ringback) or without (ringtone) ActivateForVoipCall: keep a previous
  // call's delayed Deactivate retry from stopping this tone's I/O.
  CallAudioSession::CancelPendingDeactivate();
  auto lease = devices_.AcquireAudio(request);
  if (!lease || !(*lease)->HasDevice()) {
    SDL_Log("CallRingtone: no speaker: %s", lease ? (*lease)->OpenError().c_str() : lease.error().message.c_str());
    clear_playing_if_current();
    signal_done();
    return;
  }
  AudioDeviceLease& speaker = **lease;
  // Only this worker activates / releases the session for OutgoingRingback (never the UI thread),
  // so a fast cancel can never activate after the controller already decided to release it. Skip
  // the activate if a stop landed while the speaker was still opening — nobody would release it.
  bool activated = false;
  if (tone_ == Tone::OutgoingRingback) {
    if (previous_done) {
      wait_for_previous(/*honor_stop=*/true);
      if (!previous_done->load(std::memory_order_acquire) && !stop->load()) {
        SDL_Log("CallRingtone: previous ringback generation did not signal done within 2000ms — activating anyway");
      }
    }
    if (!stop->load()) {
      // After the open: SDL rewrites the AVAudioSession when it opens a device (routes phones
      // to the earpiece).
      g_ringback_session_holders.fetch_add(1, std::memory_order_acq_rel);
      CallAudioSession::ActivateForVoipCall();
      activated = true;
    }
  }
  SDL_Log("CallRingtone: playing loop freq=%d ch=%d bytes=%zu", wav_freq_, wav_channels_, wav_pcm_.size());

  const int low_water = wav_freq_ * static_cast<int>(sizeof(int16_t)) / 2;
  while (!stop->load()) {
    if (speaker.Queued() < low_water) {
      (void)speaker.Write(wav_pcm_.data(), static_cast<int>(wav_pcm_.size()));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
  }

  speaker.Clear();
  // Releasing the lease queues the close on the arbiter's device thread — never blocks here.
  lease->reset();
  if (activated) {
    if (release_on_stop && release_on_stop->load(std::memory_order_relaxed)) {
      CallAudioSession::Deactivate();
    }
    // Released, or handed to call media: either way a call may activate its own session now.
    g_ringback_session_holders.fetch_sub(1, std::memory_order_acq_rel);
  }
  // No playing_ write here: the loop only exits once `stop` is set, and RequestStop already
  // cleared playing_ at that moment (rule 5).
  signal_done();
}

} // namespace pbr
