#include "base/media/CallRingtone.h"

#include "base/platform/IAssetLocator.h"

#include <SDL3/SDL.h>

#include <chrono>

namespace pbr {
namespace {

bool LoadRingWav(std::vector<unsigned char>& pcm, int& freq, int& channels) {
  const std::string path = IAssetLocator::Instance().Resolve("sounds/call_ring.wav");
  if (path.empty()) {
    return false;
  }
  SDL_AudioSpec spec{};
  Uint8* buf = nullptr;
  Uint32 len = 0;
  if (!SDL_LoadWAV(path.c_str(), &spec, &buf, &len) || !buf || len == 0) {
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
  Stop(/*wait=*/true);
}

void CallRingtone::Start() {
  std::thread finishing;
  {
    std::lock_guard lock(mutex_);
    if (playing_.load()) {
      return;
    }
    // A prior worker may still be tearing down audio. Never join it on the UI/SDL
    // thread — device close can wait on that thread (Samsung Accept hang).
    if (thread_.joinable()) {
      stop_ = true;
      finishing = std::move(thread_);
    }
  }
  if (finishing.joinable()) {
    std::thread([t = std::move(finishing)]() mutable {
      if (t.joinable()) {
        t.join();
      }
    }).detach();
  }
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

void CallRingtone::Stop(const bool wait) {
  stop_ = true;
  std::thread finishing;
  {
    std::lock_guard lock(mutex_);
    playing_ = false;
    if (thread_.joinable()) {
      finishing = std::move(thread_);
    }
  }
  if (!finishing.joinable()) {
    return;
  }
  if (wait) {
    finishing.join();
    return;
  }
  // Never join the ringtone worker on the Accept/UI click path: that runs inside
  // SDL/Rml event dispatch, and device close on the worker can wait on that thread.
  std::thread([t = std::move(finishing)]() mutable {
    if (t.joinable()) {
      t.join();
    }
  }).detach();
}

void CallRingtone::Stop() {
  Stop(/*wait=*/false);
}

void CallRingtone::RunLoop() {
  if (!SDL_WasInit(SDL_INIT_AUDIO)) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
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
    playing_ = false;
    return;
  }
  const SDL_AudioDeviceID device = SDL_GetAudioStreamDevice(stream);
  (void)SDL_ResumeAudioDevice(device);

  while (!stop_.load()) {
    const int queued = SDL_GetAudioStreamQueued(stream);
    if (queued < want.freq * static_cast<int>(sizeof(int16_t)) / 2) {
      (void)SDL_PutAudioStreamData(stream, wav_pcm_.data(), static_cast<int>(wav_pcm_.size()));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
  }

  SDL_ClearAudioStream(stream);
  SDL_DestroyAudioStream(stream);
  SDL_CloseAudioDevice(device);
  playing_ = false;
}

} // namespace pbr
