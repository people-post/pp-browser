#include "domain/media/CallMediaEngine.h"

#include "domain/media/CallAudioSession.h"
#include "domain/media/CallMediaPlayout.h"
#include "domain/media/CallRingtone.h"
#include "domain/media/CameraCaptureOrientation.h"
#include "domain/media/IVideoCodec.h"
#include "domain/media/MediaDeviceArbiter.h"
#include "domain/media/NoiseSuppressor.h"
#include "domain/media/VideoYuv.h"
#include "common/Utilities.h"

#include <SDL3/SDL.h>
#include <opus.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

constexpr int kSampleRate = 48000;
constexpr int kChannels = 1;
constexpr int kFrameMs = 20;
constexpr int kFrameSamples = kSampleRate * kFrameMs / 1000;
/** Bytes of one 20 ms mono s16 frame on the speaker. */
constexpr int kFrameBytes = kFrameSamples * static_cast<int>(sizeof(int16_t));
/** Keep ~60 ms queued in the device (spec §1); above ~120 ms skip a slot to shed latency. */
constexpr int kPlayoutTargetQueuedBytes = 3 * kFrameBytes;
constexpr int kPlayoutHighWaterBytes = 6 * kFrameBytes;
/** Never produce more than this many slots per 20 ms wake-up (startup / after a stall). */
constexpr int kPlayoutMaxSlotsPerTick = 3;
constexpr int kVideoFps = 20;
/** Soft stall: keep last frame; UI may show Reconnecting… */
constexpr int64_t kRemoteVideoStallSoftMs = 2000;
/** Hard stall: drop last frame so the tile does not freeze forever. */
constexpr int64_t kRemoteVideoStallHardMs = 5000;

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
  std::atomic<int64_t> last_uplink_bps{0};
  std::mutex video_refresh_mu;
  std::unordered_set<uint32_t> pending_video_refresh_streams;
  std::unordered_map<uint32_t, int64_t> last_video_refresh_note_ms;
  std::string connection_state = "idle";

  StateChangedFn on_state_changed;

  bool sfu_mode = false;
  /** Test fixtures: skip SDL mic/camera open (silence TX only). */
  std::atomic<bool> skip_device_open_for_test{false};
  /** Bumped on StartSfu so async StopMeshMedia can detect a newer session. */
  std::atomic<uint64_t> session_generation{0};
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
  std::atomic<bool> video_need_keyframe{false};
  std::atomic<bool> adaptation_camera_allowed{true};
  std::atomic<int64_t> adaptation_target_video_bps{0};
  std::atomic<int64_t> adaptation_target_audio_bps{CallMediaAdaptation::kComfortAudioBps};
  std::atomic<double> path_pressure{0.0};
  std::atomic<uint64_t> outbound_drops{0};
  std::atomic<uint64_t> playout_ticks{0};
  std::atomic<uint64_t> rx_audio_frames{0};
  std::atomic<uint64_t> tx_audio_frames{0};
  std::atomic<int64_t> last_rx_audio_ms{0};
  std::atomic<int64_t> last_tx_audio_ms{0};
  std::atomic<uint64_t> rx_video_frames{0};
  std::atomic<uint64_t> tx_video_frames{0};
  std::atomic<int64_t> last_rx_video_ms{0};
  std::atomic<uint64_t> playout_underruns_total{0};
  std::atomic<uint64_t> plc_frames_total{0};
  std::atomic<uint64_t> fec_frames_total{0};

  bool capture_available = false;
#ifdef PP_BROWSER_CALL_DENOISE
  NoiseSuppressor denoise;
  int64_t last_denoise_log_ms = 0;
