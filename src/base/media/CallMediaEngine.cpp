#include "base/media/CallMediaEngine.h"

#include "base/media/CallAudioSession.h"
#include "base/media/CallMediaPlayout.h"
#include "base/media/CameraCaptureOrientation.h"
#include "base/media/IVideoCodec.h"
#include "base/media/VideoYuv.h"
#include "common/Utilities.h"

#include <SDL3/SDL.h>
#include <opus.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pbr {
namespace {

constexpr int kSampleRate = 48000;
constexpr int kChannels = 1;
constexpr int kFrameMs = 20;
constexpr int kFrameSamples = kSampleRate * kFrameMs / 1000;
constexpr int kDefaultVideoWidth = 640;
constexpr int kDefaultVideoHeight = 360;
constexpr int kVideoFps = 20;
/** Soft stall: keep last frame; UI may show Reconnecting… */
constexpr int64_t kRemoteVideoStallSoftMs = 2000;
/** Hard stall: drop last frame so the tile does not freeze forever. */
constexpr int64_t kRemoteVideoStallHardMs = 5000;

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

} // namespace

struct CallMediaEngine::Impl {
  std::recursive_mutex mutex;
  std::string call_id;
  std::atomic<bool> active{false};
  std::atomic<bool> connected{false};
  std::atomic<bool> muted{false};
  std::atomic<bool> camera_enabled{false};
  std::atomic<bool> has_remote_video{false};
  std::atomic<bool> ever_had_remote_video{false};
  std::atomic<int64_t> connected_at_ms{0};
  std::atomic<int64_t> started_at_ms{0};
  std::atomic<int64_t> last_remote_video_ms{0};
  std::string connection_state = "idle";

  StateChangedFn on_state_changed;

  bool sfu_mode = false;
  /** Shared so SoftMigrate can replace the callback while capture/video still invoke the old one. */
  std::shared_ptr<SfuSendFn> sfu_send;
  /**
   * Held around (*sfu_send)(...) in capture/video threads. StartSfu takes this after swapping the
   * callback so SoftMigrate ReleaseDirectTransport cannot Detach while an old send is mid-write
   * (Linux: malloc unaligned tcache / Aborted right after 2nd join).
   */
  std::mutex sfu_send_call_mu;
  std::atomic<uint32_t> sfu_audio_seq{0};
  std::atomic<uint32_t> sfu_video_seq{0};
  std::atomic<bool> adaptation_camera_allowed{true};
  int64_t adaptation_target_video_bps = 0;
  std::atomic<int64_t> adaptation_target_audio_bps{CallMediaAdaptation::kComfortAudioBps};
  std::atomic<double> path_pressure{0.0};
  std::atomic<uint64_t> outbound_drops{0};
  std::atomic<uint64_t> playout_ticks{0};
  std::atomic<uint64_t> rx_audio_frames{0};
  std::atomic<uint64_t> tx_audio_frames{0};
  std::atomic<uint64_t> playout_underruns_total{0};
  std::atomic<uint64_t> plc_frames_total{0};
  std::atomic<int64_t> last_rx_audio_ms{0};
  std::atomic<int64_t> last_tx_audio_ms{0};

  bool capture_available = false;

  OpusEncoder* encoder = nullptr;
  SDL_AudioStream* capture_stream = nullptr;
  SDL_AudioStream* playback_stream = nullptr;
  SDL_AudioDeviceID capture_device = 0;
  SDL_AudioDeviceID playback_device = 0;

  struct RemoteAudioTrack {
    OpusDecoder* decoder = nullptr;
    AudioJitterBuffer jitter;
    ~RemoteAudioTrack() {
      if (decoder) {
        opus_decoder_destroy(decoder);
        decoder = nullptr;
      }
    }
  };
  /** Per publisher stream_id (V032). Guarded by mutex (decode + playout mix). */
  std::unordered_map<uint32_t, std::unique_ptr<RemoteAudioTrack>> audio_tracks;

  std::unique_ptr<IVideoCodec> video_codec;
  SDL_Camera* camera = nullptr;
  SDL_CameraID camera_id = 0;
  /** Clockwise degrees to apply to sensor buffers before encode. */
  int camera_rotate_cw = 0;
  int encode_width = kDefaultVideoWidth;
  int encode_height = kDefaultVideoHeight;

  std::thread capture_thread;
  std::thread video_thread;
  std::thread playout_thread;
  std::atomic<bool> capture_running{false};
  std::atomic<bool> video_running{false};
  std::atomic<bool> playout_running{false};
  std::atomic<float> local_input_level{0.f};
  std::atomic<float> remote_output_level{0.f};
  std::atomic<int64_t> remote_level_ms{0};

  mutable std::mutex video_frame_mutex;
  VideoTileFrame local_video_frame;
  VideoTileFrame remote_video_frame;
  uint64_t local_video_seq = 0;
  uint64_t remote_video_seq = 0;

  static float FramePeakLevel(const int16_t* pcm, int samples) {
    int peak = 0;
    for (int i = 0; i < samples; ++i) {
      peak = std::max(peak, std::abs(static_cast<int>(pcm[i])));
    }
    return std::clamp(static_cast<float>(peak) / 32768.f, 0.f, 1.f);
  }

  static void SmoothLevel(std::atomic<float>& level, float instant) {
    const float cur = level.load(std::memory_order_relaxed);
    const float next = instant >= cur ? instant : (cur * 0.82f + instant * 0.18f);
    level.store(std::clamp(next, 0.f, 1.f), std::memory_order_relaxed);
  }

