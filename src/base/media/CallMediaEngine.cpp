#include "base/media/CallMediaEngine.h"

#include "base/media/CallAudioSession.h"
#include "base/media/CameraCaptureOrientation.h"
#include "base/media/IVideoCodec.h"
#include "base/media/VideoYuv.h"
#include "common/Utilities.h"

#include <SDL3/SDL.h>
#include <opus.h>
#include <rtc/rtc.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <thread>
#include <vector>

namespace pbr {
namespace {

constexpr int kSampleRate = 48000;
constexpr int kChannels = 1;
constexpr int kFrameMs = 20;
constexpr int kFrameSamples = kSampleRate * kFrameMs / 1000;
constexpr int kOpusPayloadType = 111;
constexpr int kH264PayloadType = 96;
constexpr int kDefaultVideoWidth = 640;
constexpr int kDefaultVideoHeight = 360;
constexpr int kVideoFps = 20;
/** Soft stall: keep last frame; UI may show Reconnecting… */
constexpr int64_t kRemoteVideoStallSoftMs = 2000;
/** Hard stall: drop last frame so the tile does not freeze forever. */
constexpr int64_t kRemoteVideoStallHardMs = 5000;
/** ICE disconnected grace before treating remote video as dead. */
constexpr int64_t kIceDisconnectedVideoClearMs = 3000;

constexpr rtc::SSRC kOffererAudioSsrc = 1;
constexpr rtc::SSRC kAnswererAudioSsrc = 2;
constexpr rtc::SSRC kOffererVideoSsrc = 3;
constexpr rtc::SSRC kAnswererVideoSsrc = 4;

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

std::string StateToString(rtc::PeerConnection::State state) {
  switch (state) {
  case rtc::PeerConnection::State::New:
    return "new";
  case rtc::PeerConnection::State::Connecting:
    return "connecting";
  case rtc::PeerConnection::State::Connected:
    return "connected";
  case rtc::PeerConnection::State::Disconnected:
    return "disconnected";
  case rtc::PeerConnection::State::Failed:
    return "failed";
  case rtc::PeerConnection::State::Closed:
    return "closed";
  }
  return "unknown";
}

} // namespace

struct CallMediaEngine::Impl {
  std::mutex mutex;
  std::string call_id;
  Role role = Role::Offerer;
  std::atomic<bool> active{false};
  std::atomic<bool> connected{false};
  std::atomic<bool> muted{false};
  std::atomic<bool> camera_enabled{false};
  std::atomic<bool> has_remote_video{false};
  std::atomic<bool> ever_had_remote_video{false};
  std::atomic<int64_t> connected_at_ms{0};
  std::atomic<int64_t> last_remote_video_ms{0};
  std::atomic<int64_t> disconnected_since_ms{0};
  std::string connection_state = "idle";

  LocalDescriptionFn on_local_description;
  IceCandidateFn on_ice_candidate;
  StateChangedFn on_state_changed;

  bool sfu_mode = false;
  SfuSendFn sfu_send;
  std::atomic<uint32_t> sfu_audio_seq{0};
  std::atomic<uint32_t> sfu_video_seq{0};
  std::atomic<bool> adaptation_camera_allowed{true};
  int64_t adaptation_target_video_bps = 0;

  std::shared_ptr<rtc::PeerConnection> pc;
  std::shared_ptr<rtc::Track> audio_track;
  std::shared_ptr<rtc::Track> video_track;
  std::shared_ptr<rtc::RtpPacketizationConfig> audio_rtp_config;
  std::shared_ptr<rtc::RtpPacketizationConfig> video_rtp_config;
  rtc::SSRC local_audio_ssrc = kOffererAudioSsrc;
  rtc::SSRC local_video_ssrc = kOffererVideoSsrc;
  bool capture_available = false;

  OpusEncoder* encoder = nullptr;
  OpusDecoder* decoder = nullptr;
  SDL_AudioStream* capture_stream = nullptr;
  SDL_AudioStream* playback_stream = nullptr;
  SDL_AudioDeviceID capture_device = 0;
  SDL_AudioDeviceID playback_device = 0;

  std::unique_ptr<IVideoCodec> video_codec;
  SDL_Camera* camera = nullptr;
  SDL_CameraID camera_id = 0;
  /** Clockwise degrees to apply to sensor buffers before encode. */
  int camera_rotate_cw = 0;
  int encode_width = kDefaultVideoWidth;
  int encode_height = kDefaultVideoHeight;

