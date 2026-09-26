#include "domain/media/CallRingtone.h"

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

CallRingtone::CallRingtone(MediaDeviceArbiter& devices) : devices_(devices) {}

CallRingtone::~CallRingtone() {
  StopAndJoin();
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
    if (!LoadRingWav(wav_pcm_, wav_freq_, wav_channels_)) {
      return;
    }
  }
  stop_ = false;
  playing_ = true;
  thread_ = std::thread([this]() { RunLoop(); });
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

void CallRingtone::RunLoop() {
  AudioLeaseRequest request;
  request.kind = MediaDeviceKind::Speaker;
  request.holder = "ringtone";
  request.format = AudioDeviceFormat{wav_freq_, wav_channels_};
  request.still_wanted = [this]() { return !stop_.load(); };
  auto lease = devices_.AcquireAudio(request);
  if (!lease || !(*lease)->HasDevice()) {
    SDL_Log("CallRingtone: no speaker: %s", lease ? (*lease)->OpenError().c_str() : lease.error().message.c_str());
    playing_ = false;
    return;
  }
  AudioDeviceLease& speaker = **lease;
  SDL_Log("CallRingtone: playing loop freq=%d ch=%d bytes=%zu", wav_freq_, wav_channels_, wav_pcm_.size());

  const int low_water = wav_freq_ * static_cast<int>(sizeof(int16_t)) / 2;
  while (!stop_.load()) {
    if (speaker.Queued() < low_water) {
      (void)speaker.Write(wav_pcm_.data(), static_cast<int>(wav_pcm_.size()));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
  }

  speaker.Clear();
  // Releasing the lease queues the close on the arbiter's device thread — never blocks here.
  lease->reset();
  playing_ = false;
}

} // namespace pbr