  /** Update connection fields only. Never invoke on_state_changed here — callers may
   *  already hold `mutex`, and callbacks (ActiveCallId / SFU migrate) re-enter the engine. */
  void ApplyStateLocked(const std::string& state) {
    connection_state = state;
    const bool now_connected = (state == "connected");
    if (now_connected && !connected.load()) {
      connected_at_ms.store(util::NowUnixMs(), std::memory_order_relaxed);
    }
    if (!now_connected) {
      connected_at_ms.store(0, std::memory_order_relaxed);
    }
    connected = now_connected;
    if (state == "failed" || state == "closed") {
      ClearRemoteVideoFrames();
    }
  }

  void EmitStateChanged(const std::string& state) {
    StateChangedFn cb;
    {
      std::lock_guard lock(mutex);
      cb = on_state_changed;
    }
    if (cb) {
      cb(state);
    }
  }

  /** External threads: apply under lock, then notify outside the lock. */
  void SetState(const std::string& state) {
    {
      std::lock_guard lock(mutex);
      ApplyStateLocked(state);
    }
    EmitStateChanged(state);
  }

  void PublishLocalPreview(const VideoFrameRgba& frame) {
    VideoFrameRgba copy = frame;
    PremultiplyRgbaInPlace(copy.rgba);
    std::lock_guard lock(video_frame_mutex);
    local_video_frame.width = copy.width;
    local_video_frame.height = copy.height;
    local_video_frame.rgba = std::move(copy.rgba);
    local_video_frame.seq = ++local_video_seq;
  }

  void PublishRemoteFrame(VideoFrameRgba&& frame) {
    PremultiplyRgbaInPlace(frame.rgba);
    std::lock_guard lock(video_frame_mutex);
    remote_video_frame.width = frame.width;
    remote_video_frame.height = frame.height;
    remote_video_frame.rgba = std::move(frame.rgba);
    remote_video_frame.seq = ++remote_video_seq;
    has_remote_video.store(true, std::memory_order_relaxed);
    ever_had_remote_video.store(true, std::memory_order_relaxed);
    last_remote_video_ms.store(util::NowUnixMs(), std::memory_order_relaxed);
  }

  void ClearRemoteVideoFrames() {
    std::lock_guard lock(video_frame_mutex);
    remote_video_frame = {};
    remote_video_seq = 0;
    has_remote_video.store(false, std::memory_order_relaxed);
    last_remote_video_ms.store(0, std::memory_order_relaxed);
  }

  void ClearVideoFrames() {
    std::lock_guard lock(video_frame_mutex);
    local_video_frame = {};
    remote_video_frame = {};
    local_video_seq = 0;
    remote_video_seq = 0;
    has_remote_video.store(false, std::memory_order_relaxed);
    ever_had_remote_video.store(false, std::memory_order_relaxed);
    last_remote_video_ms.store(0, std::memory_order_relaxed);
  }

  void CloseCameraLocked() {
    video_running = false;
    if (video_thread.joinable()) {
      video_thread.join();
    }
    if (camera) {
      SDL_CloseCamera(camera);
      camera = nullptr;
      camera_id = 0;
    }
    camera_rotate_cw = 0;
    encode_width = kDefaultVideoWidth;
    encode_height = kDefaultVideoHeight;
    camera_enabled.store(false, std::memory_order_relaxed);
    if (video_codec) {
      video_codec->ResetEncoder();
    }
    {
      std::lock_guard lock(video_frame_mutex);
      local_video_frame = {};
      local_video_frame.seq = ++local_video_seq;
    }
  }

  void ClearAudioTracksLocked() {
    audio_tracks.clear();
  }

  void TearDownAudioLocked() {
    // Caller must not hold mutex across JoinCaptureThread — capture may call sfu_send /
    // OnSfuPacket which need the same mutex (deadlock + SDL double-free on quit).
    capture_running = false;
    playout_running = false;
    CloseCameraLocked();
    if (capture_stream) {
      SDL_DestroyAudioStream(capture_stream);
      capture_stream = nullptr;
    }
    if (playback_stream) {
      SDL_DestroyAudioStream(playback_stream);
      playback_stream = nullptr;
    }
    if (capture_device) {
      SDL_CloseAudioDevice(capture_device);
      capture_device = 0;
    }
    if (playback_device) {
      SDL_CloseAudioDevice(playback_device);
      playback_device = 0;
    }
    if (encoder) {
      opus_encoder_destroy(encoder);
      encoder = nullptr;
    }
    ClearAudioTracksLocked();
    if (video_codec) {
      video_codec->ResetEncoder();
      video_codec->ResetDecoder();
    }
    ClearVideoFrames();
    local_input_level.store(0.f, std::memory_order_relaxed);
    remote_output_level.store(0.f, std::memory_order_relaxed);
    remote_level_ms.store(0, std::memory_order_relaxed);
    path_pressure.store(0.0, std::memory_order_relaxed);
    outbound_drops.store(0, std::memory_order_relaxed);
    playout_ticks.store(0, std::memory_order_relaxed);
    rx_audio_frames.store(0, std::memory_order_relaxed);
    tx_audio_frames.store(0, std::memory_order_relaxed);
    playout_underruns_total.store(0, std::memory_order_relaxed);
    plc_frames_total.store(0, std::memory_order_relaxed);
    last_rx_audio_ms.store(0, std::memory_order_relaxed);
    last_tx_audio_ms.store(0, std::memory_order_relaxed);
    capture_available = false;
    CallAudioSession::Deactivate();
  }

