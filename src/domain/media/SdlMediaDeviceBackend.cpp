#include "domain/media/CallAudioSession.h"
#include "domain/media/CameraCaptureOrientation.h"
#include "domain/media/MediaDeviceArbiter.h"
#include "domain/media/SdlAudioBootstrap.h"
#include "domain/media/VideoYuv.h"
#include "domain/media/VoiceProcessingIo.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
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

/** One OS voice-processing unit shared by its mic and speaker halves; closes with the last half. */
class VoiceUnit final : public IVoiceProcessing {
public:
  ~VoiceUnit() override {
    const auto t0 = std::chrono::steady_clock::now();
    io.Close();  // AudioOutputUnitStop waits for its callbacks
    SDL_Log("MediaDeviceArbiter: vpio close_ms=%lld",
            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - t0)
                                       .count()));
  }
  size_t RenderChunkBytes() const override { return io.RenderChunkBytes(); }
  uint64_t PlayoutUnderruns() const override { return io.PlayoutUnderruns(); }
  std::string TakeDiag() override { return io.TakeDiag(); }
  bool TakeDeviceChanged() override { return io.TakeDeviceChanged(); }

  // Const-callable on the unit: the halves' queries are const on IAudioEndpoint.
  mutable VoiceProcessingIo io;
};

class VoiceMicEndpoint final : public IAudioEndpoint {
public:
  explicit VoiceMicEndpoint(std::shared_ptr<VoiceUnit> unit) : unit_(std::move(unit)) {}
  int Read(void* dst, int bytes) override {
    const size_t samples = unit_->io.ReadCapture(static_cast<int16_t*>(dst), static_cast<size_t>(bytes) / sizeof(int16_t));
    return static_cast<int>(samples * sizeof(int16_t));
  }
  bool Write(const void*, int) override { return false; }
  int Queued() const override { return 0; }
  void Clear() override {}
  IVoiceProcessing* VoiceProcessing() override { return unit_.get(); }

private:
  std::shared_ptr<VoiceUnit> unit_;
};