#endif

  OpusEncoder* encoder = nullptr;
  /** Where audio devices come from: `own_devices` (constructor) or `test_devices` (skip-open). */
  MediaDeviceArbiter* devices = nullptr;
  MediaDeviceArbiter* own_devices = nullptr;
  std::unique_ptr<MediaDeviceArbiter> test_devices;
  /**
   * Held while the session runs. Acquired / reopened by the capture thread (the only writer while it
   * runs, under `mutex`); the playout thread writes the speaker under `mutex`.
   */
  std::unique_ptr<AudioDeviceLease> mic_lease;
  std::unique_ptr<AudioDeviceLease> speaker_lease;
  /** Set for the rest of this call once voice-processing capture has starved 3 times in a row (I3):
   *  the next opens skip the voice duplex until the next StartCaptureLoop(). */
  std::atomic<bool> vpio_disabled_for_call{false};
  /** Voice-processing playout underruns of pairs already released this call; under `mutex`. */
  uint64_t vpio_underruns_closed = 0;

  struct RemoteAudioTrack {
    OpusDecoder* decoder = nullptr;
    AudioJitterBuffer jitter;
    uint64_t rx_frames = 0;
    int64_t last_rx_ms = 0;
    float peak_level = 0.f;
    ~RemoteAudioTrack() {
      if (decoder) {
        opus_decoder_destroy(decoder);
        decoder = nullptr;
      }
    }
  };
  /** Per publisher stream_id (V032). Guarded by mutex (decode + playout mix). */
  std::unordered_map<uint32_t, std::unique_ptr<RemoteAudioTrack>> audio_tracks;
  /** First OnSfuPacket log per stream; cleared on SoftMigrate send-swap / StartSfu. */
  std::mutex sfu_rx_log_mu;
  std::unordered_set<uint32_t> sfu_rx_logged_streams;

  /** Local encoder: created, configured, fed and bitrate-adjusted by the video thread only
   *  (reset by TearDown after that thread joined). */
  std::unique_ptr<IVideoCodec> video_codec;
  /** Local encoder + remote decoders come from here (platform HW; tests inject a stub). */
  std::function<std::unique_ptr<IVideoCodec>()> make_video_codec = CreatePlatformVideoCodec;
  std::unordered_map<uint32_t, std::unique_ptr<IVideoCodec>> remote_decoders;
  static constexpr size_t kMaxRemoteVideoDecoders = 4;
  /** Display rotation read on the SetCameraEnabled caller's (UI) thread for the next camera open. */
  std::atomic<int> camera_display_rotation{0};
  /** Why the last requested camera did not open; taken by the UI (TakeCameraFailure). */
  std::mutex camera_failure_mu;
  std::string camera_failure;

  std::thread capture_thread;
  std::thread video_thread;
  std::thread playout_thread;
  /** Halves of the running session; written under `mutex` in Start, read by the device thread. */
  SessionSpec spec = SessionSpec::Duplex();
  std::atomic<bool> capture_running{false};
  std::atomic<bool> video_running{false};
  std::atomic<bool> playout_running{false};
  /** Capture worker closes+reopens SDL devices (speaker route / SoftMigrate). */
  std::atomic<bool> audio_reopen_requested{false};
  std::atomic<float> local_input_level{0.f};
  std::atomic<float> remote_output_level{0.f};
  std::atomic<int64_t> remote_level_ms{0};

  mutable std::mutex video_frame_mutex;
  VideoTileFrame local_video_frame;
  VideoTileFrame remote_video_frame;
  std::unordered_map<uint32_t, VideoTileFrame> remote_video_by_stream;
  std::unordered_map<uint32_t, int64_t> last_video_ms_by_stream;
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

  static void SmoothLevel(float& level, float instant) {
    const float next = instant >= level ? instant : (level * 0.82f + instant * 0.18f);
    level = std::clamp(next, 0.f, 1.f);
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

  void NoteVideoRefreshNeeded(uint32_t stream_id) {
    const int64_t now = util::NowUnixMs();
    std::lock_guard lock(video_refresh_mu);
    auto it = last_video_refresh_note_ms.find(stream_id);
    if (it != last_video_refresh_note_ms.end() && now - it->second < 2000) {
      return;
    }
    last_video_refresh_note_ms[stream_id] = now;
    pending_video_refresh_streams.insert(stream_id);
  }

  void PublishRemoteFrame(uint32_t stream_id, VideoFrameRgba&& frame) {
    PremultiplyRgbaInPlace(frame.rgba);
    const int64_t now = util::NowUnixMs();
    std::lock_guard lock(video_frame_mutex);
    remote_video_frame.width = frame.width;
    remote_video_frame.height = frame.height;
    remote_video_frame.rgba = frame.rgba;
    remote_video_frame.seq = ++remote_video_seq;
    VideoTileFrame tile;
    tile.width = frame.width;
    tile.height = frame.height;
    tile.rgba = std::move(frame.rgba);
    tile.seq = remote_video_frame.seq;
    remote_video_by_stream[stream_id] = std::move(tile);
    last_video_ms_by_stream[stream_id] = now;
    has_remote_video.store(true, std::memory_order_relaxed);
    ever_had_remote_video.store(true, std::memory_order_relaxed);
    last_remote_video_ms.store(now, std::memory_order_relaxed);
    last_rx_video_ms.store(now, std::memory_order_relaxed);
    rx_video_frames.fetch_add(1, std::memory_order_relaxed);
  }

  void ClearRemoteVideoFrames() {
    std::lock_guard lock(video_frame_mutex);
    remote_video_frame = {};
    remote_video_by_stream.clear();
    last_video_ms_by_stream.clear();
    remote_video_seq = 0;
    has_remote_video.store(false, std::memory_order_relaxed);
    last_remote_video_ms.store(0, std::memory_order_relaxed);
  }

  void ClearVideoFrames() {
    std::lock_guard lock(video_frame_mutex);
    local_video_frame = {};
    remote_video_frame = {};
    remote_video_by_stream.clear();
    last_video_ms_by_stream.clear();
    local_video_seq = 0;
    remote_video_seq = 0;
    has_remote_video.store(false, std::memory_order_relaxed);
    ever_had_remote_video.store(false, std::memory_order_relaxed);
    last_remote_video_ms.store(0, std::memory_order_relaxed);
  }

  /** Ask the video thread to release the camera (it resets the encoder and clears the preview).
   *  Never joins: the video thread takes `mutex` to read the send callback. */
  void RequestCameraOffLocked() {
    camera_enabled.store(false, std::memory_order_relaxed);
  }

  void ClearLocalPreview() {
    std::lock_guard lock(video_frame_mutex);
    local_video_frame = {};
    local_video_frame.seq = ++local_video_seq;
  }

  void NoteCameraFailure(const std::string& error) {
    std::lock_guard lock(camera_failure_mu);
    camera_failure = error;
  }

  void ClearAudioTracksLocked() {
    audio_tracks.clear();
  }

  /** Release device leases only — keep Opus, tracks, and VoIP session active. Closes run on the
   *  arbiter's device thread; this never blocks on the OS. */
  void CloseAudioDevicesLocked() {
    vpio_underruns_closed += VoiceUnderruns(speaker_lease.get());
    mic_lease.reset();
    speaker_lease.reset();
    capture_available = false;
  }

  /** Playout underruns of the voice-processing unit behind `lease` (0 when none). */
  static uint64_t VoiceUnderruns(AudioDeviceLease* lease) {
    uint64_t underruns = 0;
    if (lease) {
      (void)lease->WithVoiceProcessing([&](IVoiceProcessing& vp) { underruns = vp.PlayoutUnderruns(); });
    }
    return underruns;
  }

  void TearDownAudioLocked() {
    // Caller must not hold mutex across JoinCaptureThread — capture may call sfu_send /
    // OnSfuPacket which need the same mutex (deadlock + SDL double-free on quit).
    capture_running = false;
    playout_running = false;
    audio_reopen_requested.store(false, std::memory_order_relaxed);
    RequestCameraOffLocked();
    CloseAudioDevicesLocked();
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
    fec_frames_total.store(0, std::memory_order_relaxed);
    vpio_underruns_closed = 0;
    last_rx_audio_ms.store(0, std::memory_order_relaxed);
    last_tx_audio_ms.store(0, std::memory_order_relaxed);
    CallAudioSession::Deactivate();
    CallAudioSession::ClearCaptureAudioHints();
  }

  void JoinCaptureThread() {
    capture_running = false;
    JoinThreadBudgeted(capture_thread, std::chrono::milliseconds::max(), "capture");
  }

  void JoinPlayoutThread() {
    playout_running = false;
    JoinThreadBudgeted(playout_thread, std::chrono::milliseconds::max(), "playout");
  }

  /** Product quit: cap capture/playout joins so SDL device close cannot hang Shutdown. */
  void JoinCaptureThreadBudgeted(std::chrono::milliseconds budget) {
    capture_running = false;
    JoinThreadBudgeted(capture_thread, budget, "capture");
  }

  void JoinPlayoutThreadBudgeted(std::chrono::milliseconds budget) {
    playout_running = false;
    JoinThreadBudgeted(playout_thread, budget, "playout");
  }

  void JoinThreadBudgeted(std::thread& thread, std::chrono::milliseconds budget, const char* name) {
    if (!thread.joinable()) {
      return;
    }
    if (budget == std::chrono::milliseconds::max() || budget.count() <= 0) {
      thread.join();
      return;
    }
    auto finished = std::make_shared<std::atomic<bool>>(false);
    std::thread waiter([finishing = std::move(thread), finished]() mutable {
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
      return;
    }
    SDL_Log("CallMediaEngine: %s join still live after %lldms — detaching (process exit must follow)",
            name, static_cast<long long>(budget.count()));
    waiter.detach();
  }

  /** One playout slot for one track: decode Packet / FEC-on-Gap / PLC-on-Empty into `mix`. */
  void PopAndDecodeTrackLocked(RemoteAudioTrack& track, std::vector<int16_t>& mix, bool& any) {
    if (!track.decoder) {
      return;
    }
    const uint64_t underruns_before = track.jitter.underruns();
    AudioPlayoutPop pop = track.jitter.PopForPlayout();
    std::vector<int16_t> pcm(static_cast<size_t>(kFrameSamples), 0);
    const auto plc = [&]() {
      const int n = opus_decode(track.decoder, nullptr, 0, pcm.data(), kFrameSamples, 0);
      if (n > 0) {
        plc_frames_total.fetch_add(1, std::memory_order_relaxed);
      }
      return n;
    };
    int decoded = 0;
    switch (pop.kind) {
    case AudioPlayoutPop::Kind::Packet:
      decoded = opus_decode(track.decoder, pop.payload.data(), static_cast<int>(pop.payload.size()), pcm.data(),
                            kFrameSamples, 0);
      break;
    case AudioPlayoutPop::Kind::Gap:
      // The next packet's in-band FEC (LBRR) only covers the frame right before it; earlier
      // missing frames of a wider hole are PLC. PLC also if the FEC decode fails.
      if (pop.fec_usable) {
        decoded = opus_decode(track.decoder, pop.payload.data(), static_cast<int>(pop.payload.size()), pcm.data(),
                              kFrameSamples, 1);
      }
      if (decoded > 0) {
        // libopus returns >0 both for a real FEC frame and for a PLC-like frame it synthesises
        // when the packet carried no FEC data — counting both as fec= is accepted (diagnostic).
        fec_frames_total.fetch_add(1, std::memory_order_relaxed);
      } else {
        decoded = plc();
      }
      break;
    case AudioPlayoutPop::Kind::Empty:
      // Priming (buffer hasn't reached target depth yet) also returns Empty but is not a real
      // underrun — only count/PLC it when this pop actually incremented the cumulative counter.
      if (track.jitter.underruns() > underruns_before) {
        playout_underruns_total.fetch_add(1, std::memory_order_relaxed);
        decoded = plc();
      }
      break;
    }
    if (decoded <= 0) {
      return;
    }
    pcm.resize(static_cast<size_t>(decoded));
    SmoothLevel(track.peak_level, FramePeakLevel(pcm.data(), decoded));
    MixPcmSat(mix, pcm);
    any = true;
  }

  /**
   * Playout slots this tick, clocked from the speaker's queue (B37): keep ~60 ms queued — a voice-
   * processing unit pulls device-sized chunks, so one chunk plus a frame there — and shed a slot
   * above the high-water mark. Under `mutex`.
   */
  int PlayoutSlotsLocked(AudioDeviceLease& out) {
    size_t render_chunk = 0;
    const bool voice = out.WithVoiceProcessing([&](IVoiceProcessing& vp) { render_chunk = vp.RenderChunkBytes(); });
    const int queued = out.Queued();
    const int target =
        voice ? std::max(kPlayoutTargetQueuedBytes, static_cast<int>(render_chunk) + kFrameBytes) : kPlayoutTargetQueuedBytes;
    const int high_water = target + (kPlayoutHighWaterBytes - kPlayoutTargetQueuedBytes);
    if (queued < 0) {
      return 1;  // errored device: keep one frame per tick so buffers keep draining
    }
    if (queued > high_water) {
      return 0;  // device is ahead: let it drain this tick
    }
    const int deficit = target - queued;
    return deficit <= 0 ? 0 : std::min(kPlayoutMaxSlotsPerTick, (deficit + kFrameBytes - 1) / kFrameBytes);
  }

  void StartPlayoutLoop() {
    playout_running = true;
    playout_thread = std::thread([this]() {
      std::vector<int16_t> mix(static_cast<size_t>(kFrameSamples), 0);
      // Diagnostics: longest gap between playout wake-ups, logged with voice-processing stats.
      auto last_tick = std::chrono::steady_clock::now();
      auto last_diag = last_tick;
      int64_t tick_gap_max_ms = 0;
      while (playout_running.load(std::memory_order_relaxed)) {
        const auto t0 = std::chrono::steady_clock::now();
        tick_gap_max_ms = std::max<int64_t>(
            tick_gap_max_ms, std::chrono::duration_cast<std::chrono::milliseconds>(t0 - last_tick).count());
        last_tick = t0;
        double pressure = 0.0;
        {
          std::lock_guard lock(mutex);
          AudioDeviceLease* out = speaker_lease && speaker_lease->HasDevice() ? speaker_lease.get() : nullptr;
          if (t0 - last_diag >= std::chrono::seconds(2)) {
            last_diag = t0;
            if (out) {
              const int queued = out->Queued();
              (void)out->WithVoiceProcessing([&](IVoiceProcessing& vp) {
                SDL_Log("CallMediaEngine: vpio_diag tick_gap_max_ms=%lld queued_bytes=%d %s",
                        static_cast<long long>(tick_gap_max_ms), queued, vp.TakeDiag().c_str());
              });
            }
            tick_gap_max_ms = 0;
          }
          int slots = out ? PlayoutSlotsLocked(*out) : 1;
          if (audio_tracks.empty()) {
            slots = 0;  // nothing to pop or put; don't inflate playout_ticks (Pressure window)
          }
          for (int s = 0; s < slots; ++s) {
            std::fill(mix.begin(), mix.end(), int16_t{0});
            bool any = false;
            const uint64_t ticks = playout_ticks.fetch_add(1, std::memory_order_relaxed) + 1;
            for (auto& [id, track] : audio_tracks) {
              (void)id;
              if (!track) {
                continue;
              }
              PopAndDecodeTrackLocked(*track, mix, any);
              pressure = std::max(pressure, track->jitter.Pressure(std::max<uint64_t>(ticks, 1)));
            }
            if (out && any) {
              SmoothLevel(remote_output_level, FramePeakLevel(mix.data(), kFrameSamples));
              remote_level_ms.store(util::NowUnixMs(), std::memory_order_relaxed);
              (void)out->Write(mix.data(), kFrameBytes);
            } else if (out) {
              // Silent frame keeps the device clock fed while all streams are priming.
              (void)out->Write(mix.data(), kFrameBytes);
            }
          }
          if (slots == 0) {
            // Still report pressure from buffer fill when we skipped (no pops this tick).
            for (auto& [id, track] : audio_tracks) {
              (void)id;
              if (track) {
                pressure = std::max(pressure, track->jitter.Pressure(std::max<uint64_t>(playout_ticks.load(), 1)));
              }
            }
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
    opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(10));
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(CallAudioSession::OpusEncoderComplexity()));
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

  MediaDeviceArbiter& DeviceArbiter() {
    return *devices;
  }

  /** Capture thread only. Null when refused by policy (logged) — the session runs without it. */
  std::unique_ptr<AudioDeviceLease> AcquireLease(MediaDeviceKind kind, const std::string& holder) {
    AudioLeaseRequest request;
    request.kind = kind;
    request.holder = holder;
    request.format = AudioDeviceFormat{kSampleRate, kChannels};
    request.still_wanted = [this]() { return capture_running.load(std::memory_order_acquire); };
    auto lease = DeviceArbiter().AcquireAudio(request);
    if (!lease) {
      SDL_Log("CallMediaEngine: %s refused: %s", MediaDeviceKindName(kind), lease.error().message.c_str());
      return nullptr;
    }
    return std::move(*lease);
  }

  /**
   * Capture thread only: take the leases the session spec asks for (first open), or reopen held
   * ones and retry missing ones (`reopen` — speaker route change, starved capture, SoftMigrate).
   * Device open / close itself runs on the arbiter's device thread and may block on the OS mic
   * permission prompt — never on UI, relay IO or the mesh host thread.
   */
  Roe<void> EnsureAudioLeases(bool reopen) {
    SessionSpec want_spec;
    std::string holder;
    std::unique_ptr<AudioDeviceLease> mic;
    std::unique_ptr<AudioDeviceLease> speaker;
    {
      std::lock_guard lock(mutex);
      want_spec = spec;
      holder = call_id;
      // A voice-processing pair is one unit: re-acquired whole on reopen (never Reopen'd per half).
      if (reopen && ((mic_lease && mic_lease->IsVoiceDuplex()) || (speaker_lease && speaker_lease->IsVoiceDuplex()))) {
        CloseAudioDevicesLocked();
      }
      mic = std::move(mic_lease);
      speaker = std::move(speaker_lease);
      capture_available = false;
    }
    const bool voip = want_spec.capture && !skip_device_open_for_test.load(std::memory_order_relaxed);
    if (voip) {
      // VoIP audio session only when the mic is ours (a playback-only viewer is not a call).
      // Re-asserted on reopen too: Android route changes can drop MODE_IN_COMMUNICATION.
      // A caller ringback that activated the session may still release it: let it finish first.
      CallRingtone::WaitUntilRingbackSessionReleased();
      if (!capture_running.load(std::memory_order_acquire)) {
        return Error("call media stopped");
      }
      CallAudioSession::ApplyCaptureAudioHints();
      CallAudioSession::ActivateForVoipCall();
    }
    TryAcquireVoiceDuplex(want_spec, holder, mic, speaker);
    const auto ensure = [&](bool wanted, MediaDeviceKind kind, std::unique_ptr<AudioDeviceLease>& lease) {
      if (!wanted || !capture_running.load(std::memory_order_acquire)) {
        return;
      }
      if (lease) {
        if (!lease->IsVoiceDuplex()) {
          (void)lease->Reopen();
        }
      } else {
        lease = AcquireLease(kind, holder);
      }
    };
    ensure(want_spec.capture, MediaDeviceKind::Mic, mic);
    ensure(want_spec.playback, MediaDeviceKind::Speaker, speaker);
    if (!capture_running.load(std::memory_order_acquire)) {
      return Error("call media stopped");  // leases release on scope exit
    }
    const bool voice = mic && mic->IsVoiceDuplex();
    if (voip && !voice) {
      // SDL's coreaudio backend rewrites the AVAudioSession category/options with ModeDefault and
      // DefaultToSpeaker when it opens devices (B36). Re-apply our VoiceChat mode + earpiece route.
      CallAudioSession::ActivateForVoipCall();
    }
    if (speaker && !speaker->HasDevice()) {
      // Headless CI / no default device: still run silence TX (same as no-capture path).
      SDL_Log("CallMediaEngine: no playback device — RX muted; capture/silence TX still active: %s",
              speaker->OpenError().c_str());
    }
    std::lock_guard lock(mutex);
    mic_lease = std::move(mic);
    speaker_lease = std::move(speaker);
    capture_available = mic_lease && mic_lease->HasDevice();
#ifdef PP_BROWSER_CALL_DENOISE
    denoise.Reset();
#endif
    return {};
  }

  /**
   * Duplex calls take mic + speaker from one OS voice-processing unit (echo cancellation) when the
   * backend has one (macOS VPIO; the other platforms' backends say "unsupported"); otherwise — or
   * once disabled for this call — separate leases follow.
   */
  void TryAcquireVoiceDuplex(const SessionSpec& want_spec, const std::string& holder,
                             std::unique_ptr<AudioDeviceLease>& mic, std::unique_ptr<AudioDeviceLease>& speaker) {
    if (!want_spec.capture || !want_spec.playback || mic || speaker ||
        skip_device_open_for_test.load(std::memory_order_relaxed) || !capture_running.load(std::memory_order_acquire)) {
      return;
    }
    if (vpio_disabled_for_call.load(std::memory_order_acquire)) {
      SDL_Log("CallMediaEngine: audio_io=sdl reason=vpio disabled for call (capture starved)");
      return;
    }
    AudioLeaseRequest request;
    request.holder = holder;
    request.format = AudioDeviceFormat{kSampleRate, kChannels};
    request.still_wanted = [this]() { return capture_running.load(std::memory_order_acquire); };
    auto pair = DeviceArbiter().AcquireVoiceDuplex(request);
    if (!pair) {
      SDL_Log("CallMediaEngine: audio_io=sdl reason=%s", pair.error().message.c_str());
      return;
    }
    SDL_Log("CallMediaEngine: audio_io=vpio (voice processing: echo cancellation on)");
    mic = std::move(pair->mic);
    speaker = std::move(pair->speaker);
  }

  void StartCaptureLoop() {
    // Precondition: capture_thread not joinable (JoinCaptureThread outside media mutex).
    capture_running = true;
    audio_reopen_requested.store(false, std::memory_order_relaxed);
    vpio_disabled_for_call.store(false, std::memory_order_relaxed);
    capture_thread = std::thread([this]() {
      // Device open (and OS mic prompts) stay on this worker so CallAccept /
      // AcceptInvite can finish signaling without freezing UI or libp2p.
      if (auto audio = EnsureAudioLeases(/*reopen=*/false); !audio) {
        SDL_Log("CallMediaEngine: audio devices: %s", audio.error().message.c_str());
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
      int64_t last_capture_pcm_ms = util::NowUnixMs();
      // Opus encoders are not thread-safe: bitrate changes (ApplyAdaptation, any thread) are
      // applied here, on the thread that encodes, right before the next frame.
      OpusEncoder* bitrate_enc = nullptr;
      int64_t applied_audio_bps = 0;
      int64_t last_capture_starve_reopen_ms = 0;
      // I3: consecutive starvation-triggered reopens while on voice processing; 3 in a row falls
      // back to SDL for the rest of this call. Capture-thread-local.
      int vpio_starve_reopens = 0;
      while (capture_running.load()) {
        if (audio_reopen_requested.exchange(false, std::memory_order_acq_rel)) {
          // Android speakerphone / SoftMigrate can leave AudioRecord feeding zeros until reopen.
          // Android settle (Moto safety ramp) is the backend's ReopenSettle, on the device thread.
          pending.clear();
          SDL_Log("CallMediaEngine: reopening audio devices (speaker route / SoftMigrate)");
          if (auto audio = EnsureAudioLeases(/*reopen=*/true); !audio) {
            SDL_Log("CallMediaEngine: audio reopen failed: %s", audio.error().message.c_str());
          } else if (!capture_available) {
            SDL_Log("CallMediaEngine: audio reopen — no capture device; sending silence");
          }
          last_capture_pcm_ms = util::NowUnixMs();
        }
        // The capture thread is mic_lease's only writer while it runs: read it without `mutex`.
        bool device_changed = false;
        const bool vpio_on = capture_available && mic_lease &&
                             mic_lease->WithVoiceProcessing([&](IVoiceProcessing& vp) { device_changed = vp.TakeDeviceChanged(); });
        if (device_changed) {
          SDL_Log("CallMediaEngine: default audio device changed — reopening voice processing");
          audio_reopen_requested.store(true, std::memory_order_release);
          continue;
        }
        std::shared_ptr<SfuSendFn> send_fn;
        OpusEncoder* enc = nullptr;
        bool can_send = false;
        bool capturing = true;
        {
          std::lock_guard lock(mutex);
          capturing = spec.capture;
          enc = encoder;
          can_send = capturing && sfu_mode && static_cast<bool>(sfu_send) && enc;
          if (can_send) {
            send_fn = sfu_send;
          }
        }
        if (!capturing) {
          // Playback-only: this thread only owns the devices (open / reopen) — nothing to encode.
          if (util::NowUnixMs() - remote_level_ms.load(std::memory_order_relaxed) > 40) {
            SmoothLevel(remote_output_level, 0.f);
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(kFrameMs));
          continue;
        }
        bool paced_silence_frame = false;
        if (capture_available) {
          int16_t chunk[kFrameSamples];
          const int got = mic_lease->Read(chunk, static_cast<int>(sizeof(chunk)));
          if (got > 0) {
            const size_t samples = static_cast<size_t>(got) / sizeof(int16_t);
            pending.insert(pending.end(), chunk, chunk + samples);
            last_capture_pcm_ms = util::NowUnixMs();
            if (vpio_on) {
              vpio_starve_reopens = 0;
            }
          }
          if (pending.size() < static_cast<size_t>(kFrameSamples)) {
            const int64_t now = util::NowUnixMs();
            // Wedged capture (got<=0 after AAudio disconnect) used to spin without TX —
            // peers saw "Your mic isn't sending". After ~500ms, encode silence and reopen.
            constexpr int64_t kStarveMs = 500;
            if (got <= 0 && (now - last_capture_pcm_ms) >= kStarveMs) {
              if (!muted.load(std::memory_order_relaxed)) {
                SmoothLevel(local_input_level, 0.f);
              }
              if (now - last_capture_starve_reopen_ms > 2000) {
                last_capture_starve_reopen_ms = now;
                SDL_Log("CallMediaEngine: capture starved %lldms — requesting reopen",
                        static_cast<long long>(now - last_capture_pcm_ms));
                audio_reopen_requested.store(true, std::memory_order_release);
                // I3: persistent voice-processing capture starvation (not just a starved SDL
                // device) falls back to SDL for the rest of this call after 3 reopens in a row.
                if (vpio_on && ++vpio_starve_reopens >= 3) {
                  vpio_disabled_for_call.store(true, std::memory_order_release);
                  SDL_Log("CallMediaEngine: vpio capture starved 3x — using SDL for this call");
                }
              }
              std::fill(pcm.begin(), pcm.end(), int16_t{0});
              paced_silence_frame = true;
              // Fall through to encode/send silence so tx_alive stays true.
            } else {
              if (got <= 0) {
                if (!muted.load(std::memory_order_relaxed)) {
                  SmoothLevel(local_input_level, 0.f);
                }
                if (now - remote_level_ms.load(std::memory_order_relaxed) > 40) {
                  SmoothLevel(remote_output_level, 0.f);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
              }
              continue;
            }
          } else {
            std::copy_n(pending.begin(), kFrameSamples, pcm.begin());
            pending.erase(pending.begin(), pending.begin() + kFrameSamples);
            if (muted.load(std::memory_order_relaxed)) {
              std::fill(pcm.begin(), pcm.end(), int16_t{0});
              SmoothLevel(local_input_level, 0.f);
            } else {
#ifdef PP_BROWSER_CALL_DENOISE
              if (!vpio_on) {  // voice processing suppresses noise itself
                denoise.Process(pcm.data(), pcm.size());
                const int64_t now_ms = util::NowUnixMs();
                if (now_ms - last_denoise_log_ms >= 5000) {
                  last_denoise_log_ms = now_ms;
                  SDL_Log("CallMediaEngine: noise_floor_dbfs=%.1f", denoise.last_noise_floor_dbfs());
                }
              }
#endif
              SmoothLevel(local_input_level, FramePeakLevel(pcm.data(), kFrameSamples));
            }
          }
        } else {
          std::fill(pcm.begin(), pcm.end(), int16_t{0});
          SmoothLevel(local_input_level, 0.f);
          paced_silence_frame = true;
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
        if (const int64_t want_bps = adaptation_target_audio_bps.load(std::memory_order_relaxed);
            want_bps > 0 && (enc != bitrate_enc || want_bps != applied_audio_bps)) {
          opus_encoder_ctl(enc, OPUS_SET_BITRATE(static_cast<int>(want_bps)));
          bitrate_enc = enc;
          applied_audio_bps = want_bps;
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
        // Pace silence / starved-capture TX to one Opus frame per period.
        if (paced_silence_frame) {
          std::this_thread::sleep_for(std::chrono::milliseconds(kFrameMs));
        }
      }
    });
  }

  void StartVideoLoop() {
    video_running = true;
    video_thread = std::thread([this]() { RunVideoLoop(); });
  }

  void JoinVideoThread() {
    video_running = false;
    JoinThreadBudgeted(video_thread, std::chrono::milliseconds::max(), "video");
  }

  /**
   * Video thread only: take the camera lease (open runs on the device thread). A failed open drops
   * the request and records why; a request withdrawn while opening releases the lease again.
   */
  std::unique_ptr<CameraDeviceLease> OpenCameraLease() {
    CameraLeaseRequest request;
    {
      std::lock_guard lock(mutex);
      request.holder = call_id;
    }
    request.format.display_rotation_deg = camera_display_rotation.load(std::memory_order_relaxed);
    request.format.fps = kVideoFps;
    auto lease = DeviceArbiter().AcquireCamera(request);
    if (!lease) {
      camera_enabled.store(false, std::memory_order_relaxed);
      NoteCameraFailure(lease.error().message);
      SDL_Log("CallMediaEngine: camera: %s", lease.error().message.c_str());
      return nullptr;
    }
    if (!camera_enabled.load(std::memory_order_relaxed)) {
      return nullptr;
    }
    return std::move(*lease);
  }

  /** Video thread only. */
  void ConfigureLocalEncoder(const CameraGeometry& geometry) {
    // Created with the camera, not the session: playback-only and audio-only sessions never open a
    // hardware encoder (media-client-layers l3a).
    if (!video_codec) {
      video_codec = make_video_codec();
    }
    if (!video_codec) {
      return;
    }
    if (auto cfg = video_codec->ConfigureEncoder(geometry.encode_width, geometry.encode_height, kVideoFps); !cfg) {
      SDL_Log("CallMediaEngine: video encode unavailable: %s — local preview may still work; voice continues",
              cfg.error().message.c_str());
    }
  }

  /** Video thread only: bitrate changes (ApplyAdaptation, any thread) land right before an encode. */
  void ApplyVideoBitrate(int64_t& applied_bps) {
    const int64_t want = adaptation_target_video_bps.load(std::memory_order_relaxed);
    if (video_codec && want > 0 && want != applied_bps) {
      video_codec->SetTargetBitrate(want);
      applied_bps = want;
    }
  }

  static VideoFrameRgba OrientFrame(VideoFrameRgba frame, int rotate_cw) {
    VideoFrameRgba rotated;
    if (rotate_cw == 90 && RotateRgba90Cw(frame, rotated)) {
      return rotated;
    }
    if (rotate_cw == 270 && RotateRgba90Ccw(frame, rotated)) {
      return rotated;
    }
    VideoFrameRgba twice;
    if (rotate_cw == 180 && RotateRgba90Cw(frame, rotated) && RotateRgba90Cw(rotated, twice)) {
      return twice;
    }
    return frame;
  }

  /** Video thread only: orient + crop → preview; encode → send on channel 1. */
  void EncodeAndSend(VideoFrameRgba captured, const CameraGeometry& geometry, bool& need_keyframe) {
    const VideoFrameRgba oriented = OrientFrame(std::move(captured), geometry.rotate_cw);
    VideoFrameRgba fitted;
    VideoFrameI420 i420;
    if (!ScaleCenterCropRgba(oriented, geometry.encode_width, geometry.encode_height, fitted) ||
        !RgbaToI420(fitted.rgba.data(), fitted.width, fitted.height, fitted.width * 4, true, i420)) {
      return;
    }
    PublishLocalPreview(fitted);
    if (!video_codec || !video_codec->HasEncoder()) {
      return;
    }
    auto encoded = video_codec->Encode(i420, need_keyframe);
    if (!encoded || encoded->annex_b.empty()) {
      return;
    }
    need_keyframe = false;
    std::shared_ptr<SfuSendFn> send_fn;
    {
      std::lock_guard lock(mutex);
      if (sfu_mode && sfu_send) {
        send_fn = sfu_send;
      }
    }
    if (!send_fn) {
      return;
    }
    SfuPacket pkt;
    pkt.channel_id = 1;
    pkt.seq = sfu_video_seq.fetch_add(1) + 1;
    pkt.mark = encoded->keyframe ? 1 : 0;
    pkt.payload = std::move(encoded->annex_b);
    try {
      std::lock_guard send_lock(sfu_send_call_mu);
      (*send_fn)(pkt);
      tx_video_frames.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
    }
  }

  /** The camera lease lives on this thread: open when wanted, release when not, frames in between. */
  void RunVideoLoop() {
    const auto frame_period = std::chrono::milliseconds(1000 / kVideoFps);
    std::unique_ptr<CameraDeviceLease> camera;
    CameraGeometry geometry;
    bool need_keyframe = true;
    int64_t applied_bps = 0;
    while (video_running.load()) {
      if (video_need_keyframe.exchange(false, std::memory_order_acq_rel)) {
        need_keyframe = true;
      }
      const bool wanted = camera_enabled.load(std::memory_order_relaxed);
      if (wanted && !camera) {
        camera = OpenCameraLease();
        if (camera) {
          geometry = camera->Geometry();
          ConfigureLocalEncoder(geometry);
          applied_bps = 0;
          need_keyframe = true;
        }
      } else if (!wanted && camera) {
        camera.reset();
        if (video_codec) {
          video_codec->ResetEncoder();
        }
        ClearLocalPreview();
      }
      if (!camera) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        continue;
      }
      const auto t0 = std::chrono::steady_clock::now();
      ApplyVideoBitrate(applied_bps);
      auto frame = camera->NextFrame();
      if (!frame) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }
      EncodeAndSend(std::move(*frame), geometry, need_keyframe);
      const auto elapsed = std::chrono::steady_clock::now() - t0;
      if (elapsed < frame_period) {
        std::this_thread::sleep_for(frame_period - elapsed);
      }
    }
  }

  void OnRemoteOpusFrame(uint32_t stream_id, uint32_t seq, const std::byte* data, size_t size) {
    if (size == 0) {
      return;
    }
    RemoteAudioTrack* track = EnsureRemoteTrackLocked(stream_id);
    if (!track || !track->decoder) {
      return;
    }
    // Decoded at playout (packet-level jitter buffer: FEC on a gap needs the next packet).
    const int64_t recv_ms = util::NowUnixMs();
    ++track->rx_frames;
    track->last_rx_ms = recv_ms;
    AudioPacket packet;
    packet.seq = seq;
    packet.recv_ms = recv_ms;
    packet.payload.assign(reinterpret_cast<const uint8_t*>(data), reinterpret_cast<const uint8_t*>(data) + size);
    track->jitter.Push(std::move(packet));
    rx_audio_frames.fetch_add(1, std::memory_order_relaxed);
    last_rx_audio_ms.store(recv_ms, std::memory_order_relaxed);
  }

  void OnRemoteH264Frame(uint32_t stream_id, const std::byte* data, size_t size) {
    if (size == 0) {
      return;
    }
    IVideoCodec* decoder = nullptr;
    {
      std::lock_guard lock(mutex);
      auto it = remote_decoders.find(stream_id);
      if (it == remote_decoders.end()) {
        if (remote_decoders.size() >= kMaxRemoteVideoDecoders) {
          return;
        }
        auto created = make_video_codec();
        if (!created) {
          return;
        }
        it = remote_decoders.emplace(stream_id, std::move(created)).first;
      }
      decoder = it->second.get();
    }
    if (!decoder) {
      return;
    }
    if (!decoder->HasDecoder()) {
      if (auto configured = decoder->ConfigureDecoder(); !configured) {
        static bool logged_cfg = false;
        if (!logged_cfg) {
          logged_cfg = true;
          SDL_Log("CallMediaEngine: ConfigureDecoder failed: %s", configured.error().message.c_str());
        }
        return;
      }
    }
    auto decoded = decoder->Decode(reinterpret_cast<const uint8_t*>(data), size);
    if (!decoded) {
      static int logged_decode = 0;
      if (logged_decode < 5) {
        ++logged_decode;
        SDL_Log("CallMediaEngine: remote H264 decode failed: %s (size=%zu)",
                decoded.error().message.c_str(), size);
      }
      NoteVideoRefreshNeeded(stream_id);
      return;
    }
    PublishRemoteFrame(stream_id, std::move(*decoded));
  }

};

CallMediaEngine::CallMediaEngine() : CallMediaEngine(MediaDeviceArbiter::Default()) {}

CallMediaEngine::CallMediaEngine(MediaDeviceArbiter& devices) : impl_(std::make_unique<Impl>()) {
  redirectLogger("CallMediaEngine");
  impl_->own_devices = &devices;
  impl_->devices = &devices;
}

CallMediaEngine::~CallMediaEngine() {
  Stop();
}

void CallMediaEngine::SetOnStateChanged(StateChangedFn callback) {
  std::lock_guard lock(impl_->mutex);
  impl_->on_state_changed = std::move(callback);
}

void CallMediaEngine::SetVideoCodecFactoryForTest(std::function<std::unique_ptr<IVideoCodec>()> make) {
  std::lock_guard lock(impl_->mutex);
  impl_->make_video_codec = std::move(make);
}

void CallMediaEngine::SetSkipDeviceOpenForTest(bool skip) {
  impl_->skip_device_open_for_test.store(skip, std::memory_order_relaxed);
  std::lock_guard lock(impl_->mutex);
  if (skip && !impl_->test_devices) {
    impl_->test_devices = std::make_unique<MediaDeviceArbiter>(CreateNullMediaDeviceBackend());
  }
  impl_->devices = skip ? impl_->test_devices.get() : impl_->own_devices;
}

Roe<void> CallMediaEngine::StartSfu(const std::string& call_id, SfuSendFn send) {
  return Start(call_id, SessionSpec::Duplex(), std::move(send));
}

CallMediaEngine::SessionSpec CallMediaEngine::ActiveSpec() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->spec;
}

Roe<void> CallMediaEngine::Start(const std::string& call_id, const SessionSpec spec, SfuSendFn send) {
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
    if (!spec.capture && !spec.playback) {
      return Error("media session needs capture or playback");
    }
    if (spec.capture && !*next_send) {
      return Error("SFU send callback required");
    }
    // Invalidate any in-flight StopMeshMedia posted for a prior call_id / leftover purge.
    impl_->session_generation.fetch_add(1, std::memory_order_acq_rel);
    if (impl_->active) {
      if (impl_->call_id == call_id && impl_->sfu_mode && impl_->spec == spec) {
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
    // 1:1 libp2p also uses StartSfu with stream_id=0 packets. SoftMigrate to media_relay must
    // drop that zombie track or it PLC-underruns forever and confuses stream_count (dogfood).
    // Do NOT wipe live media_relay RX tracks on duplicate StartSfu send-swap (reattach storm).
    {
      std::lock_guard lock(impl_->mutex);
      impl_->audio_tracks.erase(0);
      {
        std::lock_guard lg(impl_->sfu_rx_log_mu);
        impl_->sfu_rx_logged_streams.erase(0);
      }
    }
    // Android speaker / communication-mode route changes around SoftMigrate can leave the open
    // SDL recording device feeding zeros while encode still runs (mic_lvl→0, tx_frames rising).
    // Desktop SoftMigrate send-swap must not close/reopen SDL (malloc/double-free on Leave).
    if (CallAudioSession::CaptureReopenSettleDelayMs() > 0) {
      RequestAudioDeviceReopen();
    }
    RequestVideoKeyframe();
    return {};
  }
  abandoned_send = nullptr;
  if (need_rebuild) {
    impl_->playout_running = false;
    impl_->JoinCaptureThread();
    impl_->JoinPlayoutThread();
    impl_->JoinVideoThread();  // outside `mutex`: the video thread takes it to read the send callback
    std::lock_guard lock(impl_->mutex);
    impl_->TearDownAudioLocked();
  }
  {
    std::lock_guard lock(impl_->mutex);
    if (auto codecs = impl_->EnsureOpusCodecs(); !codecs) {
      impl_->TearDownAudioLocked();
      return codecs.error();
    }
    impl_->sfu_mode = true;
    impl_->spec = spec;
    impl_->sfu_send = spec.capture ? std::move(next_send) : nullptr;
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
    {
      std::lock_guard lg(impl_->sfu_rx_log_mu);
      impl_->sfu_rx_logged_streams.clear();
    }
    impl_->connected_at_ms.store(util::NowUnixMs(), std::memory_order_relaxed);
    impl_->started_at_ms.store(util::NowUnixMs(), std::memory_order_relaxed);
    impl_->last_remote_video_ms.store(0, std::memory_order_relaxed);
    impl_->ever_had_remote_video.store(false, std::memory_order_relaxed);
    impl_->ApplyStateLocked("connected");
    impl_->StartCaptureLoop();  // device owner for both halves; encodes only when capturing
    if (spec.playback && !impl_->playout_thread.joinable()) {
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
  if (!impl_->active || !impl_->sfu_mode || !impl_->spec.playback || packet.payload.empty()) {
    return;
  }
  // PublisherStreamIdForIdentity("") == 1. Never a real media_relay publisher — only the
  // inbound 1:1 path with a missing peer_key. Drop so SoftMigrate cannot leave a PLC zombie.
  if (packet.stream_id == 1u) {
    return;
  }
  {
    // Log the first packet per stream_id per process (PreferLocal multi-peer RX).
    // Cleared on SoftMigrate StartSfu send-swap so post-migrate RX is visible again.
    bool log_this = false;
    {
      std::lock_guard lg(impl_->sfu_rx_log_mu);
      log_this = impl_->sfu_rx_logged_streams.insert(packet.stream_id).second;
    }
    if (log_this) {
      log().info << "OnSfuPacket first stream=" << packet.stream_id << " ch=" << packet.channel_id
                 << " seq=" << packet.seq << " bytes=" << packet.payload.size()
                 << " call=" << impl_->call_id;
    }
  }
  if (packet.channel_id == 0) {
    impl_->OnRemoteOpusFrame(packet.stream_id, packet.seq,
                             reinterpret_cast<const std::byte*>(packet.payload.data()),
                             packet.payload.size());
  } else if (packet.channel_id == 1) {
    impl_->OnRemoteH264Frame(packet.stream_id, reinterpret_cast<const std::byte*>(packet.payload.data()),
                             packet.payload.size());
  }
}

void CallMediaEngine::ClearRemoteAudioTracks() {
  std::lock_guard lock(impl_->mutex);
  impl_->ClearAudioTracksLocked();
}

bool CallMediaEngine::IsSfuMode() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->sfu_mode;
}

void CallMediaEngine::ApplyAdaptation(const CallAdaptationDecision& decision) {
  impl_->adaptation_camera_allowed.store(decision.camera_allowed, std::memory_order_relaxed);
  impl_->adaptation_target_video_bps.store(decision.target_video_lo_bps, std::memory_order_relaxed);
  const int64_t audio_bps =
      decision.target_audio_bps > 0 ? decision.target_audio_bps : CallMediaAdaptation::kComfortAudioBps;
  impl_->adaptation_target_audio_bps.store(audio_bps, std::memory_order_relaxed);
  // Bitrates are applied by the threads that encode, right before their next frame (encoders are
  // not thread-safe — calling opus_encoder_ctl here raced opus_encode, TSan).
  if (!decision.camera_allowed) {
    std::lock_guard lock(impl_->mutex);
    impl_->RequestCameraOffLocked();
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
  h.muted = impl_->muted.load(std::memory_order_relaxed);
  h.path_pressure = impl_->path_pressure.load(std::memory_order_relaxed);
  h.opus_target_bps = impl_->adaptation_target_audio_bps.load(std::memory_order_relaxed);
  h.outbound_drops = impl_->outbound_drops.load(std::memory_order_relaxed);
  h.playout_underruns = impl_->playout_underruns_total.load(std::memory_order_relaxed);
  h.plc_frames = impl_->plc_frames_total.load(std::memory_order_relaxed);
  h.fec_frames = impl_->fec_frames_total.load(std::memory_order_relaxed);
  h.rx_audio_frames = impl_->rx_audio_frames.load(std::memory_order_relaxed);
  h.tx_audio_frames = impl_->tx_audio_frames.load(std::memory_order_relaxed);
  h.last_rx_audio_ms = impl_->last_rx_audio_ms.load(std::memory_order_relaxed);
  h.last_tx_audio_ms = impl_->last_tx_audio_ms.load(std::memory_order_relaxed);
  h.rx_video_frames = impl_->rx_video_frames.load(std::memory_order_relaxed);
  h.tx_video_frames = impl_->tx_video_frames.load(std::memory_order_relaxed);
  h.last_rx_video_ms = impl_->last_rx_video_ms.load(std::memory_order_relaxed);
  h.video_target_bps = impl_->adaptation_target_video_bps.load(std::memory_order_relaxed);
  h.local_level = impl_->local_input_level.load(std::memory_order_relaxed);
  h.remote_level = impl_->remote_output_level.load(std::memory_order_relaxed);
  {
    std::lock_guard lock(impl_->mutex);
    AudioDeviceLease* speaker = impl_->speaker_lease.get();
    const bool voice = speaker && speaker->IsVoiceDuplex();
    const bool any_device = (impl_->mic_lease && impl_->mic_lease->HasDevice()) || (speaker && speaker->HasDevice());
    h.audio_io = voice ? "vpio" : (any_device ? "sdl" : "none");
    h.io_underruns = impl_->vpio_underruns_closed + Impl::VoiceUnderruns(speaker);
    h.stream_count = impl_->audio_tracks.size();
    h.sfu_mode = impl_->sfu_mode;
    h.capture_available = impl_->capture_available;
    h.streams.reserve(impl_->audio_tracks.size());
    for (const auto& [id, track] : impl_->audio_tracks) {
      if (!track) {
        continue;
      }
      CallMediaStreamHealth s;
      s.stream_id = id;
      s.rx_frames = track->rx_frames;
      s.last_rx_ms = track->last_rx_ms;
      s.peak_level = track->peak_level;
      h.streams.push_back(s);
    }
  }
  return h;
}

void CallMediaEngine::Stop() {
  StateChangedFn state_cb;
  std::shared_ptr<SfuSendFn> abandoned_send;
  {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->active && !impl_->sfu_mode) {
      // Still join threads if a prior StartSfu left them running after a failed half-stop.
      if (!impl_->capture_thread.joinable() && !impl_->playout_thread.joinable() &&
          !impl_->video_thread.joinable()) {
        return;
      }
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
  // Always join — never detach. Budgeted detach raced TearDownAudioLocked / ~Impl with
  // capture still inside SDL_OpenAudioDeviceStream (Windows CI headless: segfault after
  // "OpenAudioDevices failed"). Product quit must wait for the worker; SDL open fails fast
  // when no device is present.
  impl_->JoinCaptureThread();
  impl_->JoinPlayoutThread();
  impl_->JoinVideoThread();
  {
    std::lock_guard lock(impl_->mutex);
    impl_->TearDownAudioLocked();
    impl_->call_id.clear();
    impl_->spec = SessionSpec::Duplex();
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

void CallMediaEngine::RequestAudioDeviceReopen() {
  if (!impl_->capture_running.load(std::memory_order_relaxed)) {
    return;
  }
  impl_->audio_reopen_requested.store(true, std::memory_order_release);
}

Roe<void> CallMediaEngine::SetCameraEnabled(bool enabled) {
  // Read on the caller's (UI) thread: iOS orientation is UIKit; the open itself runs elsewhere.
  return SetCameraEnabled(enabled, enabled ? CameraDisplayRotationDegrees() : 0);
}

Roe<void> CallMediaEngine::SetCameraEnabled(bool enabled, const int display_rotation) {
  std::lock_guard lock(impl_->mutex);
  if (!impl_->active) {
    return Error("Call media not active");
  }
  if (!enabled) {
    impl_->RequestCameraOffLocked();
    return {};
  }
  if (!impl_->spec.capture) {
    return Error("camera needs a capturing session");
  }
  if (!impl_->adaptation_camera_allowed.load(std::memory_order_relaxed)) {
    return Error("Camera blocked by adaptation (uplink/path)");
  }
  if (impl_->skip_device_open_for_test.load(std::memory_order_relaxed)) {
    return Error("camera skipped (test)");
  }
  // Async: the video thread takes the camera lease (opened on the media device thread). A failed
  // open flips IsCameraEnabled back to false and is reported through TakeCameraFailure.
  impl_->camera_display_rotation.store(display_rotation, std::memory_order_relaxed);
  impl_->camera_enabled.store(true, std::memory_order_relaxed);
  if (!impl_->video_thread.joinable()) {
    impl_->StartVideoLoop();
  }
  return {};
}

std::optional<std::string> CallMediaEngine::TakeCameraFailure() {
  std::lock_guard lock(impl_->camera_failure_mu);
  if (impl_->camera_failure.empty()) {
    return std::nullopt;
  }
  return std::exchange(impl_->camera_failure, std::string());
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
  std::lock_guard lock(impl_->video_frame_mutex);
  int64_t newest_ms = 0;
  uint32_t newest_stream = 0;
  for (auto it = impl_->last_video_ms_by_stream.begin(); it != impl_->last_video_ms_by_stream.end();) {
    if (it->second > 0 && (now - it->second) >= kRemoteVideoStallHardMs) {
      impl_->remote_video_by_stream.erase(it->first);
      it = impl_->last_video_ms_by_stream.erase(it);
      continue;
    }
    if (it->second >= newest_ms) {
      newest_ms = it->second;
      newest_stream = it->first;
    }
    ++it;
  }
  if (impl_->last_video_ms_by_stream.empty()) {
    impl_->remote_video_frame = {};
    impl_->has_remote_video.store(false, std::memory_order_relaxed);
    impl_->last_remote_video_ms.store(0, std::memory_order_relaxed);
    return;
  }
  impl_->last_remote_video_ms.store(newest_ms, std::memory_order_relaxed);
  auto live = impl_->remote_video_by_stream.find(newest_stream);
  if (live != impl_->remote_video_by_stream.end()) {
    impl_->remote_video_frame = live->second;
  }
}

bool CallMediaEngine::VideoEncoderAvailable() const {
  // Host capability, not session state: the local encoder only exists while the camera is on.
  return PlatformVideoEncoderSupported();
}

bool CallMediaEngine::CameraPathAllowsVideo() const {
  CallAdaptationInput in;
  in.camera_user_wants = true;
  in.path_pressure = PathPressure();
  in.per_user_up_bps = impl_->last_uplink_bps.load(std::memory_order_relaxed);
  return CallMediaAdaptation::Evaluate(in).camera_allowed;
}

void CallMediaEngine::NoteUplinkBudget(int64_t per_user_up_bps) {
  impl_->last_uplink_bps.store(per_user_up_bps, std::memory_order_relaxed);
}

void CallMediaEngine::RequestVideoKeyframe() {
  impl_->video_need_keyframe.store(true, std::memory_order_release);
}

std::vector<uint32_t> CallMediaEngine::TakePendingVideoRefreshStreamIds() {
  std::lock_guard lock(impl_->video_refresh_mu);
  std::vector<uint32_t> out(impl_->pending_video_refresh_streams.begin(),
                            impl_->pending_video_refresh_streams.end());
  impl_->pending_video_refresh_streams.clear();
  return out;
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

uint64_t CallMediaEngine::MediaSessionGeneration() const {
  return impl_->session_generation.load(std::memory_order_acquire);
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

bool CallMediaEngine::CopyRemoteVideoFrameForStream(uint32_t stream_id, VideoTileFrame& out) const {
  std::lock_guard lock(impl_->video_frame_mutex);
  auto it = impl_->remote_video_by_stream.find(stream_id);
  if (it == impl_->remote_video_by_stream.end() || it->second.rgba.empty()) {
    return false;
  }
  out = it->second;
  return true;
}

bool CallMediaEngine::HasRemoteVideoForStream(uint32_t stream_id) const {
  std::lock_guard lock(impl_->video_frame_mutex);
  auto it = impl_->remote_video_by_stream.find(stream_id);
  return it != impl_->remote_video_by_stream.end() && !it->second.rgba.empty();
}

} // namespace pbr