  void JoinCaptureThread() {
    capture_running = false;
    if (capture_thread.joinable()) {
      capture_thread.join();
    }
  }

  void JoinPlayoutThread() {
    playout_running = false;
    if (playout_thread.joinable()) {
      playout_thread.join();
    }
  }

  void StartPlayoutLoop() {
    playout_running = true;
    playout_thread = std::thread([this]() {
      std::vector<int16_t> mix(static_cast<size_t>(kFrameSamples), 0);
      while (playout_running.load(std::memory_order_relaxed)) {
        const auto t0 = std::chrono::steady_clock::now();
        std::fill(mix.begin(), mix.end(), 0);
        bool any = false;
        double pressure = 0.0;
        uint64_t ticks = 0;
        {
          std::lock_guard lock(mutex);
          ticks = playout_ticks.fetch_add(1, std::memory_order_relaxed) + 1;
          for (auto& [id, track] : audio_tracks) {
            (void)id;
            if (!track) {
              continue;
            }
            auto frame = track->jitter.PopForPlayout(true);
            if (frame) {
              MixPcmSat(mix, frame->pcm);
              any = true;
            } else {
              playout_underruns_total.fetch_add(1, std::memory_order_relaxed);
              // PLC: opus_decode with null packet into a temp buffer, then mix.
              if (track->decoder) {
                std::vector<int16_t> plc(static_cast<size_t>(kFrameSamples), 0);
                const int decoded =
                    opus_decode(track->decoder, nullptr, 0, plc.data(), kFrameSamples, 0);
                if (decoded > 0) {
                  plc.resize(static_cast<size_t>(decoded));
                  MixPcmSat(mix, plc);
                  any = true;
                  plc_frames_total.fetch_add(1, std::memory_order_relaxed);
                }
              }
            }
            pressure = std::max(pressure, track->jitter.Pressure(std::max<uint64_t>(ticks, 1)));
          }
          SDL_AudioStream* out = playback_stream;
          if (out && any) {
            SmoothLevel(remote_output_level, FramePeakLevel(mix.data(), kFrameSamples));
            remote_level_ms.store(util::NowUnixMs(), std::memory_order_relaxed);
            (void)SDL_PutAudioStreamData(out, mix.data(),
                                         kFrameSamples * static_cast<int>(sizeof(int16_t)));
          } else if (out && !audio_tracks.empty()) {
            // Silent frame keeps SDL clock alive when all streams priming.
            (void)SDL_PutAudioStreamData(out, mix.data(),
                                         kFrameSamples * static_cast<int>(sizeof(int16_t)));
          }
        }
        const double drop_p =
            std::min(1.0, static_cast<double>(outbound_drops.load(std::memory_order_relaxed)) / 50.0);
        path_pressure.store(std::clamp(std::max(pressure, drop_p * 0.5), 0.0, 1.0),
                            std::memory_order_relaxed);
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        const auto period = std::chrono::milliseconds(kFrameMs);
        if (elapsed < period) {
          std::this_thread::sleep_for(period - elapsed);
        }
      }
    });
  }