class VoiceSpeakerEndpoint final : public IAudioEndpoint {
public:
  explicit VoiceSpeakerEndpoint(std::shared_ptr<VoiceUnit> unit) : unit_(std::move(unit)) {}
  int Read(void*, int) override { return 0; }
  bool Write(const void* src, int bytes) override {
    const size_t samples = static_cast<size_t>(bytes) / sizeof(int16_t);
    return unit_->io.WritePlayout(static_cast<const int16_t*>(src), samples) == samples;
  }
  int Queued() const override { return static_cast<int>(unit_->io.QueuedPlayoutBytes()); }
  void Clear() override {}
  IVoiceProcessing* VoiceProcessing() override { return unit_.get(); }

private:
  std::shared_ptr<VoiceUnit> unit_;
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

/** Windows MF often returns NV12/YUY2 (sometimes bottom-up / negative pitch). Android usually
 *  converts cleanly via SDL; desktop needs pitch-safe + YUV fallbacks or preview stays empty. */
bool ConvertCameraSurfaceToRgba(SDL_Surface* surface, VideoFrameRgba& out) {
  if (!surface || !surface->pixels || surface->w < 2 || surface->h < 2) {
    return false;
  }

  SDL_Surface* owned = nullptr;
  SDL_Surface* src = surface;

  // Normalize negative pitch (MF bottom-up) into a positive-pitch duplicate. SDL YUV converters
  // treat pitch as Uint32 and mis-locate the UV plane when pitch is negative.
  if (surface->pitch < 0) {
    const int abs_pitch = -surface->pitch;
    owned = SDL_CreateSurface(surface->w, surface->h, surface->format);
    if (!owned || !owned->pixels) {
      if (owned) {
        SDL_DestroySurface(owned);
      }
      return false;
    }
    const auto* src_bottom = static_cast<const uint8_t*>(surface->pixels);
    auto* dst_base = static_cast<uint8_t*>(owned->pixels);
    if (SDL_ISPIXELFORMAT_FOURCC(surface->format)) {
      // Contiguous planar: Y rows are bottom-up; UV plane follows the Y plane in memory.
      const uint8_t* y_top = src_bottom + surface->pitch * (surface->h - 1);
      for (int y = 0; y < surface->h; ++y) {
        std::memcpy(dst_base + static_cast<size_t>(y) * static_cast<size_t>(owned->pitch),
                    y_top + static_cast<size_t>(y) * static_cast<size_t>(abs_pitch),
                    static_cast<size_t>(std::min(abs_pitch, owned->pitch)));
      }
      const size_t y_bytes = static_cast<size_t>(abs_pitch) * static_cast<size_t>(surface->h);
      const uint8_t* uv_src = y_top + y_bytes;
      uint8_t* uv_dst = dst_base + static_cast<size_t>(owned->pitch) * static_cast<size_t>(surface->h);
      const int uv_rows = (surface->format == SDL_PIXELFORMAT_NV12 ||
                           surface->format == SDL_PIXELFORMAT_NV21)
                              ? (surface->h + 1) / 2
                              : surface->h / 2;
      for (int y = 0; y < uv_rows; ++y) {
        std::memcpy(uv_dst + static_cast<size_t>(y) * static_cast<size_t>(owned->pitch),
                    uv_src + static_cast<size_t>(y) * static_cast<size_t>(abs_pitch),
                    static_cast<size_t>(std::min(abs_pitch, owned->pitch)));
      }
    } else {
      // Packed RGB/YUV: pixels points at the last row; rebuild top-down.
      const uint8_t* top = src_bottom + surface->pitch * (surface->h - 1);
      const int row_bytes = std::min(abs_pitch, owned->pitch);
      for (int y = 0; y < surface->h; ++y) {
        std::memcpy(dst_base + static_cast<size_t>(y) * static_cast<size_t>(owned->pitch),
                    top + static_cast<size_t>(y) * static_cast<size_t>(abs_pitch),
                    static_cast<size_t>(row_bytes));
      }
    }
    SDL_SetSurfaceColorspace(owned, SDL_GetSurfaceColorspace(surface));
    src = owned;
  }

  bool ok = false;
  SDL_Surface* converted =
      SDL_ConvertSurfaceAndColorspace(src, SDL_PIXELFORMAT_RGBA32, nullptr, SDL_COLORSPACE_SRGB, 0);
  if (converted && converted->pixels) {
    ok = CopyRgbToRgba(static_cast<const uint8_t*>(converted->pixels), converted->w, converted->h,
                       converted->pitch, true, out);
    SDL_DestroySurface(converted);
  }

  if (!ok) {
    const int pitch = src->pitch > 0 ? src->pitch : -src->pitch;
    if (src->format == SDL_PIXELFORMAT_NV12) {
      ok = Nv12ToRgba(static_cast<const uint8_t*>(src->pixels), src->w, src->h, pitch, out);
    } else if (src->format == SDL_PIXELFORMAT_YUY2) {
      ok = Yuy2ToRgba(static_cast<const uint8_t*>(src->pixels), src->w, src->h, pitch, out);
    } else if (src->format == SDL_PIXELFORMAT_RGB24 || src->format == SDL_PIXELFORMAT_RGBA32 ||
               src->format == SDL_PIXELFORMAT_XRGB8888 || src->format == SDL_PIXELFORMAT_ARGB8888 ||
               src->format == SDL_PIXELFORMAT_XBGR8888 || src->format == SDL_PIXELFORMAT_ABGR8888) {
      // Packed RGB with known channel layouts — try SDL again already failed; treat as byte RGB.
      const bool has_alpha = SDL_BYTESPERPIXEL(src->format) >= 4;
      ok = CopyRgbToRgba(static_cast<const uint8_t*>(src->pixels), src->w, src->h, pitch, has_alpha,
                         out);
    }
  }

  if (owned) {
    SDL_DestroySurface(owned);
  }
  if (ok) {
    ForceOpaqueAlphaInPlace(out.rgba);
  }
  return ok;
}

class SdlCameraEndpoint final : public ICameraEndpoint {
public:
  SdlCameraEndpoint(SDL_Camera* camera, CameraGeometry geometry) : camera_(camera), geometry_(geometry) {}
  ~SdlCameraEndpoint() override { SDL_CloseCamera(camera_); }

  CameraGeometry Geometry() const override { return geometry_; }