  std::thread capture_thread;
  std::thread video_thread;
  std::atomic<bool> capture_running{false};
  std::atomic<bool> video_running{false};
  std::atomic<float> local_input_level{0.f};
  std::atomic<float> remote_output_level{0.f};
  std::atomic<int64_t> remote_level_ms{0};

  std::mutex playback_mutex;
  std::deque<std::vector<int16_t>> playback_queue;

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

  void SetState(const std::string& state) {
    connection_state = state;
    const bool now_connected = (state == "connected");
    if (now_connected && !connected.load()) {
      connected_at_ms.store(util::NowUnixMs(), std::memory_order_relaxed);
    }
    if (!now_connected) {
      connected_at_ms.store(0, std::memory_order_relaxed);
    }
    connected = now_connected;
    if (state == "disconnected") {
      if (disconnected_since_ms.load(std::memory_order_relaxed) == 0) {
        disconnected_since_ms.store(util::NowUnixMs(), std::memory_order_relaxed);
      }
    } else if (state == "connected" || state == "connecting" || state == "new") {
      disconnected_since_ms.store(0, std::memory_order_relaxed);
    }
    if (state == "failed" || state == "closed") {
      ClearRemoteVideoFrames();
      disconnected_since_ms.store(0, std::memory_order_relaxed);
    }
    if (on_state_changed) {
      on_state_changed(state);
    }
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

  void TearDownAudioLocked() {
    capture_running = false;
    if (capture_thread.joinable()) {
      capture_thread.join();
    }
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
    if (decoder) {
      opus_decoder_destroy(decoder);
      decoder = nullptr;
    }
    if (video_codec) {
      video_codec->ResetEncoder();
      video_codec->ResetDecoder();
    }
    {
      std::lock_guard lock(playback_mutex);
      playback_queue.clear();
    }
    ClearVideoFrames();
    local_input_level.store(0.f, std::memory_order_relaxed);
    remote_output_level.store(0.f, std::memory_order_relaxed);
    remote_level_ms.store(0, std::memory_order_relaxed);
    capture_available = false;
    CallAudioSession::Deactivate();
  }

  void TearDownPcLocked() {
    if (audio_track) {
      audio_track->onFrame(nullptr);
      audio_track.reset();
    }
    if (video_track) {
      video_track->onFrame(nullptr);
      video_track.reset();
    }
    audio_rtp_config.reset();
    video_rtp_config.reset();
    if (pc) {
      pc->close();
      pc.reset();
    }
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
    if (encoder && decoder) {
      return {};
    }
    int err = 0;
    if (!encoder) {
      encoder = opus_encoder_create(kSampleRate, kChannels, OPUS_APPLICATION_VOIP, &err);
      if (!encoder || err != OPUS_OK) {
        return Error("opus_encoder_create failed");
      }
      opus_encoder_ctl(encoder, OPUS_SET_BITRATE(24000));
    }
    if (!decoder) {
      decoder = opus_decoder_create(kSampleRate, kChannels, &err);
      if (!decoder || err != OPUS_OK) {
        return Error("opus_decoder_create failed");
      }
    }
    return {};
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

  void BindAudioTrack(const std::shared_ptr<rtc::Track>& track, rtc::SSRC ssrc) {
    local_audio_ssrc = ssrc;
    audio_rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(
        ssrc, "audio", kOpusPayloadType, rtc::OpusRtpPacketizer::DefaultClockRate);
    auto packetizer = std::make_shared<rtc::OpusRtpPacketizer>(audio_rtp_config);
    auto depacketizer = std::make_shared<rtc::OpusRtpDepacketizer>();
    auto rtcp_recv = std::make_shared<rtc::RtcpReceivingSession>();
    auto sr_reporter = std::make_shared<rtc::RtcpSrReporter>(audio_rtp_config);
    packetizer->addToChain(depacketizer);
    packetizer->addToChain(rtcp_recv);
    packetizer->addToChain(sr_reporter);
    track->setMediaHandler(packetizer);
    track->onFrame([this](rtc::binary data, rtc::FrameInfo) {
      if (!data.empty()) {
        OnRemoteOpusFrame(data.data(), data.size());
      }
    });
    audio_track = track;
  }

  void BindVideoTrack(const std::shared_ptr<rtc::Track>& track, rtc::SSRC ssrc) {
    local_video_ssrc = ssrc;
    video_rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(
        ssrc, "video", kH264PayloadType, rtc::H264RtpPacketizer::defaultClockRate);
    auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence,
                                                              video_rtp_config);
    auto depacketizer = std::make_shared<rtc::H264RtpDepacketizer>(rtc::NalUnit::Separator::StartSequence);
    auto rtcp_recv = std::make_shared<rtc::RtcpReceivingSession>();
    auto sr_reporter = std::make_shared<rtc::RtcpSrReporter>(video_rtp_config);
    packetizer->addToChain(depacketizer);
    packetizer->addToChain(rtcp_recv);
    packetizer->addToChain(sr_reporter);
    track->setMediaHandler(packetizer);
    track->onFrame([this](rtc::binary data, rtc::FrameInfo) {
      if (!data.empty()) {
        OnRemoteH264Frame(data.data(), data.size());
      }
    });
    video_track = track;
  }

  void AddAudioAndVideoTracks(Role start_role) {
    local_audio_ssrc = (start_role == Role::Offerer) ? kOffererAudioSsrc : kAnswererAudioSsrc;
    local_video_ssrc = (start_role == Role::Offerer) ? kOffererVideoSsrc : kAnswererVideoSsrc;

    rtc::Description::Audio audio("audio", rtc::Description::Direction::SendRecv);
    audio.addOpusCodec(kOpusPayloadType);
    audio.addSSRC(local_audio_ssrc, "audio");
    BindAudioTrack(pc->addTrack(audio), local_audio_ssrc);

    rtc::Description::Video video("video", rtc::Description::Direction::SendRecv);
    video.addH264Codec(kH264PayloadType);
    video.addSSRC(local_video_ssrc, "video");
    BindVideoTrack(pc->addTrack(video), local_video_ssrc);
  }

  void StartCaptureLoop() {
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
        const bool can_send_p2p = audio_track && audio_track->isOpen() && encoder;
        const bool can_send_sfu = sfu_mode && static_cast<bool>(sfu_send) && encoder;
        const bool can_send = can_send_p2p || can_send_sfu;
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
        if (!can_send) {
          continue;
        }
        const int encoded =
            opus_encode(encoder, pcm.data(), kFrameSamples, opus_buf.data(), static_cast<int>(opus_buf.size()));
        if (encoded <= 0) {
          continue;
        }
        if (can_send_sfu) {
          SfuPacket pkt;
          pkt.channel_id = 0;
          pkt.seq = sfu_audio_seq.fetch_add(1) + 1;
          pkt.payload.assign(opus_buf.data(), opus_buf.data() + encoded);
          try {
            sfu_send(pkt);
          } catch (...) {
          }
        }
        if (can_send_p2p) {
          try {
            audio_track->send(reinterpret_cast<const std::byte*>(opus_buf.data()), static_cast<size_t>(encoded));
            if (audio_rtp_config) {
              audio_rtp_config->timestamp += static_cast<uint32_t>(kFrameSamples);
            }
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
              if (encoded) {
                need_keyframe = false;
                if (sfu_mode && sfu_send) {
                  SfuPacket pkt;
                  pkt.channel_id = 1;
                  pkt.seq = sfu_video_seq.fetch_add(1) + 1;
                  pkt.mark = encoded->keyframe ? 1 : 0;
                  pkt.payload = encoded->annex_b;
                  try {
                    sfu_send(pkt);
                  } catch (...) {
                  }
                }
                if (video_track && video_track->isOpen()) {
                  try {
                    video_track->send(reinterpret_cast<const std::byte*>(encoded->annex_b.data()),
                                      encoded->annex_b.size());
                    if (video_rtp_config) {
                      video_rtp_config->timestamp += static_cast<uint32_t>(90000 / kVideoFps);
                    }
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

  void OnRemoteOpusFrame(const std::byte* data, size_t size) {
    if (!decoder || size == 0) {
      return;
    }
    std::vector<int16_t> pcm(static_cast<size_t>(kFrameSamples));
    const int decoded = opus_decode(decoder, reinterpret_cast<const unsigned char*>(data),
                                    static_cast<int>(size), pcm.data(), kFrameSamples, 0);
    if (decoded <= 0) {
      return;
    }
    SmoothLevel(remote_output_level, FramePeakLevel(pcm.data(), decoded));
    remote_level_ms.store(util::NowUnixMs(), std::memory_order_relaxed);
    if (!playback_stream) {
      return;
    }
    (void)SDL_PutAudioStreamData(playback_stream, pcm.data(), decoded * static_cast<int>(sizeof(int16_t)));
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

  Roe<void> SetupPeerConnection(Role start_role) {
    rtc::Configuration config;
    config.enableIceTcp = false;
    config.disableAutoNegotiation = true;
    config.forceMediaTransport = true;
    pc = std::make_shared<rtc::PeerConnection>(config);
    role = start_role;

    pc->onStateChange([this](rtc::PeerConnection::State state) { SetState(StateToString(state)); });

    pc->onLocalCandidate([this](rtc::Candidate candidate) {
      if (!on_ice_candidate) {
        return;
      }
      IceCandidate ice;
      ice.candidate = std::string(candidate);
      ice.mid = candidate.mid();
      if (ice.mid.empty()) {
        ice.mid = "audio";
      }
      on_ice_candidate(ice);
    });

    pc->onLocalDescription([this](rtc::Description description) {
      if (!on_local_description) {
        return;
      }
      LocalDescription local;
      local.type = description.typeString();
      local.sdp = std::string(description);
      on_local_description(local);
    });

    pc->onTrack([this](std::shared_ptr<rtc::Track> remote_track) {
      if (!remote_track) {
        return;
      }
      auto desc = remote_track->description();
      const std::string mid = desc.mid();
      if (mid == "video" || desc.type() == "video") {
        desc.addSSRC(local_video_ssrc, "video");
        remote_track->setDescription(desc);
        BindVideoTrack(remote_track, local_video_ssrc);
      } else {
        desc.addSSRC(local_audio_ssrc, "audio");
        remote_track->setDescription(desc);
        BindAudioTrack(remote_track, local_audio_ssrc);
      }
    });

    AddAudioAndVideoTracks(start_role);
    if (start_role == Role::Offerer) {
      pc->setLocalDescription();
    }
    return {};
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

void CallMediaEngine::SetOnLocalDescription(LocalDescriptionFn callback) {
  std::lock_guard lock(impl_->mutex);
  impl_->on_local_description = std::move(callback);
}

void CallMediaEngine::SetOnIceCandidate(IceCandidateFn callback) {
  std::lock_guard lock(impl_->mutex);
  impl_->on_ice_candidate = std::move(callback);
}

void CallMediaEngine::SetOnStateChanged(StateChangedFn callback) {
  std::lock_guard lock(impl_->mutex);
  impl_->on_state_changed = std::move(callback);
}

Roe<void> CallMediaEngine::Start(const std::string& call_id, const Role role) {
  std::lock_guard lock(impl_->mutex);
  if (call_id.empty()) {
    return Error("call_id required");
  }
  if (impl_->active) {
    if (impl_->call_id == call_id) {
      return {};
    }
    return Error("Call media engine already active");
  }
  if (!impl_->video_codec) {
    impl_->video_codec = CreatePlatformVideoCodec();
  }
  // Codecs + PeerConnection first so SDP/ICE can flow while mic permission
  // blocks OpenAudioDevices on the capture worker (macOS TCC).
  if (auto codecs = impl_->EnsureOpusCodecs(); !codecs) {
    impl_->TearDownAudioLocked();
    return codecs.error();
  }
  if (auto pc = impl_->SetupPeerConnection(role); !pc) {
    impl_->TearDownAudioLocked();
    return pc.error();
  }
  impl_->sfu_mode = false;
  impl_->sfu_send = nullptr;
  impl_->call_id = call_id;
  impl_->active = true;
  impl_->muted.store(false, std::memory_order_relaxed);
  impl_->camera_enabled.store(false, std::memory_order_relaxed);
  impl_->adaptation_camera_allowed.store(true, std::memory_order_relaxed);
  impl_->connected_at_ms.store(0, std::memory_order_relaxed);
  impl_->last_remote_video_ms.store(0, std::memory_order_relaxed);
  impl_->disconnected_since_ms.store(0, std::memory_order_relaxed);
  impl_->ever_had_remote_video.store(false, std::memory_order_relaxed);
  impl_->SetState("connecting");
  impl_->StartCaptureLoop();
  return {};
}

Roe<void> CallMediaEngine::StartSfu(const std::string& call_id, SfuSendFn send) {
  std::lock_guard lock(impl_->mutex);
  if (call_id.empty()) {
    return Error("call_id required");
  }
  if (!send) {
    return Error("SFU send callback required");
  }
  if (impl_->active) {
    if (impl_->call_id == call_id && impl_->sfu_mode) {
      impl_->sfu_send = std::move(send);
      return {};
    }
    // Soft-migrate: tear down P2P then bring up SFU for same or new call_id.
    impl_->active = false;
    impl_->TearDownAudioLocked();
    impl_->TearDownPcLocked();
    impl_->call_id.clear();
  }
  if (!impl_->video_codec) {
    impl_->video_codec = CreatePlatformVideoCodec();
  }
  if (auto codecs = impl_->EnsureOpusCodecs(); !codecs) {
    impl_->TearDownAudioLocked();
    return codecs.error();
  }
  impl_->sfu_mode = true;
  impl_->sfu_send = std::move(send);
  impl_->sfu_audio_seq.store(0);
  impl_->sfu_video_seq.store(0);
  impl_->call_id = call_id;
  impl_->active = true;
  impl_->muted.store(false, std::memory_order_relaxed);
  impl_->camera_enabled.store(false, std::memory_order_relaxed);
  impl_->adaptation_camera_allowed.store(true, std::memory_order_relaxed);
  impl_->connected_at_ms.store(util::NowUnixMs(), std::memory_order_relaxed);
  impl_->last_remote_video_ms.store(0, std::memory_order_relaxed);
  impl_->disconnected_since_ms.store(0, std::memory_order_relaxed);
  impl_->ever_had_remote_video.store(false, std::memory_order_relaxed);
  impl_->SetState("connected");
  impl_->StartCaptureLoop();
  return {};
}

void CallMediaEngine::OnSfuPacket(const SfuPacket& packet) {
  if (!impl_->active || !impl_->sfu_mode || packet.payload.empty()) {
    return;
  }
  if (packet.channel_id == 0) {
    impl_->OnRemoteOpusFrame(reinterpret_cast<const std::byte*>(packet.payload.data()), packet.payload.size());
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
  if (!decision.camera_allowed && impl_->camera_enabled.load(std::memory_order_relaxed)) {
    std::lock_guard lock(impl_->mutex);
    impl_->CloseCameraLocked();
  }
}

Roe<void> CallMediaEngine::SetRemoteDescription(const std::string& type, const std::string& sdp) {
  std::lock_guard lock(impl_->mutex);
  if (!impl_->pc) {
    return Error("Media peer connection not started");
  }
  try {
    rtc::Description remote(sdp, type);
    impl_->pc->setRemoteDescription(remote);
    if (impl_->role == Role::Answerer && type == "offer") {
      impl_->pc->setLocalDescription(rtc::Description::Type::Answer);
    }
  } catch (const std::exception& ex) {
    return Error(std::string("setRemoteDescription failed: ") + ex.what());
  }
  return {};
}

Roe<void> CallMediaEngine::AddRemoteIceCandidate(const std::string& candidate, const std::string& mid) {
  std::lock_guard lock(impl_->mutex);
  if (!impl_->pc) {
    return Error("Media peer connection not started");
  }
  try {
    rtc::Candidate ice(candidate, mid.empty() ? "audio" : mid);
    impl_->pc->addRemoteCandidate(ice);
  } catch (const std::exception& ex) {
    return Error(std::string("addRemoteCandidate failed: ") + ex.what());
  }
  return {};
}

void CallMediaEngine::Stop() {
  std::lock_guard lock(impl_->mutex);
  if (!impl_->active && !impl_->pc && !impl_->sfu_mode) {
    return;
  }
  impl_->active = false;
  impl_->muted.store(false, std::memory_order_relaxed);
  impl_->camera_enabled.store(false, std::memory_order_relaxed);
  impl_->connected_at_ms.store(0, std::memory_order_relaxed);
  impl_->TearDownAudioLocked();
  impl_->TearDownPcLocked();
  impl_->sfu_mode = false;
  impl_->sfu_send = nullptr;
  impl_->call_id.clear();
  impl_->SetState("closed");
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
  if (state == "disconnected") {
    const int64_t since = impl_->disconnected_since_ms.load(std::memory_order_relaxed);
    if (since > 0 && (now - since) >= kIceDisconnectedVideoClearMs) {
      impl_->ClearRemoteVideoFrames();
    }
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