  Roe<void> EnsureAudioSubsystem() {
    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
      if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        return Error(std::string("SDL_InitSubSystem(AUDIO) failed: ") + SDL_GetError());
      }
    }
    return {};
  }

  Roe<void> EnsureCameraSubsystem() {
    if (!SDL_WasInit(SDL_INIT_CAMERA)) {
      if (!SDL_InitSubSystem(SDL_INIT_CAMERA)) {
        return Error(std::string("SDL_InitSubSystem(CAMERA) failed: ") + SDL_GetError());
      }
    }
    return {};
  }

  Roe<void> EnsureOpusCodecs() {
    if (encoder) {
      return {};
    }
    int err = 0;
    encoder = opus_encoder_create(kSampleRate, kChannels, OPUS_APPLICATION_VOIP, &err);
    if (!encoder || err != OPUS_OK) {
      return Error("opus_encoder_create failed");
    }
    const int64_t bps = adaptation_target_audio_bps.load(std::memory_order_relaxed);
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(static_cast<int>(bps > 0 ? bps : 24000)));
    return {};
  }

  RemoteAudioTrack* EnsureRemoteTrackLocked(uint32_t stream_id) {
    auto it = audio_tracks.find(stream_id);
    if (it != audio_tracks.end() && it->second) {
      return it->second.get();
    }
    auto track = std::make_unique<RemoteAudioTrack>();
    int err = 0;
    track->decoder = opus_decoder_create(kSampleRate, kChannels, &err);
    if (!track->decoder || err != OPUS_OK) {
      return nullptr;
    }
    RemoteAudioTrack* raw = track.get();
    audio_tracks[stream_id] = std::move(track);
    return raw;
  }

  /**
   * Open SDL capture/playback. May block for a long time on OS mic permission
   * (macOS TCC). Call only from the capture worker — never from UI, relay IO,
   * or the libp2p host thread (that freezes accept + peer signaling).
   */
  Roe<void> OpenAudioDevices() {
    if (auto ok = EnsureAudioSubsystem(); !ok) {
      return ok.error();
    }

    CallAudioSession::ActivateForVoipCall();

    SDL_AudioSpec want{};
    want.freq = kSampleRate;
    want.format = SDL_AUDIO_S16;
    want.channels = static_cast<Uint8>(kChannels);

    capture_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &want, nullptr, nullptr);
    capture_available = false;
    if (capture_stream) {
      capture_device = SDL_GetAudioStreamDevice(capture_stream);
      if (SDL_ResumeAudioDevice(capture_device)) {
        capture_available = true;
      } else {
        SDL_DestroyAudioStream(capture_stream);
        capture_stream = nullptr;
        capture_device = 0;
      }
    }

    playback_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want, nullptr, nullptr);
    if (!playback_stream) {
      return Error(std::string("SDL playback open failed: ") + SDL_GetError());
    }
    playback_device = SDL_GetAudioStreamDevice(playback_stream);
    if (!SDL_ResumeAudioDevice(playback_device)) {
      return Error(std::string("SDL playback resume failed: ") + SDL_GetError());
    }
    return {};
  }

  void StartCaptureLoop() {
    // Precondition: capture_thread not joinable (JoinCaptureThread outside media mutex).
    capture_running = true;
    capture_thread = std::thread([this]() {
      // Device open (and OS mic prompts) stay on this worker so CallAccept /
      // AcceptInvite can finish signaling without freezing UI or libp2p.
      if (auto audio = OpenAudioDevices(); !audio) {
        SDL_Log("CallMediaEngine: OpenAudioDevices failed: %s", audio.error().message.c_str());
      } else if (!capture_available) {
        SDL_Log("CallMediaEngine: started without capture device — sending silence; playback still active");
      }
      if (!capture_running.load()) {
        return;
      }

      std::vector<int16_t> pcm(static_cast<size_t>(kFrameSamples));
      std::vector<int16_t> pending;
      pending.reserve(static_cast<size_t>(kFrameSamples) * 2);
      std::vector<unsigned char> opus_buf(4000);
      while (capture_running.load()) {
        std::shared_ptr<SfuSendFn> send_fn;
        OpusEncoder* enc = nullptr;
        bool can_send = false;
        {
          std::lock_guard lock(mutex);
          enc = encoder;
          can_send = sfu_mode && static_cast<bool>(sfu_send) && enc;
          if (can_send) {
            send_fn = sfu_send;
          }
        }
        if (capture_stream) {
          int16_t chunk[kFrameSamples];
          const int got = SDL_GetAudioStreamData(capture_stream, chunk, static_cast<int>(sizeof(chunk)));
          if (got > 0) {
            const size_t samples = static_cast<size_t>(got) / sizeof(int16_t);
            pending.insert(pending.end(), chunk, chunk + samples);
          }
          if (pending.size() < static_cast<size_t>(kFrameSamples)) {
            if (got <= 0) {
              if (!muted.load(std::memory_order_relaxed)) {
                SmoothLevel(local_input_level, 0.f);
              }
              const int64_t now = util::NowUnixMs();
              if (now - remote_level_ms.load(std::memory_order_relaxed) > 40) {
                SmoothLevel(remote_output_level, 0.f);
              }
              std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            continue;
          }
          std::copy_n(pending.begin(), kFrameSamples, pcm.begin());
          pending.erase(pending.begin(), pending.begin() + kFrameSamples);
          if (muted.load(std::memory_order_relaxed)) {
            std::fill(pcm.begin(), pcm.end(), 0);
            SmoothLevel(local_input_level, 0.f);
          } else {
            SmoothLevel(local_input_level, FramePeakLevel(pcm.data(), kFrameSamples));
          }
        } else {
          std::fill(pcm.begin(), pcm.end(), 0);
          SmoothLevel(local_input_level, 0.f);
          if (!can_send) {
            const int64_t now = util::NowUnixMs();
            if (now - remote_level_ms.load(std::memory_order_relaxed) > 40) {
              SmoothLevel(remote_output_level, 0.f);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kFrameMs));
            continue;
          }
        }
        {
          const int64_t now = util::NowUnixMs();
          if (now - remote_level_ms.load(std::memory_order_relaxed) > 40) {
            SmoothLevel(remote_output_level, 0.f);
          }
        }
        if (!can_send || !enc) {
          continue;
        }
        const int encoded =
            opus_encode(enc, pcm.data(), kFrameSamples, opus_buf.data(), static_cast<int>(opus_buf.size()));
        if (encoded <= 0) {
          continue;
        }
        if (send_fn) {
          SfuPacket pkt;
          pkt.channel_id = 0;
          pkt.seq = sfu_audio_seq.fetch_add(1) + 1;
          pkt.payload.assign(opus_buf.data(), opus_buf.data() + encoded);
          try {
            std::lock_guard send_lock(sfu_send_call_mu);
            (*send_fn)(pkt);
            tx_audio_frames.fetch_add(1, std::memory_order_relaxed);
            last_tx_audio_ms.store(util::NowUnixMs(), std::memory_order_relaxed);
          } catch (...) {
          }
        }
        if (!capture_stream) {
          std::this_thread::sleep_for(std::chrono::milliseconds(kFrameMs));
        }
      }
    });
  }

  void StartVideoLoop() {
    video_running = true;
    video_thread = std::thread([this]() {
      const auto frame_period = std::chrono::milliseconds(1000 / kVideoFps);
      bool need_keyframe = true;
      bool logged_convert_fail = false;
      while (video_running.load()) {
        const auto t0 = std::chrono::steady_clock::now();
        if (!camera || !camera_enabled.load(std::memory_order_relaxed)) {
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
          continue;
        }
        Uint64 timestamp_ns = 0;
        SDL_Surface* surface = SDL_AcquireCameraFrame(camera, &timestamp_ns);
        if (!surface) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
          continue;
        }

        VideoFrameI420 i420;
        VideoFrameRgba captured;
        if (ConvertCameraSurfaceToRgba(surface, captured)) {
          VideoFrameRgba oriented = std::move(captured);
          if (camera_rotate_cw == 90) {
            VideoFrameRgba rotated;
            if (RotateRgba90Cw(oriented, rotated)) {
              oriented = std::move(rotated);
            }
          } else if (camera_rotate_cw == 270) {
            VideoFrameRgba rotated;
            if (RotateRgba90Ccw(oriented, rotated)) {
              oriented = std::move(rotated);
            }
          } else if (camera_rotate_cw == 180) {
            VideoFrameRgba once;
            VideoFrameRgba twice;
            if (RotateRgba90Cw(oriented, once) && RotateRgba90Cw(once, twice)) {
              oriented = std::move(twice);
            }
          }

          VideoFrameRgba fitted;
          if (ScaleCenterCropRgba(oriented, encode_width, encode_height, fitted) &&
              RgbaToI420(fitted.rgba.data(), fitted.width, fitted.height, fitted.width * 4, true,
                         i420)) {
            VideoFrameRgba preview = fitted;
            PremultiplyRgbaInPlace(preview.rgba);
            {
              std::lock_guard lock(video_frame_mutex);
              local_video_frame.width = preview.width;
              local_video_frame.height = preview.height;
              local_video_frame.rgba = std::move(preview.rgba);
              local_video_frame.seq = ++local_video_seq;
            }
            if (video_codec && video_codec->HasEncoder()) {
              auto encoded = video_codec->Encode(i420, need_keyframe);
              if (encoded && !encoded->annex_b.empty()) {
                need_keyframe = false;
                std::shared_ptr<SfuSendFn> send_fn;
                {
                  std::lock_guard lock(mutex);
                  if (sfu_mode && sfu_send) {
                    send_fn = sfu_send;
                  }
                }
                if (send_fn) {
                  SfuPacket pkt;
                  pkt.channel_id = 1;
                  pkt.seq = sfu_video_seq.fetch_add(1) + 1;
                  pkt.mark = encoded->keyframe ? 1 : 0;
                  pkt.payload = encoded->annex_b;
                  try {
                    std::lock_guard send_lock(sfu_send_call_mu);
                    (*send_fn)(pkt);
                  } catch (...) {
                  }
                }
              }
            }
          }
        } else if (!logged_convert_fail) {
          logged_convert_fail = true;
          // Use SDL_Log — CallMediaEngine logger isn't safe from this worker without more wiring.
          SDL_Log("CallMediaEngine: camera frame convert failed (format=%s pitch=%d %dx%d): %s",
                  SDL_GetPixelFormatName(surface->format), surface->pitch, surface->w, surface->h,
                  SDL_GetError());
        }

        SDL_ReleaseCameraFrame(camera, surface);

        const auto elapsed = std::chrono::steady_clock::now() - t0;
        if (elapsed < frame_period) {
          std::this_thread::sleep_for(frame_period - elapsed);
        }
      }
    });
  }

  void OnRemoteOpusFrame(uint32_t stream_id, uint32_t seq, const std::byte* data, size_t size) {
    if (size == 0) {
      return;
    }
    RemoteAudioTrack* track = EnsureRemoteTrackLocked(stream_id);
    if (!track || !track->decoder) {
      return;
    }
    std::vector<int16_t> pcm(static_cast<size_t>(kFrameSamples));
    const int decoded = opus_decode(track->decoder, reinterpret_cast<const unsigned char*>(data),
                                    static_cast<int>(size), pcm.data(), kFrameSamples, 0);
    if (decoded <= 0) {
      return;
    }
    pcm.resize(static_cast<size_t>(decoded));
    const int64_t recv_ms = util::NowUnixMs();
    PlayoutPcmFrame frame;
    frame.seq = seq;
    frame.recv_ms = recv_ms;
    frame.pcm = std::move(pcm);
    track->jitter.Push(std::move(frame));
    rx_audio_frames.fetch_add(1, std::memory_order_relaxed);
    last_rx_audio_ms.store(recv_ms, std::memory_order_relaxed);
  }

  void OnRemoteH264Frame(const std::byte* data, size_t size) {
    if (!video_codec || size == 0) {
      return;
    }
    if (!video_codec->HasDecoder()) {
      if (auto configured = video_codec->ConfigureDecoder(); !configured) {
        static bool logged_cfg = false;
        if (!logged_cfg) {
          logged_cfg = true;
          SDL_Log("CallMediaEngine: ConfigureDecoder failed: %s", configured.error().message.c_str());
        }
        return;
      }
    }
    auto decoded = video_codec->Decode(reinterpret_cast<const uint8_t*>(data), size);
    if (!decoded) {
      static int logged_decode = 0;
      if (logged_decode < 5) {
        ++logged_decode;
        SDL_Log("CallMediaEngine: remote H264 decode failed: %s (size=%zu)",
                decoded.error().message.c_str(), size);
      }
      return;
    }
    PublishRemoteFrame(std::move(*decoded));
  }

  Roe<void> EnableCameraLocked() {
    if (camera_enabled.load(std::memory_order_relaxed) && camera) {
      return {};
    }
    if (auto ok = EnsureCameraSubsystem(); !ok) {
      return ok.error();
    }
    if (!video_codec) {
      video_codec = CreatePlatformVideoCodec();
    }

    int count = 0;
    SDL_CameraID* cameras = SDL_GetCameras(&count);
    if (!cameras || count <= 0) {
      if (cameras) {
        SDL_free(cameras);
      }
      return Error(std::string("No camera: ") + SDL_GetError());
    }

    SDL_CameraID chosen = cameras[0];
    for (int i = 0; i < count; ++i) {
      const SDL_CameraPosition pos = SDL_GetCameraPosition(cameras[i]);
      if (pos == SDL_CAMERA_POSITION_FRONT_FACING) {
        chosen = cameras[i];
        break;
      }
    }
    SDL_free(cameras);

    const CameraCaptureTransform xform = ResolveCameraCaptureTransform(chosen);
    encode_width = xform.encode_width;
    encode_height = xform.encode_height;
    camera_rotate_cw = xform.rotate_cw;

    std::string encode_warn;
    if (video_codec) {
      if (auto cfg = video_codec->ConfigureEncoder(encode_width, encode_height, kVideoFps); !cfg) {
        encode_warn = cfg.error().message;
      }
    }

    SDL_CameraSpec want{};
    // Prefer a convertible packed/YUV format. UNKNOWN picks the driver's first enum
    // entry (often MJPG/NV12 on Windows); conversion is handled in the capture loop.
    want.format = SDL_PIXELFORMAT_UNKNOWN;
    // Prefer landscape sensor buffers; rotate/crop into encode_* below.
    want.width = std::max(encode_width, encode_height);
    want.height = std::min(encode_width, encode_height);
    want.framerate_numerator = kVideoFps;
    want.framerate_denominator = 1;
    camera = SDL_OpenCamera(chosen, &want);
    if (!camera) {
      // Fall back: ask SDL to deliver RGBA so the driver converts when possible.
      want.format = SDL_PIXELFORMAT_RGBA32;
      camera = SDL_OpenCamera(chosen, &want);
    }
    if (!camera) {
      camera = SDL_OpenCamera(chosen, nullptr);
    }
    if (!camera) {
      encode_width = kDefaultVideoWidth;
      encode_height = kDefaultVideoHeight;
      camera_rotate_cw = 0;
      return Error(std::string("SDL_OpenCamera failed: ") + SDL_GetError());
    }
    camera_id = chosen;
    camera_enabled.store(true, std::memory_order_relaxed);
    if (!video_running.load()) {
      StartVideoLoop();
    }
    if (!encode_warn.empty()) {
      // Preview-only path; caller (CallMediaEngine) logs.
      last_camera_warn = encode_warn;
    } else {
      last_camera_warn.clear();
    }
    return {};
  }

  std::string last_camera_warn;
};