  std::optional<VideoFrameRgba> NextFrame() override {
    Uint64 timestamp_ns = 0;
    SDL_Surface* surface = SDL_AcquireCameraFrame(camera_, &timestamp_ns);
    if (!surface) {
      return std::nullopt;  // none ready, or permission still pending
    }
    VideoFrameRgba rgba;
    const bool ok = ConvertCameraSurfaceToRgba(surface, rgba);
    if (!ok && !logged_convert_fail_) {
      logged_convert_fail_ = true;
      SDL_Log("MediaDeviceArbiter: camera frame convert failed (format=%s pitch=%d %dx%d): %s",
              SDL_GetPixelFormatName(surface->format), surface->pitch, surface->w, surface->h, SDL_GetError());
    }
    SDL_ReleaseCameraFrame(camera_, surface);
    if (!ok) {
      return std::nullopt;
    }
    return rgba;
  }

private:
  SDL_Camera* camera_;
  CameraGeometry geometry_;
  bool logged_convert_fail_ = false;
};

class SdlMediaDeviceBackend final : public IMediaDeviceBackend {
public:
  std::unique_ptr<IAudioEndpoint> OpenAudio(MediaDeviceKind kind, const AudioDeviceFormat& format,
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

  std::unique_ptr<ICameraEndpoint> OpenCamera(const CameraRequestFormat& format, std::string* error) override {
    const auto fail = [error](std::string message) -> std::unique_ptr<ICameraEndpoint> {
      SDL_Log("MediaDeviceArbiter: camera %s", message.c_str());
      if (error) {
        *error = std::move(message);
      }
      return nullptr;
    };
    if (!SDL_WasInit(SDL_INIT_CAMERA) && !SDL_InitSubSystem(SDL_INIT_CAMERA)) {
      return fail(std::string("SDL_InitSubSystem(CAMERA) failed: ") + SDL_GetError());
    }
    int count = 0;
    SDL_CameraID* cameras = SDL_GetCameras(&count);
    if (!cameras || count <= 0) {
      if (cameras) {
        SDL_free(cameras);
      }
      return fail(std::string("No camera: ") + SDL_GetError());
    }
    SDL_CameraID chosen = cameras[0];
    for (int i = 0; i < count; ++i) {
      if (SDL_GetCameraPosition(cameras[i]) == SDL_CAMERA_POSITION_FRONT_FACING) {
        chosen = cameras[i];
        break;
      }
    }
    SDL_free(cameras);

    const CameraCaptureTransform xform = ResolveCameraCaptureTransform(chosen, format.display_rotation_deg);
    CameraGeometry geometry;
    geometry.rotate_cw = xform.rotate_cw;
    geometry.encode_width = xform.encode_width;
    geometry.encode_height = xform.encode_height;
    geometry.front_facing = xform.front_facing;

    SDL_CameraSpec want{};
    // Prefer a convertible packed/YUV format. UNKNOWN picks the driver's first enum entry (often
    // MJPG/NV12 on Windows); conversion happens per frame. Landscape sensor buffers; the holder
    // rotates / crops into the encode size.
    want.format = SDL_PIXELFORMAT_UNKNOWN;
    want.width = std::max(geometry.encode_width, geometry.encode_height);
    want.height = std::min(geometry.encode_width, geometry.encode_height);
    want.framerate_numerator = format.fps;
    want.framerate_denominator = 1;
    SDL_Camera* camera = SDL_OpenCamera(chosen, &want);
    if (!camera) {
      // Fall back: ask SDL to deliver RGBA so the driver converts when possible.
      want.format = SDL_PIXELFORMAT_RGBA32;
      camera = SDL_OpenCamera(chosen, &want);
    }
    if (!camera) {
      camera = SDL_OpenCamera(chosen, nullptr);
    }
    if (!camera) {
      return fail(std::string("SDL_OpenCamera failed: ") + SDL_GetError());
    }
    return std::make_unique<SdlCameraEndpoint>(camera, geometry);
  }

  VoiceDuplexEndpoints OpenVoiceDuplex(const AudioDeviceFormat& format, std::string* error) override {
    // VoiceProcessingIo is Apple VPIO on macOS and iOS; elsewhere a stub whose Open() fails "unsupported".
    if (format.freq != 48000 || format.channels != 1) {
      *error = "voice processing runs 48 kHz mono only";
      return {};
    }
    auto unit = std::make_shared<VoiceUnit>();
    const auto t0 = std::chrono::steady_clock::now();
    std::string reason;
    const bool ok = unit->io.Open(&reason);
    const auto open_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    if (!ok) {
      if (reason != "unsupported") {
        SDL_Log("MediaDeviceArbiter: vpio open failed (%s) open_ms=%lld", reason.c_str(), static_cast<long long>(open_ms));
      }
      *error = reason.empty() ? std::string("vpio open failed") : reason;
      unit->io.Close();
      return {};
    }
    SDL_Log("MediaDeviceArbiter: vpio open (voice processing: echo cancellation on) open_ms=%lld",
            static_cast<long long>(open_ms));
    VoiceDuplexEndpoints pair;
    pair.mic = std::make_unique<VoiceMicEndpoint>(unit);
    pair.speaker = std::make_unique<VoiceSpeakerEndpoint>(unit);
    return pair;
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

std::unique_ptr<IMediaDeviceBackend> CreateSdlMediaDeviceBackend() {
  return std::make_unique<SdlMediaDeviceBackend>();
}

} // namespace pbr
