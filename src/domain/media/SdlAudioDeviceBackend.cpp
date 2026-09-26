#include "domain/media/CallAudioSession.h"
#include "domain/media/MediaDeviceArbiter.h"
#include "domain/media/SdlAudioBootstrap.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <thread>

namespace pbr {
namespace {

class SdlAudioEndpoint final : public IAudioEndpoint {
public:
  explicit SdlAudioEndpoint(SDL_AudioStream* stream) : stream_(stream) {}
  ~SdlAudioEndpoint() override {
    // A stream from SDL_OpenAudioDeviceStream owns its device: destroying it closes the device.
    // Never SDL_CloseAudioDevice afterwards (double-free / tcache abort on Linux, quit hang on Android).
    SDL_DestroyAudioStream(stream_);
  }
  int Read(void* dst, int bytes) override { return SDL_GetAudioStreamData(stream_, dst, bytes); }
  bool Write(const void* src, int bytes) override { return SDL_PutAudioStreamData(stream_, src, bytes); }
  int Queued() const override { return SDL_GetAudioStreamQueued(stream_); }
  void Clear() override { (void)SDL_ClearAudioStream(stream_); }

private:
  SDL_AudioStream* stream_;
};

SDL_AudioSpec ToSdlSpec(const AudioDeviceFormat& format) {
  SDL_AudioSpec spec{};
  spec.freq = format.freq;
  spec.format = SDL_AUDIO_S16;
  spec.channels = static_cast<Uint8>(format.channels);
  return spec;
}

/** Open + resume once; null with `error` on failure. */
SDL_AudioStream* OpenResumed(SDL_AudioDeviceID which, const SDL_AudioSpec& spec, std::string* error) {
  SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(which, &spec, nullptr, nullptr);
  if (!stream) {
    *error = std::string("open failed: ") + SDL_GetError();
    return nullptr;
  }
  if (!SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(stream))) {
    *error = std::string("resume failed: ") + SDL_GetError();
    SDL_DestroyAudioStream(stream);
    return nullptr;
  }
  return stream;
}

class SdlAudioDeviceBackend final : public IAudioDeviceBackend {
public:
  std::unique_ptr<IAudioEndpoint> Open(MediaDeviceKind kind, const AudioDeviceFormat& format,
                                       const std::function<bool()>& still_wanted, std::string* error) override {
    std::string err;
    if (!EnsureSdlAudioSubsystem()) {
      err = std::string("SDL_InitSubSystem(AUDIO) failed: ") + SDL_GetError();
    } else if (auto* stream = kind == MediaDeviceKind::Mic ? OpenMic(format, still_wanted, &err)
                                                           : OpenResumed(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                                                         ToSdlSpec(format), &err)) {
      return std::make_unique<SdlAudioEndpoint>(stream);
    }
    SDL_Log("MediaDeviceArbiter: %s %s", MediaDeviceKindName(kind), err.c_str());
    if (error) {
      *error = err;
    }
    return nullptr;
  }

  std::chrono::milliseconds ReopenSettle(MediaDeviceKind kind) const override {
    // Android: let the OEM speaker route settle before AudioRecord reopens (Moto safety ramp).
    return kind == MediaDeviceKind::Mic ? std::chrono::milliseconds(CallAudioSession::CaptureReopenSettleDelayMs())
                                        : std::chrono::milliseconds(0);
  }

private:
  /** DEFAULT_RECORDING with platform retries — OEM speaker route (Moto) races AAudio open. */
  static SDL_AudioStream* OpenMic(const AudioDeviceFormat& format, const std::function<bool()>& still_wanted,
                                  std::string* error) {
    const SDL_AudioSpec spec = ToSdlSpec(format);
    const int attempts = CallAudioSession::CaptureOpenAttemptCount();
    for (int attempt = 0; attempt < attempts; ++attempt) {
      if (const int delay_ms = CallAudioSession::CaptureOpenRetryDelayMs(attempt); delay_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
      }
      if (!still_wanted()) {
        *error = "open abandoned";
        return nullptr;
      }
      if (auto* stream = OpenResumed(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, spec, error)) {
        return stream;
      }
      SDL_Log("MediaDeviceArbiter: mic attempt %d/%d: %s", attempt + 1, attempts, error->c_str());
    }
    return nullptr;
  }
};

} // namespace

std::unique_ptr<IAudioDeviceBackend> CreateSdlAudioDeviceBackend() {
  return std::make_unique<SdlAudioDeviceBackend>();
}

} // namespace pbr