CallMediaEngine::CallMediaEngine() : impl_(std::make_unique<Impl>()) {
  redirectLogger("CallMediaEngine");
  impl_->video_codec = CreatePlatformVideoCodec();
}

CallMediaEngine::~CallMediaEngine() {
  Stop();
}

void CallMediaEngine::SetOnStateChanged(StateChangedFn callback) {
  std::lock_guard lock(impl_->mutex);
  impl_->on_state_changed = std::move(callback);
}

Roe<void> CallMediaEngine::StartSfu(const std::string& call_id, SfuSendFn send) {
  StateChangedFn state_cb;
  std::shared_ptr<SfuSendFn> abandoned_send;
  bool need_rebuild = false;
  bool send_replaced_only = false;
  auto next_send = std::make_shared<SfuSendFn>(std::move(send));
  {
    std::lock_guard lock(impl_->mutex);
    if (call_id.empty()) {
      return Error("call_id required");
    }
    if (!*next_send) {
      return Error("SFU send callback required");
    }
    if (impl_->active) {
      if (impl_->call_id == call_id && impl_->sfu_mode) {
        // SoftMigrate from libp2p→media_relay: swap callback; capture may still hold old shared_ptr.
        abandoned_send = std::move(impl_->sfu_send);
        impl_->sfu_send = std::move(next_send);
        send_replaced_only = true;
      } else {
        // Soft-migrate: clear send callback BEFORE destroying opus/SDL.
        impl_->active = false;
        abandoned_send = std::move(impl_->sfu_send);
        impl_->sfu_send = nullptr;
        impl_->capture_running = false;
        need_rebuild = true;
        impl_->call_id.clear();
      }
    }
  }
  if (send_replaced_only) {
    // Drain any capture/video thread still inside the old send before the caller Detach's that
    // stream — otherwise SoftMigrate races into heap corruption.
    std::lock_guard drain(impl_->sfu_send_call_mu);
    abandoned_send = nullptr;
    return {};
  }
  abandoned_send = nullptr;
  if (need_rebuild) {
    impl_->playout_running = false;
    impl_->JoinCaptureThread();
    impl_->JoinPlayoutThread();
    std::lock_guard lock(impl_->mutex);
    impl_->TearDownAudioLocked();
  }
  {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->video_codec) {
      impl_->video_codec = CreatePlatformVideoCodec();
    }
    if (auto codecs = impl_->EnsureOpusCodecs(); !codecs) {
      impl_->TearDownAudioLocked();
      return codecs.error();
    }
    impl_->sfu_mode = true;
    impl_->sfu_send = std::move(next_send);
    impl_->sfu_audio_seq.store(0);
    impl_->sfu_video_seq.store(0);
    impl_->call_id = call_id;
    impl_->active = true;
    impl_->muted.store(false, std::memory_order_relaxed);
    impl_->camera_enabled.store(false, std::memory_order_relaxed);
    impl_->adaptation_camera_allowed.store(true, std::memory_order_relaxed);
    impl_->outbound_drops.store(0, std::memory_order_relaxed);
    impl_->playout_ticks.store(0, std::memory_order_relaxed);
    impl_->path_pressure.store(0.0, std::memory_order_relaxed);
    impl_->ClearAudioTracksLocked();
    impl_->connected_at_ms.store(util::NowUnixMs(), std::memory_order_relaxed);
    impl_->started_at_ms.store(util::NowUnixMs(), std::memory_order_relaxed);
    impl_->last_remote_video_ms.store(0, std::memory_order_relaxed);
    impl_->ever_had_remote_video.store(false, std::memory_order_relaxed);
    impl_->ApplyStateLocked("connected");
    impl_->StartCaptureLoop();
    if (!impl_->playout_thread.joinable()) {
      impl_->StartPlayoutLoop();
    }
    state_cb = impl_->on_state_changed;
  }
  if (state_cb) {
    state_cb("connected");
  }
  return {};
}

