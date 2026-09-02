#include "base/media/CallRingtone.h"

#include "foundation/platform/IAssetLocator.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
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

CallRingtone::CallRingtone() = default;

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

void CallRingtone::RunLoop() {
  if (!SDL_WasInit(SDL_INIT_AUDIO)) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
      SDL_Log("CallRingtone: SDL_InitSubSystem(AUDIO) failed: %s", SDL_GetError());
      playing_ = false;
      return;
    }
  }
  SDL_AudioSpec want{};
  want.freq = wav_freq_;
  want.format = SDL_AUDIO_S16;
  want.channels = static_cast<Uint8>(wav_channels_);
  SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want, nullptr, nullptr);
  if (!stream) {
    SDL_Log("CallRingtone: playback open failed: %s", SDL_GetError());
    playing_ = false;
    return;
  }
  g_ringtone_playback_device_holders.fetch_add(1, std::memory_order_acq_rel);
  const SDL_AudioDeviceID device = SDL_GetAudioStreamDevice(stream);
  (void)SDL_ResumeAudioDevice(device);
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
  g_ringtone_playback_device_holders.fetch_sub(1, std::memory_order_acq_rel);
  playing_ = false;
}

} // namespace pbr