void CallMediaEngine::OnSfuPacket(const SfuPacket& packet) {
  // Must hold mutex: SoftMigrate StartSfu TearDownAudioLocked destroys opus/SDL while the
  // media_relay client reader can already deliver frames (guest attach dogfood crash).
  std::lock_guard lock(impl_->mutex);
  if (!impl_->active || !impl_->sfu_mode || packet.payload.empty()) {
    return;
  }
  {
    static std::atomic<int> sfu_rx_log{0};
    const int n = sfu_rx_log.fetch_add(1, std::memory_order_relaxed);
    if (n < 8) {
      log().info << "OnSfuPacket #" << n << " stream=" << packet.stream_id << " ch=" << packet.channel_id
                 << " seq=" << packet.seq << " bytes=" << packet.payload.size()
                 << " call=" << impl_->call_id;
    }
  }
  if (packet.channel_id == 0) {
    impl_->OnRemoteOpusFrame(packet.stream_id, packet.seq,
                             reinterpret_cast<const std::byte*>(packet.payload.data()),
                             packet.payload.size());
  } else if (packet.channel_id == 1) {
    impl_->OnRemoteH264Frame(reinterpret_cast<const std::byte*>(packet.payload.data()), packet.payload.size());
  }
}

bool CallMediaEngine::IsSfuMode() const {
  return impl_->sfu_mode;
}

void CallMediaEngine::ApplyAdaptation(const CallAdaptationDecision& decision) {
  impl_->adaptation_camera_allowed.store(decision.camera_allowed, std::memory_order_relaxed);
  impl_->adaptation_target_video_bps = decision.target_video_lo_bps;
  const int64_t audio_bps =
      decision.target_audio_bps > 0 ? decision.target_audio_bps : CallMediaAdaptation::kComfortAudioBps;
  impl_->adaptation_target_audio_bps.store(audio_bps, std::memory_order_relaxed);
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->encoder) {
      opus_encoder_ctl(impl_->encoder, OPUS_SET_BITRATE(static_cast<int>(audio_bps)));
    }
    if (!decision.camera_allowed && impl_->camera_enabled.load(std::memory_order_relaxed)) {
      impl_->CloseCameraLocked();
    }
  }
}

double CallMediaEngine::PathPressure() const {
  return impl_->path_pressure.load(std::memory_order_relaxed);
}

void CallMediaEngine::NoteOutboundDrop() {
  impl_->outbound_drops.fetch_add(1, std::memory_order_relaxed);
}

CallMediaEngineHealth CallMediaEngine::HealthSnapshot() const {
  CallMediaEngineHealth h;
  h.active = impl_->active.load(std::memory_order_relaxed);
  h.connected = impl_->connected.load(std::memory_order_relaxed);
  h.sfu_mode = impl_->sfu_mode;
  h.muted = impl_->muted.load(std::memory_order_relaxed);
  h.path_pressure = impl_->path_pressure.load(std::memory_order_relaxed);
  h.opus_target_bps = impl_->adaptation_target_audio_bps.load(std::memory_order_relaxed);
  h.outbound_drops = impl_->outbound_drops.load(std::memory_order_relaxed);
  h.playout_underruns = impl_->playout_underruns_total.load(std::memory_order_relaxed);
  h.plc_frames = impl_->plc_frames_total.load(std::memory_order_relaxed);
  h.rx_audio_frames = impl_->rx_audio_frames.load(std::memory_order_relaxed);
  h.tx_audio_frames = impl_->tx_audio_frames.load(std::memory_order_relaxed);
  h.last_rx_audio_ms = impl_->last_rx_audio_ms.load(std::memory_order_relaxed);
  h.last_tx_audio_ms = impl_->last_tx_audio_ms.load(std::memory_order_relaxed);
  h.local_level = impl_->local_input_level.load(std::memory_order_relaxed);
  h.remote_level = impl_->remote_output_level.load(std::memory_order_relaxed);
  {
    std::lock_guard lock(impl_->mutex);
    h.stream_count = impl_->audio_tracks.size();
    h.sfu_mode = impl_->sfu_mode;
  }
  return h;
}

void CallMediaEngine::Stop() {
  StateChangedFn state_cb;
  std::shared_ptr<SfuSendFn> abandoned_send;
  {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->active && !impl_->sfu_mode) {
      return;
    }
    impl_->active = false;
    impl_->muted.store(false, std::memory_order_relaxed);
    impl_->camera_enabled.store(false, std::memory_order_relaxed);
    impl_->connected_at_ms.store(0, std::memory_order_relaxed);
    impl_->started_at_ms.store(0, std::memory_order_relaxed);
    abandoned_send = std::move(impl_->sfu_send);
    impl_->sfu_send = nullptr;
    impl_->sfu_mode = false;
    impl_->capture_running = false;
    impl_->playout_running = false;
  }
  abandoned_send = nullptr;
  // Drain capture/video still inside (*sfu_send) after Detach unblocked BlockingWrite.
  {
    std::lock_guard drain(impl_->sfu_send_call_mu);
  }
  impl_->JoinCaptureThread();
  impl_->JoinPlayoutThread();
  {
    std::lock_guard lock(impl_->mutex);
    impl_->TearDownAudioLocked();
    impl_->call_id.clear();
    impl_->ApplyStateLocked("closed");
    state_cb = impl_->on_state_changed;
  }
  if (state_cb) {
    state_cb("closed");
  }
}

void CallMediaEngine::SetMuted(bool muted) {
  impl_->muted.store(muted, std::memory_order_relaxed);
  if (muted) {
    impl_->local_input_level.store(0.f, std::memory_order_relaxed);
  }
}

bool CallMediaEngine::IsMuted() const {
  return impl_->muted.load(std::memory_order_relaxed);
}

Roe<void> CallMediaEngine::SetCameraEnabled(bool enabled) {
  std::lock_guard lock(impl_->mutex);
  if (!impl_->active) {
    return Error("Call media not active");
  }
  if (!enabled) {
    impl_->CloseCameraLocked();
    return {};
  }
  if (!impl_->adaptation_camera_allowed.load(std::memory_order_relaxed)) {
    return Error("Camera blocked by adaptation (uplink/path)");
  }
  auto opened = impl_->EnableCameraLocked();
  if (!opened) {
    return opened.error();
  }
  if (!impl_->last_camera_warn.empty()) {
    log().warning << "Video encode unavailable: " << impl_->last_camera_warn
                  << " — local preview may still work; voice continues";
  }
  return {};
}

bool CallMediaEngine::IsCameraEnabled() const {
  return impl_->camera_enabled.load(std::memory_order_relaxed);
}

bool CallMediaEngine::HasRemoteVideo() const {
  return impl_->has_remote_video.load(std::memory_order_relaxed);
}

bool CallMediaEngine::IsRemoteVideoStalling() const {
  if (!impl_->has_remote_video.load(std::memory_order_relaxed)) {
    return false;
  }
  const int64_t last = impl_->last_remote_video_ms.load(std::memory_order_relaxed);
  if (last <= 0) {
    return false;
  }
  const int64_t age = util::NowUnixMs() - last;
  return age >= kRemoteVideoStallSoftMs && age < kRemoteVideoStallHardMs;
}

bool CallMediaEngine::EverHadRemoteVideo() const {
  return impl_->ever_had_remote_video.load(std::memory_order_relaxed);
}

void CallMediaEngine::ClearRemoteVideo() {
  impl_->ClearRemoteVideoFrames();
}

void CallMediaEngine::RefreshRemoteVideoHealth() {
  if (!impl_->active.load()) {
    return;
  }
  const int64_t now = util::NowUnixMs();
  std::string state;
  {
    std::lock_guard lock(impl_->mutex);
    state = impl_->connection_state;
  }
  if (state == "failed" || state == "closed") {
    impl_->ClearRemoteVideoFrames();
    return;
  }
  if (!impl_->has_remote_video.load(std::memory_order_relaxed)) {
    return;
  }
  const int64_t last = impl_->last_remote_video_ms.load(std::memory_order_relaxed);
  if (last > 0 && (now - last) >= kRemoteVideoStallHardMs) {
    impl_->ClearRemoteVideoFrames();
  }
}

bool CallMediaEngine::VideoEncoderAvailable() const {
  return impl_->video_codec && impl_->video_codec->HasEncoder();
}

bool CallMediaEngine::IsActive() const {
  return impl_->active.load();
}

bool CallMediaEngine::IsConnected() const {
  return impl_->connected.load();
}

void CallMediaEngine::SetConnectionState(const std::string& state) {
  impl_->SetState(state);
}

std::string CallMediaEngine::ActiveCallId() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->call_id;
}

std::string CallMediaEngine::ConnectionState() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->connection_state;
}

int64_t CallMediaEngine::ConnectedAtMs() const {
  return impl_->connected_at_ms.load(std::memory_order_relaxed);
}

int64_t CallMediaEngine::StartedAtMs() const {
  return impl_->started_at_ms.load(std::memory_order_relaxed);
}

bool CallMediaEngine::HasLocalCapture() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->capture_available;
}

float CallMediaEngine::LocalInputLevel() const {
  return impl_->local_input_level.load(std::memory_order_relaxed);
}

float CallMediaEngine::RemoteOutputLevel() const {
  return impl_->remote_output_level.load(std::memory_order_relaxed);
}

bool CallMediaEngine::CopyLocalVideoFrame(VideoTileFrame& out) const {
  std::lock_guard lock(impl_->video_frame_mutex);
  if (impl_->local_video_frame.rgba.empty()) {
    return false;
  }
  out = impl_->local_video_frame;
  return true;
}

bool CallMediaEngine::CopyRemoteVideoFrame(VideoTileFrame& out) const {
  std::lock_guard lock(impl_->video_frame_mutex);
  if (impl_->remote_video_frame.rgba.empty()) {
    return false;
  }
  out = impl_->remote_video_frame;
  return true;
}

} // namespace pbr
