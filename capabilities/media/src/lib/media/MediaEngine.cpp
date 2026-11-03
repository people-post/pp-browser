#include "lib/media/MediaEngine.h"

#include "mesh/types/MediaChannel.h"
#include "lib/media/VideoLevelProfile.h"

#include "lib/media/AudioSession.h"
#include "lib/media/AudioPlayout.h"
#include "lib/media/CameraCaptureOrientation.h"
#include "lib/media/CaptureStarvePolicy.h"
#include "lib/media/DecodeStallPolicy.h"
#include "lib/media/GuardedVideoCodec.h"
#include "common/Metrics.h"
#include "lib/media/IVideoCodec.h"
#include "lib/media/MediaDeviceArbiter.h"
#include "lib/media/NoiseSuppressor.h"
#include "lib/media/VideoYuv.h"
#include "common/Utilities.h"

#include <SDL3/SDL.h>
#include <opus.h>

#include <optional>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
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
#include "common/MeshAlias.h"

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
/** +6 dB on the voice-processing (VPIO) path: the OS attenuates call output there by design and
 *  the Mac speaker was noticeably quieter than other apps. Soft-kneed, so peaks don't clip. */
constexpr float kVoicePlayoutGain = 2.0f;
constexpr int kPlayoutHighWaterBytes = 6 * kFrameBytes;
/** Never produce more than this many slots per 20 ms wake-up (startup / after a stall). */
constexpr int kPlayoutMaxSlotsPerTick = 3;
constexpr int kVideoFps = 20;
/** How often a session waiting for a busy or stuck device asks the arbiter again (L016). */
constexpr auto kDeviceRetryPoll = std::chrono::milliseconds(100);
/** A new session's first mic open waits this long for the previous session to let the mic go. */
constexpr auto kMicHandoverWait = std::chrono::milliseconds(1500);
/** CameraFailure details (the log's words; the screen words the failure from its kind). */
constexpr const char* kCameraBusyMessage = "Camera is busy — video turns on when it's free";
constexpr const char* kCameraStuckMessage = "Camera isn't responding — restart the app to use it";
/** A hardware codec that stops responding is replaced this many times per camera request / stream. */
constexpr int kMaxCodecRestarts = 3;
constexpr const char* kEncoderStuckMessage = "Video isn't responding — restart the app to send video";
/** Soft stall: keep last frame; UI may show Reconnecting… */
constexpr int64_t kRemoteVideoStallSoftMs = 2000;
/** Hard stall: drop last frame so the tile does not freeze forever. */
constexpr int64_t kRemoteVideoStallHardMs = 5000;
/** A getter waiting this long for the media mutex is logged (B125), at most kMaxLockWaitWarns per session. */
constexpr int64_t kLockWaitWarnMs = 50;
constexpr int kMaxLockWaitWarns = 10;
/** A Stop() step (thread join, decoder hand-off, audio teardown) longer than this is a WARN (B128). */
constexpr int64_t kSlowStopStepMs = 500;

/** For Impl, which is not the Module (same channel as MediaEngine::log()). */
logging::Logger& EngineLog() {
  static logging::Logger log = logging::getLogger("MediaEngine");
  return log;
}

int64_t MsSince(std::chrono::steady_clock::time_point since) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count();
}

int64_t SteadyNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

/** A join still waiting after this logs which stage the thread is in (B128), then keeps waiting. */
constexpr auto kJoinStageWarnAfter = std::chrono::milliseconds(2000);

/** Where a media thread is (B128: Stop hung joining one; which session was it in?). */
enum class MediaThreadStage : uint8_t {
  Idle,
  OpenDevices,  // capture: audio leases open / reopen
  WaitCamera,   // video: no camera wanted / open yet
  OpenCamera,
  ConfigureEncoder,
  ResetEncoder,
  CloseCamera,
  SetBitrate,
  WaitFrame,
  Convert,      // video: orient / crop / preview
  ReadMic,
  Encode,
  LockMedia,    // waiting for the media mutex
  Mix,          // playout: decode + mix + speaker write, under the media mutex
  Send,
  Sleep,
  Exiting,
};

const char* MediaThreadStageName(MediaThreadStage stage) {
  switch (stage) {
    case MediaThreadStage::Idle: return "idle";
    case MediaThreadStage::OpenDevices: return "open_devices";
    case MediaThreadStage::WaitCamera: return "wait_camera";
    case MediaThreadStage::OpenCamera: return "open_camera";
    case MediaThreadStage::ConfigureEncoder: return "configure_encoder";
    case MediaThreadStage::ResetEncoder: return "reset_encoder";
    case MediaThreadStage::CloseCamera: return "close_camera";
    case MediaThreadStage::SetBitrate: return "set_bitrate";
    case MediaThreadStage::WaitFrame: return "wait_frame";
    case MediaThreadStage::Convert: return "convert";
    case MediaThreadStage::ReadMic: return "read_mic";
    case MediaThreadStage::Encode: return "encode";
    case MediaThreadStage::LockMedia: return "lock_media";
    case MediaThreadStage::Mix: return "mix";
    case MediaThreadStage::Send: return "send";
    case MediaThreadStage::Sleep: return "sleep";
    case MediaThreadStage::Exiting: return "exiting";
  }
  return "unknown";
}

/** One media thread's current stage and since when (steady ms); relaxed stores from that thread. */
struct MediaThreadStageTracker {
  std::atomic<uint8_t> stage{static_cast<uint8_t>(MediaThreadStage::Idle)};
  std::atomic<int64_t> since_ms{0};

  void Set(MediaThreadStage next) {
    if (stage.load(std::memory_order_relaxed) == static_cast<uint8_t>(next)) {
      return;
    }
    stage.store(static_cast<uint8_t>(next), std::memory_order_relaxed);
    since_ms.store(SteadyNowMs(), std::memory_order_relaxed);
  }
  const char* Name() const { return MediaThreadStageName(static_cast<MediaThreadStage>(stage.load(std::memory_order_relaxed))); }
  int64_t MsInStage() const { return SteadyNowMs() - since_ms.load(std::memory_order_relaxed); }
};

/**
 * Joins `thread`. If it is still running after kJoinStageWarnAfter, logs the stage it is in
 * ("<context> join_<name> slow stage=… since_ms=…") and keeps waiting — never detaches: the media
 * threads use Impl, which the caller may free next.
 */
void JoinThreadWatched(std::thread& thread, const char* context, const char* name,
                       const MediaThreadStageTracker& stage) {
  if (!thread.joinable()) {
    return;
  }
  struct Done {
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
  };
  auto done = std::make_shared<Done>();
  std::thread waiter([joining = std::move(thread), done]() mutable {
    joining.join();
    {
      std::lock_guard lock(done->mu);
      done->done = true;
    }
    done->cv.notify_all();
  });
  bool finished = false;
  {
    std::unique_lock lock(done->mu);
    finished = done->cv.wait_for(lock, kJoinStageWarnAfter, [&] { return done->done; });
  }
  if (!finished) {
    EngineLog().warning << context << " join_" << name << " slow stage=" << stage.Name()
                        << " since_ms=" << stage.MsInStage();
  }
  waiter.join();
}

} // namespace

/**
 * What outlives one media session: the engine's settings and its device source. Each session holds
 * it, so a retired session cleaning up on its own thread keeps it alive.
 */
struct MediaEngine::Shared {
  std::mutex mu;
  /** Under `mu`; a session takes a copy when it starts. */
  std::function<std::unique_ptr<IVideoCodec>()> make_video_codec = CreatePlatformVideoCodec;
  /** Where devices come from: `own_devices` (constructor) or `test_devices` (skip-open); under `mu`. */
  MediaDeviceArbiter* devices = nullptr;
  MediaDeviceArbiter* own_devices = nullptr;
  std::unique_ptr<MediaDeviceArbiter> test_devices;
  /** Test fixtures: skip SDL mic/camera open (silence TX only). */
  std::atomic<bool> skip_device_open_for_test{false};
  /** Under `mu`; a session takes a copy when it starts. */
  HardwareLimits limits;
  std::atomic<int64_t> last_uplink_bps{0};
  std::atomic<int64_t> adaptation_target_video_bps{0};
  std::atomic<int64_t> adaptation_target_audio_bps{MediaTargets::kDefaultAudioBps};
  /** The level outgoing video is sent at (docs/contracts/MEDIA_CHANNELS.md). */
  std::atomic<uint8_t> video_level{mesh::kDefaultVideoLevel};
  /** Set by SetVideoLevel: the level's profile sizes the encode and sets its bitrate (broadcast). */
  std::atomic<bool> video_level_profile{false};
};

/** Sessions that retired and still clean up (join their threads, release devices) on their own threads. */
struct MediaEngine::Retirements {
  std::mutex mu;
  std::condition_variable cv;
  size_t running = 0;
};

namespace {

/**
 * The OS audio session is process-wide. A retiring session deactivates it after its threads are
 * gone; a session that started meanwhile activated it again, and that stale deactivation must not
 * undo it. So activation bumps the generation and a retiring session deactivates only if it is still
 * the one it saw when it retired.
 */
std::mutex g_audio_session_mu;
uint64_t g_audio_session_gen = 0;

} // namespace

struct MediaEngine::Impl {
  explicit Impl(std::shared_ptr<Shared> settings)
      : shared(std::move(settings)),
        last_uplink_bps(shared->last_uplink_bps),
        skip_device_open_for_test(shared->skip_device_open_for_test),
        adaptation_target_video_bps(shared->adaptation_target_video_bps),
        video_level(shared->video_level),
        video_level_profile(shared->video_level_profile),
        adaptation_target_audio_bps(shared->adaptation_target_audio_bps) {
    std::lock_guard lock(shared->mu);
    devices = shared->devices;
    // Every hardware codec runs behind a guard: a call into it waits at most its budget (L016).
    limits = shared->limits;
    make_video_codec = [make = shared->make_video_codec, budgets = limits.codecs](VideoCodecRole role) {
      return MakeGuardedVideoCodec(make, role, budgets);
    };
  }

  std::shared_ptr<Shared> shared;
  /** Stopped (or replaced by a newer session): it only cleans up now, and reports nothing. */
  std::atomic<bool> retired{false};
  /** The audio session generation when it retired (deactivate only if nothing activated since). */
  uint64_t audio_session_seen = 0;
  std::recursive_mutex mutex;
  std::string session_id;
  std::atomic<bool> active{false};
  std::atomic<bool> connected{false};
  std::atomic<bool> muted{false};
  std::atomic<bool> camera_enabled{false};
  std::atomic<bool> has_remote_video{false};
  std::atomic<bool> ever_had_remote_video{false};
  std::atomic<int64_t> started_at_ms{0};
  std::atomic<int64_t> last_remote_video_ms{0};
  std::atomic<int64_t>& last_uplink_bps;
  std::mutex video_refresh_mu;
  std::unordered_set<uint32_t> pending_video_refresh_streams;
  std::unordered_map<uint32_t, int64_t> last_video_refresh_note_ms;
  std::string connection_state = "idle";

  bool packet_mode = false;
  std::atomic<bool>& skip_device_open_for_test;
  /** Shared so SoftMigrate can replace the callback while capture/video still invoke the old one. */
  std::shared_ptr<PacketSendFn> packet_send;
  /**
   * Held around (*packet_send)(...) in capture/video threads. StartDuplex takes this after swapping the
   * callback so SoftMigrate ReleaseDirectTransport cannot Detach while an old send is mid-write
   * (Linux: malloc unaligned tcache / Aborted right after 2nd join).
   */
  std::mutex packet_send_call_mu;
  std::atomic<uint32_t> tx_audio_seq{0};
  std::atomic<uint32_t> tx_video_seq{0};
  std::atomic<bool> video_need_keyframe{false};
  std::atomic<bool> adaptation_send_video{true};
  std::atomic<int64_t>& adaptation_target_video_bps;
  std::atomic<uint8_t>& video_level;
  std::atomic<bool>& video_level_profile;

  /** The camera's geometry, sized by the level's profile when one was set. */
  CameraGeometry EncodeGeometry(CameraGeometry geometry) const {
    if (video_level_profile.load(std::memory_order_relaxed)) {
      const VideoLevelProfile profile = ProfileForVideoLevel(video_level.load(std::memory_order_relaxed));
      geometry.encode_width = profile.width;
      geometry.encode_height = profile.height;
    }
    return geometry;
  }
  std::atomic<int64_t>& adaptation_target_audio_bps;
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
  /** Where this session's devices come from (the engine's, when it started). */
  MediaDeviceArbiter* devices = nullptr;
  /**
   * Held while the session runs. Acquired / reopened by the capture thread (the only writer while it
   * runs, under `mutex`); the playout thread writes the speaker under `mutex`.
   */
  std::unique_ptr<AudioDeviceLease> mic_lease;
  std::unique_ptr<AudioDeviceLease> speaker_lease;
  /** Set for the rest of this session once voice-processing capture has starved 3 times in a row (I3):
   *  the next opens skip the voice duplex until the next StartCaptureLoop(). */
  std::atomic<bool> vpio_disabled_for_session{false};
  /** Voice-processing playout underruns of pairs already released this session; under `mutex`. */
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
  bool audio_track_cap_logged = false;
  /** First OnInboundPacket log per stream; cleared on SoftMigrate send-swap / StartDuplex. */
  std::mutex rx_log_mu;
  std::unordered_set<uint32_t> rx_logged_streams;

  /** Local encoder: created, configured, fed and bitrate-adjusted by the video thread only
   *  (reset by TearDown after that thread joined). */
  std::unique_ptr<IVideoCodec> video_codec;
  /** Local encoder + remote decoders come from here (platform HW; tests inject a stub), guarded. */
  std::function<std::unique_ptr<IVideoCodec>(VideoCodecRole)> make_video_codec;
  HardwareLimits limits;
  /** Encoders dropped for not responding since the camera opened (video thread only). */
  int encoder_restarts = 0;
  /** Decoders dropped for not responding, per stream; a stream past the cap shows no video. Under `mutex`. */
  std::unordered_map<uint32_t, int> decoder_restarts;
  /**
   * One remote stream's decoder. Decoding runs outside `mutex` (#474: a hung codec held it 1.5-5 s per step and
   * stalled playout and capture); `decode_mu` keeps one decode at a time on it (two readers can deliver a stream
   * while a path migrates). Shared: Stop or a stuck-decoder drop may take it out of the map mid-decode.
   */
  struct RemoteDecoder {
    std::mutex decode_mu;
    std::unique_ptr<IVideoCodec> codec;
  };
  std::unordered_map<uint32_t, std::shared_ptr<RemoteDecoder>> remote_decoders;
  /** Per remote video stream: when a stalled decoder is rebuilt (B125). Cleared with the decoders. */
  std::unordered_map<uint32_t, DecoderResetBudget> decoder_resets;
  std::atomic<int> video_decode_fail_logs{0};
  /** Getter waits for `mutex` over kLockWaitWarnMs logged this session (B125: is decode holding it?). */
  std::atomic<int> lock_wait_warns{0};
  static constexpr size_t kMaxRemoteVideoDecoders = 4;
  /**
   * Display rotation read on the UI thread: at SetCameraEnabled (for the open) and live via
   * UpdateCameraDisplayRotation (per-frame rotation).
   */
  std::atomic<int> camera_display_rotation{0};
  /** Facing the next camera open asks for: front at each session's start, turned over by FlipCamera. */
  std::atomic<bool> camera_prefer_front{true};
  /** FlipCamera asked the video thread to release the open camera and take the other facing. */
  std::atomic<bool> camera_reopen_requested{false};
  /** Facing of the camera the video thread last opened (CameraGeometry.front_facing). */
  std::atomic<bool> camera_front_facing{false};
  /** The video thread holds an open camera lease (false while an open is pending or after a release). */
  std::atomic<bool> camera_open{false};
  /** Wanted, but held by another holder or not responding: opened when the arbiter can grant it (L016). */
  std::atomic<bool> camera_waiting{false};
  /** Why the last requested camera did not open; taken by the UI (TakeCameraFailure). */
  std::mutex camera_failure_mu;
  std::optional<CameraFailure> camera_failure;

  std::thread capture_thread;
  std::thread video_thread;
  std::thread playout_thread;
  /** Halves of the running session; written under `mutex` in Start, read by the device thread. */
  SessionSpec spec = SessionSpec::Duplex();
  std::atomic<bool> capture_running{false};
  std::atomic<bool> video_running{false};
  std::atomic<bool> playout_running{false};
  /** Where each media thread is, for a join that does not return (B128). */
  MediaThreadStageTracker capture_stage;
  MediaThreadStageTracker playout_stage;
  MediaThreadStageTracker video_stage;
  /** Capture worker closes+reopens SDL devices (speaker route / SoftMigrate). */
  std::atomic<bool> audio_reopen_requested{false};
  /**
   * A device the spec wants was refused (held by another holder, or the audio devices are not
   * responding): the capture thread takes the leases again when the arbiter can grant them (L016).
   */
  std::atomic<bool> audio_waiting{false};
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
   *  already hold `mutex`, and callbacks (ActiveSessionId / SFU migrate) re-enter the engine. */
  void ApplyStateLocked(const std::string& state) {
    connection_state = state;
    const bool now_connected = (state == "connected");
    connected = now_connected;
    if (state == "failed" || state == "closed") {
      ClearRemoteVideoFrames();
    }
  }

  void PublishLocalPreview(const VideoFrameRgba& frame, bool mirror) {
    VideoFrameRgba copy = frame;
    if (mirror) {
      MirrorRgbaInPlace(copy);
    }
    PremultiplyRgbaInPlace(copy.rgba);
    std::lock_guard lock(video_frame_mutex);
    local_video_frame.width = copy.width;
    local_video_frame.height = copy.height;
    local_video_frame.rgba = std::move(copy.rgba);
    local_video_frame.seq = ++local_video_seq;
  }

  /**
   * A public getter's hold on `mutex` (B125: the UI thread polls several every frame; does decode
   * keep it waiting?). Times the wait; a long one is logged after the lock is released.
   */
  class TimedGetterLock {
  public:
    TimedGetterLock(Impl& impl, const char* getter)
        : impl_(impl),
          getter_(getter),
          since_(std::chrono::steady_clock::now()),
          lock_(impl.mutex),
          waited_ms_(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since_)
                         .count()) {}
    ~TimedGetterLock() {
      if (waited_ms_ <= kLockWaitWarnMs || impl_.lock_wait_warns.fetch_add(1) >= kMaxLockWaitWarns) {
        return;
      }
      const std::string session = impl_.session_id;
      lock_.unlock();
      EngineLog().warning << "media lock wait getter=" << getter_ << " waited_ms=" << waited_ms_
                          << " session=" << session;
    }
    TimedGetterLock(const TimedGetterLock&) = delete;
    TimedGetterLock& operator=(const TimedGetterLock&) = delete;

  private:
    Impl& impl_;
    const char* getter_;
    std::chrono::steady_clock::time_point since_;
    std::unique_lock<std::recursive_mutex> lock_;
    int64_t waited_ms_;
  };

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
    camera_waiting.store(false, std::memory_order_relaxed);
    camera_reopen_requested.store(false, std::memory_order_relaxed);  // a flip in flight dies with the camera
  }

  void ClearLocalPreview() {
    std::lock_guard lock(video_frame_mutex);
    local_video_frame = {};
    local_video_frame.seq = ++local_video_seq;
  }

  void NoteCameraFailure(const CameraFailure::Kind kind, const std::string& detail) {
    std::lock_guard lock(camera_failure_mu);
    camera_failure = CameraFailure{.kind = kind, .detail = detail};
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

  /** The audio half: device leases, the Opus encoder, remote tracks, counters. Not the OS session. */
  void ReleaseAudioLocked() {
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
  }

  /** OS audio session ownership (g_audio_session_gen): never under `mutex`. */
  static void ActivateAudioSession() {
    std::lock_guard lock(g_audio_session_mu);
    ++g_audio_session_gen;
    AudioSession::ActivateForVoip();
  }
  static uint64_t SessionGeneration() {
    std::lock_guard lock(g_audio_session_mu);
    return g_audio_session_gen;
  }
  /** A retired session: deactivate the OS session unless a session activated it since `seen_gen`. */
  static void ReleaseAudioSessionIfUnchanged(const uint64_t seen_gen) {
    std::lock_guard lock(g_audio_session_mu);
    if (g_audio_session_gen == seen_gen) {
      ReleaseAudioSession();
    }
  }

  /** Touches the OS audio session only, not Impl. */
  static void ReleaseAudioSession() {
    AudioSession::Deactivate();
    AudioSession::ClearCaptureAudioHints();
  }

  /** `context` names the caller in the slow-join warning ("media stop", "media restart"). */
  void JoinCaptureThread(const char* context) {
    capture_running = false;
    JoinThreadWatched(capture_thread, context, "capture", capture_stage);
  }

  void JoinPlayoutThread(const char* context) {
    playout_running = false;
    JoinThreadWatched(playout_thread, context, "playout", playout_stage);
  }

  /**
   * One playout slot for one track: decode Packet / FEC-on-Gap / PLC-on-Empty into `mix`. When the
   * buffer is over its adaptive target, a first-pass decoded Packet that turns out to be silence is
   * dropped and a second frame is popped/decoded to catch up by one slot without an audible gap
   * (adaptive jitter §2).
   */
  void PopAndDecodeTrackLocked(RemoteAudioTrack& track, std::vector<int16_t>& mix, bool& any) {
    if (!track.decoder) {
      return;
    }
    std::vector<int16_t> pcm(static_cast<size_t>(kFrameSamples), 0);
    const auto plc = [&]() {
      const int n = opus_decode(track.decoder, nullptr, 0, pcm.data(), kFrameSamples, 0);
      if (n > 0) {
        plc_frames_total.fetch_add(1, std::memory_order_relaxed);
      }
      return n;
    };
    int decoded = 0;
    for (int pass = 0; pass < 2; ++pass) {
      const uint64_t underruns_before = track.jitter.underruns();
      AudioPlayoutPop pop = track.jitter.PopForPlayout();
      decoded = 0;
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
      if (pass == 0 && pop.kind == AudioPlayoutPop::Kind::Packet && decoded > 0 && track.jitter.OverTarget() &&
          IsCatchUpSilence(pcm, static_cast<size_t>(decoded))) {
        track.jitter.NoteSilenceDrop();
        continue; // catch up: pop and decode the next frame instead of playing this silent one
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
    playout_stage.Set(MediaThreadStage::Idle);
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
          playout_stage.Set(MediaThreadStage::LockMedia);
          std::lock_guard lock(mutex);
          playout_stage.Set(MediaThreadStage::Mix);
          AudioDeviceLease* out = speaker_lease && speaker_lease->HasDevice() ? speaker_lease.get() : nullptr;
          if (t0 - last_diag >= std::chrono::seconds(2)) {
            last_diag = t0;
            if (out) {
              const int queued = out->Queued();
              (void)out->WithVoiceProcessing([&](IVoiceProcessing& vp) {
                SDL_Log("MediaEngine: vpio_diag tick_gap_max_ms=%lld queued_bytes=%d %s",
                        static_cast<long long>(tick_gap_max_ms), queued, vp.TakeDiag().c_str());
              });
            }
            tick_gap_max_ms = 0;
          }
          int slots = out ? PlayoutSlotsLocked(*out) : 1;
          const bool voice_out = out && out->WithVoiceProcessing([](IVoiceProcessing&) {});
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
              if (voice_out) {
                ApplySoftGain(mix, kVoicePlayoutGain);
              }
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
        playout_stage.Set(MediaThreadStage::Sleep);
        if (elapsed < period) {
          std::this_thread::sleep_for(period - elapsed);
        }
      }
      playout_stage.Set(MediaThreadStage::Exiting);
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
    opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(AudioSession::OpusEncoderComplexity()));
    return {};
  }

  RemoteAudioTrack* EnsureRemoteTrackLocked(uint32_t stream_id) {
    auto it = audio_tracks.find(stream_id);
    if (it != audio_tracks.end() && it->second) {
      return it->second.get();
    }
    if (audio_tracks.size() >= MediaEngine::kMaxRemoteAudioTracks) {
      if (!audio_track_cap_logged) {
        audio_track_cap_logged = true;
        EngineLog().warning << "remote audio stream " << stream_id << " dropped: already " << audio_tracks.size()
                            << " tracks (cap " << MediaEngine::kMaxRemoteAudioTracks << ")";
      }
      return nullptr;
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
      SDL_Log("MediaEngine: %s refused: %s", MediaDeviceKindName(kind), lease.error().message.c_str());
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
      holder = session_id;
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
      AudioSession::WaitUntilToneHoldsReleased();
      if (!capture_running.load(std::memory_order_acquire)) {
        return Error("media session stopped");
      }
      AudioSession::ApplyCaptureAudioHints();
      ActivateAudioSession();
    }
    if (!reopen && want_spec.capture && !mic) {
      WaitForMicHandover();
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
      return Error("media session stopped"); // leases release on scope exit
    }
    const bool voice = mic && mic->IsVoiceDuplex();
    if (voip && !voice) {
      // SDL's coreaudio backend rewrites the AVAudioSession category/options with ModeDefault and
      // DefaultToSpeaker when it opens devices (B36). Re-apply our VoiceChat mode + earpiece route.
      ActivateAudioSession();
    }
    if (speaker && !speaker->HasDevice()) {
      // Headless CI / no default device: still run silence TX (same as no-capture path).
      SDL_Log("MediaEngine: no playback device — RX muted; capture/silence TX still active: %s",
              speaker->OpenError().c_str());
    }
    std::lock_guard lock(mutex);
    mic_lease = std::move(mic);
    speaker_lease = std::move(speaker);
    capture_available = mic_lease && mic_lease->HasDevice();
    audio_waiting.store((want_spec.capture && !mic_lease) || (want_spec.playback && !speaker_lease),
                        std::memory_order_release);
#ifdef PP_BROWSER_CALL_DENOISE
    denoise.Reset();
#endif
    return {};
  }

  /**
   * The session before this one (a call that just ended, or the one this replaced) may still be
   * letting the mic go on its own thread: give it a moment, so this session gets the mic — and the
   * voice-processing pair — at its first open rather than at a retry.
   */
  void WaitForMicHandover() {
    const auto deadline = std::chrono::steady_clock::now() + kMicHandoverWait;
    while (!DeviceArbiter().CanAcquire(MediaDeviceKind::Mic) && capture_running.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  /** The devices a waiting session was refused can be granted now. */
  bool WaitedAudioGrantable() {
    SessionSpec want_spec;
    bool need_mic = false;
    bool need_speaker = false;
    {
      std::lock_guard lock(mutex);
      want_spec = spec;
      need_mic = want_spec.capture && !mic_lease;
      need_speaker = want_spec.playback && !speaker_lease;
    }
    return (!need_mic || DeviceArbiter().CanAcquire(MediaDeviceKind::Mic)) &&
           (!need_speaker || DeviceArbiter().CanAcquire(MediaDeviceKind::Speaker));
  }

  /**
   * Duplex calls take mic + speaker from one OS voice-processing unit (echo cancellation) when the
   * backend has one (Apple VPIO on macOS / iOS; other platforms say "unsupported"); otherwise — or
   * once disabled for this call — separate leases follow.
   */
  void TryAcquireVoiceDuplex(const SessionSpec& want_spec, const std::string& holder,
                             std::unique_ptr<AudioDeviceLease>& mic, std::unique_ptr<AudioDeviceLease>& speaker) {
    if (!want_spec.capture || !want_spec.playback || mic || speaker ||
        skip_device_open_for_test.load(std::memory_order_relaxed) || !capture_running.load(std::memory_order_acquire)) {
      return;
    }
    if (vpio_disabled_for_session.load(std::memory_order_acquire)) {
      EngineLog().info << "audio_io=sdl reason=vpio disabled for call (capture starved)";
      return;
    }
    AudioLeaseRequest request;
    request.holder = holder;
    request.format = AudioDeviceFormat{kSampleRate, kChannels};
    request.still_wanted = [this]() { return capture_running.load(std::memory_order_acquire); };
    auto pair = DeviceArbiter().AcquireVoiceDuplex(request);
    if (!pair) {
      EngineLog().info << "audio_io=sdl reason=" << pair.error().message;
      return;
    }
    EngineLog().info << "audio_io=vpio (voice processing: echo cancellation on)";
    mic = std::move(pair->mic);
    speaker = std::move(pair->speaker);
  }

  void StartCaptureLoop() {
    // Precondition: capture_thread not joinable (JoinCaptureThread outside media mutex).
    capture_running = true;
    audio_reopen_requested.store(false, std::memory_order_relaxed);
    vpio_disabled_for_session.store(false, std::memory_order_relaxed);
    capture_stage.Set(MediaThreadStage::Idle);
    capture_thread = std::thread([this]() {
      // Device open (and OS mic prompts) stay on this worker so the session's user (a call's accept)
      // can finish its signaling without freezing UI or the mesh.
      capture_stage.Set(MediaThreadStage::OpenDevices);
      if (auto audio = EnsureAudioLeases(/*reopen=*/false); !audio) {
        SDL_Log("MediaEngine: audio devices: %s", audio.error().message.c_str());
      } else if (!capture_available) {
        SDL_Log("MediaEngine: started without capture device — sending silence; playback still active");
      }
      if (!capture_running.load()) {
        capture_stage.Set(MediaThreadStage::Exiting);
        return;
      }

      std::vector<int16_t> pcm(static_cast<size_t>(kFrameSamples));
      std::vector<int16_t> pending;
      pending.reserve(static_cast<size_t>(kFrameSamples) * 2);
      std::vector<unsigned char> opus_buf(4000);
      int64_t last_capture_pcm_ms = util::NowUnixMs();
      // Opus encoders are not thread-safe: bitrate changes (ApplyTargets, any thread) are
      // applied here, on the thread that encodes, right before the next frame.
      OpusEncoder* bitrate_enc = nullptr;
      int64_t applied_audio_bps = 0;
      int64_t last_capture_starve_reopen_ms = 0;
      // When the audio devices were last (re)opened: voice processing gets a warm-up (B56).
      int64_t capture_opened_ms = last_capture_pcm_ms;
      // I3: consecutive starvation-triggered reopens while on voice processing; 3 in a row falls
      // back to SDL for the rest of this call. Capture-thread-local.
      int vpio_starve_reopens = 0;
      int64_t last_audio_retry_ms = 0;
      while (capture_running.load()) {
        if (audio_waiting.load(std::memory_order_acquire)) {
          const int64_t now = util::NowUnixMs();
          if (now - last_audio_retry_ms >= kDeviceRetryPoll.count() && WaitedAudioGrantable()) {
            last_audio_retry_ms = now;
            // All of it again (not just the missing half), so a duplex call can get its voice pair.
            pending.clear();
            SDL_Log("MediaEngine: audio devices free again — taking them");
            capture_stage.Set(MediaThreadStage::OpenDevices);
            {
              std::lock_guard lock(mutex);
              CloseAudioDevicesLocked();
            }
            if (auto audio = EnsureAudioLeases(/*reopen=*/false); !audio) {
              SDL_Log("MediaEngine: audio devices: %s", audio.error().message.c_str());
            }
            last_capture_pcm_ms = util::NowUnixMs();
            capture_opened_ms = last_capture_pcm_ms;
          } else if (now - last_audio_retry_ms >= kDeviceRetryPoll.count()) {
            last_audio_retry_ms = now;
          }
        }
        if (audio_reopen_requested.exchange(false, std::memory_order_acq_rel)) {
          // Android speakerphone / SoftMigrate can leave AudioRecord feeding zeros until reopen.
          // Android settle (Moto safety ramp) is the backend's ReopenSettle, on the device thread.
          pending.clear();
          SDL_Log("MediaEngine: reopening audio devices (speaker route / SoftMigrate)");
          capture_stage.Set(MediaThreadStage::OpenDevices);
          if (auto audio = EnsureAudioLeases(/*reopen=*/true); !audio) {
            SDL_Log("MediaEngine: audio reopen failed: %s", audio.error().message.c_str());
          } else if (!capture_available) {
            SDL_Log("MediaEngine: audio reopen — no capture device; sending silence");
          }
          last_capture_pcm_ms = util::NowUnixMs();
          capture_opened_ms = last_capture_pcm_ms;
        }
        // The capture thread is mic_lease's only writer while it runs: read it without `mutex`.
        bool device_changed = false;
        const bool vpio_on = capture_available && mic_lease &&
                             mic_lease->WithVoiceProcessing([&](IVoiceProcessing& vp) { device_changed = vp.TakeDeviceChanged(); });
        if (device_changed) {
          SDL_Log("MediaEngine: default audio device changed — reopening voice processing");
          audio_reopen_requested.store(true, std::memory_order_release);
          continue;
        }
        std::shared_ptr<PacketSendFn> send_fn;
        OpusEncoder* enc = nullptr;
        bool can_send = false;
        bool capturing = true;
        {
          capture_stage.Set(MediaThreadStage::LockMedia);
          std::lock_guard lock(mutex);
          capturing = spec.capture;
          enc = encoder;
          can_send = capturing && packet_mode && static_cast<bool>(packet_send) && enc;
          if (can_send) {
            send_fn = packet_send;
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
          capture_stage.Set(MediaThreadStage::ReadMic);
          const int got = mic_lease->Read(chunk, static_cast<int>(sizeof(chunk)));
          if (got > 0) {
            const size_t samples = static_cast<size_t>(got) / sizeof(int16_t);
            pending.insert(pending.end(), chunk, chunk + samples);
            last_capture_pcm_ms = util::NowUnixMs();
            // Not on the first PCM after a reopen: a unit that starts and stalls again is still
            // starving "in a row" — otherwise the SDL fallback below never triggered (B56).
            if (vpio_on && CaptureStarvePolicy::HealthyAfterOpen(last_capture_pcm_ms - capture_opened_ms)) {
              vpio_starve_reopens = 0;
            }
          }
          if (pending.size() < static_cast<size_t>(kFrameSamples)) {
            const int64_t now = util::NowUnixMs();
            // Wedged capture (got<=0 after AAudio disconnect) used to spin without TX —
            // peers saw "Your mic isn't sending". After ~500ms, encode silence and reopen (voice
            // processing: only once it had time to warm up — CaptureStarvePolicy).
            if (got <= 0 && (now - last_capture_pcm_ms) >= CaptureStarvePolicy::kSilenceAfterMs) {
              if (!muted.load(std::memory_order_relaxed)) {
                SmoothLevel(local_input_level, 0.f);
              }
              const int64_t reopen_after_ms = CaptureStarvePolicy::ReopenAfterMs(vpio_on, now - capture_opened_ms);
              if (now - last_capture_pcm_ms >= reopen_after_ms && now - last_capture_starve_reopen_ms > 2000) {
                last_capture_starve_reopen_ms = now;
                SDL_Log("MediaEngine: capture starved %lldms — requesting reopen",
                        static_cast<long long>(now - last_capture_pcm_ms));
                audio_reopen_requested.store(true, std::memory_order_release);
                // I3: persistent voice-processing capture starvation (not just a starved SDL
                // device) falls back to SDL for the rest of this call after 3 reopens in a row.
                if (vpio_on && ++vpio_starve_reopens >= CaptureStarvePolicy::kVoiceMaxReopens) {
                  vpio_disabled_for_session.store(true, std::memory_order_release);
                  SDL_Log("MediaEngine: vpio capture starved 3x — using SDL for this call");
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
                  SDL_Log("MediaEngine: noise_floor_dbfs=%.1f", denoise.last_noise_floor_dbfs());
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
        capture_stage.Set(MediaThreadStage::Encode);
        const int encoded =
            opus_encode(enc, pcm.data(), kFrameSamples, opus_buf.data(), static_cast<int>(opus_buf.size()));
        if (encoded <= 0) {
          continue;
        }
        if (send_fn) {
          MediaPacket pkt;
          pkt.channel_id = mesh::kMediaChannelAudio;
          pkt.seq = tx_audio_seq.fetch_add(1) + 1;
          pkt.payload.assign(opus_buf.data(), opus_buf.data() + encoded);
          try {
            capture_stage.Set(MediaThreadStage::Send);
            std::lock_guard send_lock(packet_send_call_mu);
            if (!retired.load(std::memory_order_acquire)) { // a retired session never sends (Retire drains)
              (*send_fn)(pkt);
              tx_audio_frames.fetch_add(1, std::memory_order_relaxed);
              last_tx_audio_ms.store(util::NowUnixMs(), std::memory_order_relaxed);
            }
          } catch (...) {
          }
        }
        // Pace silence / starved-capture TX to one Opus frame per period.
        if (paced_silence_frame) {
          capture_stage.Set(MediaThreadStage::Sleep);
          std::this_thread::sleep_for(std::chrono::milliseconds(kFrameMs));
        }
      }
      capture_stage.Set(MediaThreadStage::Exiting);
    });
  }

  void StartVideoLoop() {
    video_running = true;
    video_stage.Set(MediaThreadStage::Idle);
    video_thread = std::thread([this]() { RunVideoLoop(); });
  }

  void JoinVideoThread(const char* context) {
    video_running = false;
    JoinThreadWatched(video_thread, context, "video", video_stage);
  }

  /**
   * Video thread only: take the camera lease (open runs on the device thread). A failed open drops
   * the request and records why; a request withdrawn while opening releases the lease again.
   */
  std::unique_ptr<CameraDeviceLease> OpenCameraLease() {
    CameraLeaseRequest request;
    {
      std::lock_guard lock(mutex);
      request.holder = session_id;
    }
    request.format.display_rotation_deg = camera_display_rotation.load(std::memory_order_relaxed);
    request.format.fps = kVideoFps;
    request.format.prefer_front_facing = camera_prefer_front.load(std::memory_order_relaxed);
    auto lease = DeviceArbiter().AcquireCamera(request);
    if (!lease && !DeviceArbiter().CanAcquire(MediaDeviceKind::Camera)) {
      // Held by another holder, or the camera is not responding (stuck on an earlier close): stay
      // wanted and open when it is free again — told once.
      if (CameraStuckTooLong()) {
        GiveUpCamera(kCameraStuckMessage);
        return nullptr;
      }
      if (!camera_waiting.exchange(true, std::memory_order_acq_rel)) {
        EngineLog().info << "camera waiting for the device: " << lease.error().message;
        NoteCameraFailure(CameraFailure::Kind::Busy, kCameraBusyMessage);
      }
      return nullptr;
    }
    camera_waiting.store(false, std::memory_order_relaxed);
    if (!lease) {
      camera_enabled.store(false, std::memory_order_relaxed);
      camera_prefer_front.store(true, std::memory_order_relaxed);  // a failed facing is not asked for again
      NoteCameraFailure(CameraFailure::Kind::Failed, lease.error().message);
      SDL_Log("MediaEngine: camera: %s", lease.error().message.c_str());
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
      video_codec = make_video_codec(VideoCodecRole::Encoder);
    }
    if (!video_codec) {
      return;
    }
    if (auto cfg = video_codec->ConfigureEncoder(geometry.encode_width, geometry.encode_height, kVideoFps); !cfg) {
      SDL_Log("MediaEngine: video encode unavailable: %s — local preview may still work; voice continues",
              cfg.error().message.c_str());
    }
  }

  /** Video thread only: bitrate changes (ApplyTargets, any thread) land right before an encode. */
  void ApplyVideoBitrate(int64_t& applied_bps) {
    int64_t want = adaptation_target_video_bps.load(std::memory_order_relaxed);
    if (want <= 0 && video_level_profile.load(std::memory_order_relaxed)) {
      want = ProfileForVideoLevel(video_level.load(std::memory_order_relaxed)).target_bps;
    }
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

  /** Video thread only: orient + crop → preview (mirrored like FaceTime / WhatsApp self-view, Kenneth
   *  2026-11-03 "视频显示自己是反的"); encode the un-mirrored frame → send on channel 1. */
  void EncodeAndSend(VideoFrameRgba captured, const CameraGeometry& geometry, bool& need_keyframe) {
    // Rotation follows the display turning mid-call (iOS: the phone's physical orientation); the
    // encode size stays the one chosen at open — ScaleCenterCropRgba cover-crops the rest.
    const CameraCaptureTransform opened{.rotate_cw = geometry.rotate_cw,
                                        .encode_width = geometry.encode_width,
                                        .encode_height = geometry.encode_height,
                                        .front_facing = geometry.front_facing};
    const int rotate_cw = CameraFrameRotateCw(opened, camera_display_rotation.load(std::memory_order_relaxed));
    // The local preview is drawn on this device's screen, which turns with the phone: keep it
    // upright relative to the screen (only differs from the sent frame when the phone is turned).
    const int preview_rotate_cw = CameraPreviewRotateCw(opened);
    if (preview_rotate_cw != rotate_cw) {
      VideoFrameRgba preview_fitted;
      if (ScaleCenterCropRgba(OrientFrame(captured, preview_rotate_cw), geometry.encode_width,
                              geometry.encode_height, preview_fitted)) {
        PublishLocalPreview(preview_fitted, geometry.mirror_preview);
      }
    }
    const VideoFrameRgba oriented = OrientFrame(std::move(captured), rotate_cw);
    VideoFrameRgba fitted;
    VideoFrameI420 i420;
    if (!ScaleCenterCropRgba(oriented, geometry.encode_width, geometry.encode_height, fitted) ||
        !RgbaToI420(fitted.rgba.data(), fitted.width, fitted.height, fitted.width * 4, true, i420)) {
      return;
    }
    if (preview_rotate_cw == rotate_cw) {
      PublishLocalPreview(fitted, geometry.mirror_preview);
    }
    if (!adaptation_send_video.load(std::memory_order_relaxed)) {
      need_keyframe = true; // the path cannot carry video now: nothing goes out; a keyframe starts it again
      return;
    }
    if (!video_codec || !video_codec->HasEncoder()) {
      return;
    }
    video_stage.Set(MediaThreadStage::Encode);
    auto encoded = video_codec->Encode(i420, need_keyframe);
    if (!encoded || encoded->annex_b.empty()) {
      return;
    }
    need_keyframe = false;
    std::shared_ptr<PacketSendFn> send_fn;
    {
      video_stage.Set(MediaThreadStage::LockMedia);
      std::lock_guard lock(mutex);
      if (packet_mode && packet_send) {
        send_fn = packet_send;
      }
    }
    if (!send_fn) {
      return;
    }
    MediaPacket pkt;
    pkt.channel_id = mesh::VideoChannel(video_level.load(std::memory_order_relaxed));
    pkt.seq = tx_video_seq.fetch_add(1) + 1;
    pkt.mark = encoded->keyframe ? 1 : 0;
    pkt.payload = std::move(encoded->annex_b);
    try {
      video_stage.Set(MediaThreadStage::Send);
      std::lock_guard send_lock(packet_send_call_mu);
      if (!retired.load(std::memory_order_acquire)) { // a retired session never sends (Retire drains)
        (*send_fn)(pkt);
        tx_video_frames.fetch_add(1, std::memory_order_relaxed);
      }
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
    // Camera start timing (video start took 4-6 s on device, 2026-09-29): request → lease (device
    // thread queue + open) → first frame. Steady clock: an NTP step must not skew it (cf. M6).
    std::optional<std::chrono::steady_clock::time_point> camera_requested;
    int64_t camera_lease_ms = 0;
    auto ms_since = [](std::chrono::steady_clock::time_point t) {
      return static_cast<int64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t).count());
    };
    while (video_running.load()) {
      if (video_need_keyframe.exchange(false, std::memory_order_acq_rel)) {
        need_keyframe = true;
      }
      const bool wanted = camera_enabled.load(std::memory_order_relaxed);
      if (!wanted) {
        camera_requested.reset();
      }
      // FlipCamera: release the open camera like a turn-off (the preview keeps its last frame);
      // the open below takes the other facing. A flip pending when the camera is off is dropped.
      if (camera_reopen_requested.exchange(false, std::memory_order_acq_rel) && wanted && camera) {
        video_stage.Set(MediaThreadStage::CloseCamera);
        camera.reset();
        camera_open.store(false, std::memory_order_release);
        camera_requested.reset();  // the open below is timed on its own (camera.start lease_ms)
        if (video_codec) {
          video_stage.Set(MediaThreadStage::ResetEncoder);
          video_codec->ResetEncoder();
        }
      }
      if (wanted && !camera && camera_waiting.load(std::memory_order_acquire) &&
          !DeviceArbiter().CanAcquire(MediaDeviceKind::Camera)) {
        video_stage.Set(MediaThreadStage::WaitCamera);
        if (CameraStuckTooLong()) {
          GiveUpCamera(kCameraStuckMessage);
          continue;
        }
        std::this_thread::sleep_for(kDeviceRetryPoll);
        continue;
      }
      if (wanted && !camera) {
        if (!camera_requested) {
          camera_requested = std::chrono::steady_clock::now();
        }
        video_stage.Set(MediaThreadStage::OpenCamera);
        camera = OpenCameraLease();
        if (camera) {
          camera_lease_ms = ms_since(*camera_requested);
          geometry = EncodeGeometry(camera->Geometry());
          camera_front_facing.store(geometry.front_facing, std::memory_order_relaxed);
          camera_open.store(true, std::memory_order_release);
          video_stage.Set(MediaThreadStage::ConfigureEncoder);
          encoder_restarts = 0; // each camera request gets its own replacements
          ConfigureLocalEncoder(geometry);
          (void)DropStuckEncoder();
          applied_bps = 0;
          need_keyframe = true;
        }
      } else if (!wanted && camera) {
        video_stage.Set(MediaThreadStage::CloseCamera);
        camera.reset();
        camera_open.store(false, std::memory_order_release);
        if (video_codec) {
          video_stage.Set(MediaThreadStage::ResetEncoder);
          video_codec->ResetEncoder();
        }
        ClearLocalPreview();
      }
      if (!camera) {
        video_stage.Set(MediaThreadStage::WaitCamera);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        continue;
      }
      const auto t0 = std::chrono::steady_clock::now();
      if (!video_codec && encoder_restarts > 0) {
        // The last encoder stopped responding: a new one, and an IDR to start its stream.
        video_stage.Set(MediaThreadStage::ConfigureEncoder);
        ConfigureLocalEncoder(geometry);
        applied_bps = 0;
        need_keyframe = true;
        if (DropStuckEncoder()) {
          continue;
        }
      }
      video_stage.Set(MediaThreadStage::SetBitrate);
      ApplyVideoBitrate(applied_bps);
      if (DropStuckEncoder()) {
        continue;
      }
      video_stage.Set(MediaThreadStage::WaitFrame);
      auto frame = camera->NextFrame();
      if (!frame) {
        if (camera->PermissionDenied()) {
          // No frame will ever come: say so, with where to allow it, instead of a camera that is
          // "on" and black (B147, a phone that never showed the prompt again). The next turn of the
          // loop closes the lease like the person turning video off.
          EngineLog().info << "camera permission denied by the OS";
          camera_enabled.store(false, std::memory_order_relaxed);
          NoteCameraFailure(CameraFailure::Kind::PermissionDenied, "camera permission denied");
          continue;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        continue;
      }
      if (camera_requested) {
        MetricsLine("camera.start").Add("lease_ms", camera_lease_ms).Add("first_frame_ms", ms_since(*camera_requested)).Emit();
        camera_requested.reset();
      }
      video_stage.Set(MediaThreadStage::Convert);
      EncodeAndSend(std::move(*frame), geometry, need_keyframe);
      (void)DropStuckEncoder();
      const auto elapsed = std::chrono::steady_clock::now() - t0;
      video_stage.Set(MediaThreadStage::Sleep);
      if (elapsed < frame_period) {
        std::this_thread::sleep_for(frame_period - elapsed);
      }
    }
    // Leaving the loop closes the camera (the lease's destructor, device thread) — the step a stop
    // waits on here.
    video_stage.Set(MediaThreadStage::Exiting);
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
    // Monotonic clock for the jitter estimator (AudioPacket.recv_ms): a wall-clock (NTP) step
    // would otherwise look like packet lateness (M6). rx_age / health still compare wall clock.
    const int64_t recv_mono_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now().time_since_epoch())
                                      .count();
    ++track->rx_frames;
    track->last_rx_ms = recv_ms;
    AudioPacket packet;
    packet.seq = seq;
    packet.recv_ms = recv_mono_ms;
    packet.payload.assign(reinterpret_cast<const uint8_t*>(data), reinterpret_cast<const uint8_t*>(data) + size);
    track->jitter.Push(std::move(packet));
    rx_audio_frames.fetch_add(1, std::memory_order_relaxed);
    last_rx_audio_ms.store(recv_ms, std::memory_order_relaxed);
  }

  /** Under `mutex`: the stream's decoder, made on its first packet; none past the limits. */
  std::shared_ptr<RemoteDecoder> RemoteDecoderLocked(uint32_t stream_id) {
    if (auto it = remote_decoders.find(stream_id); it != remote_decoders.end()) {
      return it->second;
    }
    if (remote_decoders.size() >= kMaxRemoteVideoDecoders) {
      return nullptr;
    }
    if (const auto restarts = decoder_restarts.find(stream_id);
        restarts != decoder_restarts.end() && restarts->second > kMaxCodecRestarts) {
      return nullptr; // its decoders kept hanging: no video from this stream this call
    }
    auto created = make_video_codec(VideoCodecRole::Decoder);
    if (!created) {
      return nullptr;
    }
    auto decoder = std::make_shared<RemoteDecoder>();
    decoder->codec = std::move(created);
    remote_decoders.emplace(stream_id, decoder);
    return decoder;
  }

  /** Under `mutex`: whether `decoder` is still the stream's (Stop / a drop / a migration did not replace it). */
  bool StillTheDecoderLocked(uint32_t stream_id, const std::shared_ptr<RemoteDecoder>& decoder) const {
    auto it = remote_decoders.find(stream_id);
    return it != remote_decoders.end() && it->second == decoder;
  }

  /** Not under `mutex` (#474): one access unit through the stream's decoder, the frame to the screen. */
  void DecodeRemoteVideo(uint32_t stream_id, const std::shared_ptr<RemoteDecoder>& decoder, const std::byte* data,
                         size_t size) {
    std::lock_guard decode(decoder->decode_mu);
    IVideoCodec& codec = *decoder->codec;
    if (!codec.HasDecoder()) {
      if (auto configured = codec.ConfigureDecoder(); !configured) {
        if (codec.NotResponding()) {
          DropStuckDecoder(stream_id, decoder);
          return;
        }
        static std::atomic<bool> logged_cfg{false};
        if (!logged_cfg.exchange(true)) {
          SDL_Log("MediaEngine: ConfigureDecoder failed: %s", configured.error().message.c_str());
        }
        return;
      }
    }
    auto decoded = codec.Decode(reinterpret_cast<const uint8_t*>(data), size);
    if (!decoded && codec.NotResponding()) {
      DropStuckDecoder(stream_id, decoder);
      return;
    }
    if (!decoded) {
      // In the app log (SDL_Log never reaches the phone's log file), a few per call.
      if (video_decode_fail_logs.fetch_add(1, std::memory_order_relaxed) < 5) {
        EngineLog().warning << "remote H264 decode failed stream=" << stream_id << " size=" << size << ": "
                            << decoded.error().message;
      }
      NoteVideoRefreshNeeded(stream_id);
      if (decoded.error().message == kDecoderStalledError) {
        ResetStalledDecoder(stream_id, decoder);
      }
      return;
    }
    if (!decoded->has_value()) {
      // No frame to show yet (decoder filling its pipeline / waiting for an IDR): not an error.
      return;
    }
    {
      std::lock_guard lock(mutex);
      // Stopped, or the stream's decoder replaced while this one decoded: the frame is not this session's to show.
      if (!active || !StillTheDecoderLocked(stream_id, decoder)) {
        return;
      }
      if (auto it = decoder_resets.find(stream_id); it != decoder_resets.end()) {
        it->second.NoteFrame();
      }
    }
    PublishRemoteFrame(stream_id, std::move(**decoded));
  }

  /**
   * A decoder that stopped responding (L016) is dropped — destroyed on its own thread when it lets
   * go — and the next packet makes a new one; it asks the sender for an IDR. Past kMaxCodecRestarts
   * the stream shows no video for the rest of the call.
   */
  void DropStuckDecoder(uint32_t stream_id, const std::shared_ptr<RemoteDecoder>& decoder) {
    std::lock_guard lock(mutex);
    if (!StillTheDecoderLocked(stream_id, decoder)) {
      return; // already dropped (or replaced): counted once
    }
    remote_decoders.erase(stream_id);
    decoder_resets.erase(stream_id);
    const int restarts = ++decoder_restarts[stream_id];
    EngineLog().warning << "remote H264 decoder not responding; dropped stream=" << stream_id
                        << " restarts=" << restarts << (restarts > kMaxCodecRestarts ? " (no more this call)" : "")
                        << " session=" << session_id;
    NoteVideoRefreshNeeded(stream_id);
  }

  /**
   * Video thread: the local encoder stopped responding (L016) — drop it (destroyed on its own thread
   * when it lets go); the next frame makes a new one. Past kMaxCodecRestarts the camera is given up
   * on. True when it was dropped.
   */
  bool DropStuckEncoder() {
    if (!video_codec || !video_codec->NotResponding()) {
      return false;
    }
    video_codec.reset();
    ++encoder_restarts;
    EngineLog().warning << "video encoder not responding; dropped restarts=" << encoder_restarts;
    if (encoder_restarts > kMaxCodecRestarts) {
      GiveUpCamera(kEncoderStuckMessage);
    }
    return true;
  }

  /** The camera cannot work this session: off, the person told why (and the roster withdrawn). */
  void GiveUpCamera(const char* message) {
    EngineLog().warning << "camera given up: " << message;
    camera_enabled.store(false, std::memory_order_relaxed);
    camera_waiting.store(false, std::memory_order_relaxed);
    NoteCameraFailure(CameraFailure::Kind::NotResponding, message);
  }

  /** A camera that waits for its device gives up once that device has been stuck device_give_up_after. */
  bool CameraStuckTooLong() {
    return DeviceArbiter().Health(MediaDeviceKind::Camera).stuck_ms >= limits.device_give_up_after.count();
  }

  /**
   * B125: a decoder that stays stalled is rebuilt — two stalls in a row, at most twice per call, ten
   * seconds apart. The budget is kept under the media lock; the rebuild runs under the decoder's own
   * (the caller holds it), not the media lock. The next packet configures it again; it shows nothing
   * until an IDR, which the stall has already asked for.
   */
  void ResetStalledDecoder(uint32_t stream_id, const std::shared_ptr<RemoteDecoder>& decoder) {
    const auto started = std::chrono::steady_clock::now();
    int resets = 0;
    bool exhausted = false;
    {
      std::lock_guard lock(mutex);
      // Stop / SoftMigrate may have dropped the decoder (and the budgets) since Decode returned — then
      // there is nothing to rebuild and no budget to start.
      if (!StillTheDecoderLocked(stream_id, decoder)) {
        return;
      }
      DecoderResetBudget& budget = decoder_resets[stream_id];
      const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(started.time_since_epoch()).count();
      if (!budget.NoteStall(now_ms)) {
        return;
      }
      resets = budget.Resets();
      exhausted = budget.Exhausted();
    }
    decoder->codec->ResetDecoder();
    const auto took_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    EngineLog().warning << "remote H264 decoder stalled; rebuilt stream=" << stream_id << " resets=" << resets
                        << (exhausted ? " (no more rebuilds this call)" : "") << " took_ms=" << took_ms;
  }

  /**
   * Stopped, or replaced by a newer session: from here this session only cleans up (FinishRetired,
   * on its own thread) and reports nothing. Never waits on a media thread or a device. Returns
   * whether anything ran (an idle session has nothing to clean up).
   */
  bool BeginRetire() {
    audio_session_seen = SessionGeneration();
    retired.store(true, std::memory_order_release);
    std::lock_guard lock(mutex);
    const bool running =
        active || packet_mode || capture_thread.joinable() || playout_thread.joinable() || video_thread.joinable();
    if (running) {
      EngineLog().info << "media stop begin session=" << session_id;
    }
    active = false;
    muted.store(false, std::memory_order_relaxed);
    RequestCameraOffLocked();
    started_at_ms.store(0, std::memory_order_relaxed);
    retired_send = std::move(packet_send);
    packet_send = nullptr;
    packet_mode = false;
    capture_running = false;
    playout_running = false;
    video_running = false;
    ApplyStateLocked("closed");
    return running;
  }

  /**
   * Wait out a send already inside the callback (the transport's Detach unblocks a blocking write),
   * then drop the callback: from here no thread of this session calls it — they check `retired`
   * under the same lock. So the owner may destroy what the callback reaches as soon as Stop returns.
   */
  void DrainSend() {
    std::shared_ptr<PacketSendFn> abandoned;
    std::lock_guard drain(packet_send_call_mu);
    std::lock_guard lock(mutex);
    abandoned = std::move(retired_send);
  }

  /**
   * A retired session's cleanup, on its own thread: join its media threads, release the devices,
   * tear the codecs down. A step that hangs (a wedged OS device or hardware codec) holds only this
   * thread — and the devices this session still holds, which the arbiter hands to the next holder
   * once they are released (L016). The audio devices go before the video thread is joined, so a
   * stuck camera or encoder keeps no microphone.
   */
  void FinishRetired() {
    const auto stop_from = std::chrono::steady_clock::now();
    std::string stopping_session;
    {
      std::lock_guard lock(mutex);
      stopping_session = session_id;
    }
    // B128: which step of a stop hangs (codec teardown vs. capture / playout / camera joins).
    const auto timed_step = [&stopping_session](const char* step, const auto& run) {
      const auto from = std::chrono::steady_clock::now();
      run();
      const int64_t ms = MsSince(from);
      if (ms > kSlowStopStepMs) {
        EngineLog().warning << "media stop slow step=" << step << " ms=" << ms << " session=" << stopping_session;
      } else {
        EngineLog().info << "media stop step=" << step << " ms=" << ms;
      }
    };
    timed_step("join_capture", [&] { JoinCaptureThread("media stop"); });
    timed_step("join_playout", [&] { JoinPlayoutThread("media stop"); });
    timed_step("release_audio", [&] {
      {
        std::lock_guard lock(mutex);
        ReleaseAudioLocked();
      }
      ReleaseAudioSessionIfUnchanged(audio_session_seen);
    });
    timed_step("join_video", [&] { JoinVideoThread("media stop"); });
    // B125 / B128: destroying a hardware codec can take ~1 s on a phone (or block for good when the
    // vendor codec is wedged): taken out under the lock, destroyed after it.
    timed_step("codecs", [&] {
      std::unique_ptr<IVideoCodec> codec;
      std::unordered_map<uint32_t, std::shared_ptr<RemoteDecoder>> decoders;
      {
        std::lock_guard lock(mutex);
        codec = std::move(video_codec);
        decoders.swap(remote_decoders);
        decoder_resets.clear();
        ClearVideoFrames();
        session_id.clear();
      }
    });
    EngineLog().info << "media stop done session=" << stopping_session << " ms=" << MsSince(stop_from);
  }

  /** The send callback a retired session dropped, released once no thread is inside it. */
  std::shared_ptr<PacketSendFn> retired_send;
};

MediaEngine::MediaEngine() : MediaEngine(MediaDeviceArbiter::Default()) {}

MediaEngine::MediaEngine(MediaDeviceArbiter& devices)
    : shared_(std::make_shared<Shared>()), retirements_(std::make_shared<Retirements>()) {
  redirectLogger("MediaEngine");
  shared_->own_devices = &devices;
  shared_->devices = &devices;
  impl_ = std::make_shared<Impl>(shared_);
}

MediaEngine::~MediaEngine() {
  Stop();
  // Product quit / owner teardown: give retired sessions a moment; one stuck on a device is left to
  // the process exit (it keeps what it uses alive).
  if (!WaitForRetiredSessions(kRetiredWaitOnDestroy)) {
    log().warning << "media engine destroyed while a stopped session still cleans up";
  }
}

std::shared_ptr<MediaEngine::Impl> MediaEngine::Session() const {
  std::lock_guard lock(session_mu_);
  return impl_;
}

std::shared_ptr<MediaEngine::Impl> MediaEngine::ReplaceSession() {
  auto fresh = std::make_shared<Impl>(shared_);
  std::lock_guard lock(session_mu_);
  return std::exchange(impl_, std::move(fresh));
}

bool MediaEngine::Retire(std::shared_ptr<Impl> session) {
  if (!session || !session->BeginRetire()) {
    return false;
  }
  session->DrainSend();
  {
    std::lock_guard lock(retirements_->mu);
    ++retirements_->running;
  }
  std::thread([retirements = retirements_, session = std::move(session)]() mutable {
    session->FinishRetired();
    session.reset();
    {
      std::lock_guard lock(retirements->mu);
      --retirements->running;
    }
    retirements->cv.notify_all();
  }).detach();
  return true;
}

bool MediaEngine::WaitForRetiredSessions(const std::chrono::milliseconds budget) {
  std::unique_lock lock(retirements_->mu);
  return retirements_->cv.wait_for(lock, budget, [this]() { return retirements_->running == 0; });
}

void MediaEngine::SetHardwareLimits(const HardwareLimits& limits) {
  {
    std::lock_guard lock(shared_->mu);
    shared_->limits = limits;
  }
  RefreshIdleSession();
}

void MediaEngine::SetVideoCodecFactoryForTest(std::function<std::unique_ptr<IVideoCodec>()> make) {
  {
    std::lock_guard lock(shared_->mu);
    shared_->make_video_codec = std::move(make);
  }
  RefreshIdleSession();
}

void MediaEngine::SetSkipDeviceOpenForTest(bool skip) {
  shared_->skip_device_open_for_test.store(skip, std::memory_order_relaxed);
  {
    std::lock_guard lock(shared_->mu);
    if (skip && !shared_->test_devices) {
      shared_->test_devices = std::make_unique<MediaDeviceArbiter>(CreateNullMediaDeviceBackend());
    }
    shared_->devices = skip ? shared_->test_devices.get() : shared_->own_devices;
  }
  RefreshIdleSession();
}

void MediaEngine::RefreshIdleSession() {
  // Settings a session copies when made (devices, codec factory) are set before Start: an idle
  // session is replaced so it has them; a running one keeps what it started with.
  if (!Session()->active.load(std::memory_order_acquire)) {
    (void)Retire(ReplaceSession());
  }
}

Roe<void> MediaEngine::StartDuplex(const std::string& session_id, PacketSendFn send) {
  return Start(session_id, SessionSpec::Duplex(), std::move(send));
}

MediaEngine::SessionSpec MediaEngine::ActiveSpec() const {
  const std::shared_ptr<Impl> impl = Session();
  Impl::TimedGetterLock lock(*impl, "ActiveSpec");
  return impl->spec;
}

Roe<void> MediaEngine::Start(const std::string& session_id, const SessionSpec spec, PacketSendFn send) {
  if (session_id.empty()) {
    return Error("session id required");
  }
  if (!spec.capture && !spec.playback) {
    return Error("media session needs capture or playback");
  }
  auto next_send = std::make_shared<PacketSendFn>(std::move(send));
  if (spec.capture && !*next_send) {
    return Error("SFU send callback required");
  }
  const std::shared_ptr<Impl> current = Session();
  std::shared_ptr<PacketSendFn> abandoned_send;
  bool send_swap = false;
  {
    std::lock_guard lock(current->mutex);
    if (current->active && current->session_id == session_id && current->packet_mode && current->spec == spec) {
      // SoftMigrate from libp2p→media_relay: swap callback; capture may still hold old shared_ptr.
      abandoned_send = std::move(current->packet_send);
      current->packet_send = std::move(next_send);
      send_swap = true;
    }
  }
  if (send_swap) {
    SwapSend(*current, std::move(abandoned_send));
    return {};
  }
  // Anything else is a new session: the one before (idle, or another call's) retires and cleans up
  // on its own thread — this never waits for its threads or devices.
  (void)Retire(ReplaceSession());
  const std::shared_ptr<Impl> impl = Session();
  {
    std::lock_guard lock(impl->mutex);
    if (auto codecs = impl->EnsureOpusCodecs(); !codecs) {
      impl->ReleaseAudioLocked();
      return codecs.error();
    }
    impl->packet_mode = true;
    impl->spec = spec;
    impl->packet_send = spec.capture ? std::move(next_send) : nullptr;
    impl->session_id = session_id;
    impl->active = true;
    impl->started_at_ms.store(util::NowUnixMs(), std::memory_order_relaxed);
    impl->ApplyStateLocked("connected");
    impl->StartCaptureLoop(); // device owner for both halves; encodes only when capturing
    if (spec.playback) {
      impl->StartPlayoutLoop();
    }
  }
  return {};
}

void MediaEngine::SwapSend(Impl& impl, std::shared_ptr<PacketSendFn> abandoned_send) {
  // Drain any capture/video thread still inside the old send before the caller Detach's that
  // stream — otherwise SoftMigrate races into heap corruption.
  {
    std::lock_guard drain(impl.packet_send_call_mu);
    abandoned_send = nullptr;
  }
  // 1:1 libp2p also uses StartDuplex with stream_id=0 packets. SoftMigrate to media_relay must
  // drop that zombie track or it PLC-underruns forever and confuses stream_count (dogfood).
  // Do NOT wipe live media_relay RX tracks on duplicate StartDuplex send-swap (reattach storm).
  {
    std::lock_guard lock(impl.mutex);
    impl.audio_tracks.erase(0);
    {
      std::lock_guard lg(impl.rx_log_mu);
      impl.rx_logged_streams.erase(0);
    }
  }
  // Android speaker / communication-mode route changes around SoftMigrate can leave the open
  // SDL recording device feeding zeros while encode still runs (mic_lvl→0, tx_frames rising).
  // Desktop SoftMigrate send-swap must not close/reopen SDL (malloc/double-free on Leave).
  if (AudioSession::CaptureReopenSettleDelayMs() > 0) {
    RequestAudioDeviceReopen();
  }
  RequestVideoKeyframe();
}

void MediaEngine::OnInboundPacket(const MediaPacket& packet) {
  const std::shared_ptr<Impl> impl = Session();
  // Audio is taken under mutex: ReleaseAudioLocked (stop, a failed Start) destroys the remote tracks'
  // Opus decoders while the media_relay client reader can already deliver frames (guest attach
  // dogfood crash). Video is decoded after it (#474): only its decoder is found under it.
  std::shared_ptr<Impl::RemoteDecoder> video_decoder;
  {
    std::unique_lock lock(impl->mutex);
    if (!impl->active || !impl->packet_mode || !impl->spec.playback || packet.payload.empty()) {
      return;
    }
    // PublisherStreamIdForIdentity("") == 1. Never a real media_relay publisher — only the
    // inbound 1:1 path with a missing peer_key. Drop so SoftMigrate cannot leave a PLC zombie.
    if (packet.stream_id == 1u) {
      return;
    }
    {
      // Log the first packet per stream_id per process (PreferLocal multi-peer RX).
      // Cleared on SoftMigrate StartDuplex send-swap so post-migrate RX is visible again.
      bool log_this = false;
      {
        std::lock_guard lg(impl->rx_log_mu);
        log_this = impl->rx_logged_streams.insert(packet.stream_id).second;
      }
      if (log_this) {
        log().info << "OnInboundPacket first stream=" << packet.stream_id << " ch=" << packet.channel_id
                   << " seq=" << packet.seq << " bytes=" << packet.payload.size() << " session=" << impl->session_id;
      }
    }
    if (mesh::IsAudioChannel(packet.channel_id)) {
      impl->OnRemoteOpusFrame(packet.stream_id, packet.seq, reinterpret_cast<const std::byte*>(packet.payload.data()),
                              packet.payload.size());
    } else if (mesh::IsVideoChannel(packet.channel_id)) {
      video_decoder = impl->RemoteDecoderLocked(packet.stream_id);
    }
  }
  if (video_decoder) {
    impl->DecodeRemoteVideo(packet.stream_id, video_decoder, reinterpret_cast<const std::byte*>(packet.payload.data()),
                            packet.payload.size());
  }
}

bool MediaEngine::IsPacketMode() const {
  const std::shared_ptr<Impl> impl = Session();
  Impl::TimedGetterLock lock(*impl, "IsPacketMode");
  return impl->packet_mode;
}

void MediaEngine::ApplyTargets(const MediaTargets& targets) {
  const std::shared_ptr<Impl> impl = Session();
  if (impl->adaptation_send_video.exchange(targets.send_video, std::memory_order_relaxed) != targets.send_video) {
    EngineLog().info << "video send " << (targets.send_video ? "resumed" : "paused") << " by adaptation";
  }
  impl->adaptation_target_video_bps.store(targets.video_bps, std::memory_order_relaxed);
  const int64_t audio_bps = targets.audio_bps > 0 ? targets.audio_bps : MediaTargets::kDefaultAudioBps;
  impl->adaptation_target_audio_bps.store(audio_bps, std::memory_order_relaxed);
  // Bitrates are applied by the threads that encode, right before their next frame (encoders are
  // not thread-safe — calling opus_encoder_ctl here raced opus_encode, TSan). A paused send is the
  // video thread's too: it keeps the camera and its preview, and skips the encode.
}

double MediaEngine::PathPressure() const {
  const std::shared_ptr<Impl> impl = Session();
  return impl->path_pressure.load(std::memory_order_relaxed);
}

void MediaEngine::NoteOutboundDrop() {
  const std::shared_ptr<Impl> impl = Session();
  impl->outbound_drops.fetch_add(1, std::memory_order_relaxed);
}

MediaEngineHealth MediaEngine::HealthSnapshot() const {
  const std::shared_ptr<Impl> impl = Session();
  MediaEngineHealth h;
  h.active = impl->active.load(std::memory_order_relaxed);
  // Started and not stopped — also while running without an audio device (RX muted, device still
  // opening); a stopped engine's counters are not the call's.
  h.engine_running = h.active;
  h.muted = impl->muted.load(std::memory_order_relaxed);
  h.path_pressure = impl->path_pressure.load(std::memory_order_relaxed);
  h.opus_target_bps = impl->adaptation_target_audio_bps.load(std::memory_order_relaxed);
  h.outbound_drops = impl->outbound_drops.load(std::memory_order_relaxed);
  h.playout_underruns = impl->playout_underruns_total.load(std::memory_order_relaxed);
  h.plc_frames = impl->plc_frames_total.load(std::memory_order_relaxed);
  h.fec_frames = impl->fec_frames_total.load(std::memory_order_relaxed);
  h.rx_audio_frames = impl->rx_audio_frames.load(std::memory_order_relaxed);
  h.tx_audio_frames = impl->tx_audio_frames.load(std::memory_order_relaxed);
  h.last_rx_audio_ms = impl->last_rx_audio_ms.load(std::memory_order_relaxed);
  h.last_tx_audio_ms = impl->last_tx_audio_ms.load(std::memory_order_relaxed);
  h.rx_video_frames = impl->rx_video_frames.load(std::memory_order_relaxed);
  h.tx_video_frames = impl->tx_video_frames.load(std::memory_order_relaxed);
  h.last_rx_video_ms = impl->last_rx_video_ms.load(std::memory_order_relaxed);
  h.video_target_bps = impl->adaptation_target_video_bps.load(std::memory_order_relaxed);
  h.local_level = impl->local_input_level.load(std::memory_order_relaxed);
  h.remote_level = impl->remote_output_level.load(std::memory_order_relaxed);
  {
    Impl::TimedGetterLock lock(*impl, "HealthSnapshot");
    AudioDeviceLease* speaker = impl->speaker_lease.get();
    const bool voice = speaker && speaker->IsVoiceDuplex();
    const bool any_device = (impl->mic_lease && impl->mic_lease->HasDevice()) || (speaker && speaker->HasDevice());
    h.audio_io = voice ? "vpio" : (any_device ? "sdl" : "none");
    h.io_underruns = impl->vpio_underruns_closed + Impl::VoiceUnderruns(speaker);
    h.stream_count = impl->audio_tracks.size();
    h.packet_mode = impl->packet_mode;
    h.capture_available = impl->capture_available;
    h.streams.reserve(impl->audio_tracks.size());
    for (const auto& [id, track] : impl->audio_tracks) {
      if (!track) {
        continue;
      }
      MediaStreamHealth s;
      s.stream_id = id;
      s.rx_frames = track->rx_frames;
      s.last_rx_ms = track->last_rx_ms;
      s.peak_level = track->peak_level;
      h.streams.push_back(s);
      h.jitter_target_ms = std::max(h.jitter_target_ms,
                                     static_cast<int64_t>(track->jitter.TargetFrames()) * AudioJitterBuffer::kFrameMs);
      h.jitter_depth_ms = std::max(h.jitter_depth_ms,
                                    static_cast<int64_t>(track->jitter.size()) * AudioJitterBuffer::kFrameMs);
      h.jitter_silence_drops += track->jitter.silence_drops();
      h.jitter_speech_drops += track->jitter.speech_drops();
    }
  }
  return h;
}

void MediaEngine::Stop() {
  // The session's threads and devices are released on its own thread (FinishRetired): a stop never
  // waits on hardware, so the calls owner going on with the next invite cannot be held by it (#452).
  (void)Retire(ReplaceSession());
}

void MediaEngine::SetMuted(bool muted) {
  const std::shared_ptr<Impl> impl = Session();
  impl->muted.store(muted, std::memory_order_relaxed);
  if (muted) {
    impl->local_input_level.store(0.f, std::memory_order_relaxed);
  }
}

bool MediaEngine::IsMuted() const {
  const std::shared_ptr<Impl> impl = Session();
  return impl->muted.load(std::memory_order_relaxed);
}

void MediaEngine::RequestAudioDeviceReopen() {
  const std::shared_ptr<Impl> impl = Session();
  if (!impl->capture_running.load(std::memory_order_relaxed)) {
    return;
  }
  impl->audio_reopen_requested.store(true, std::memory_order_release);
}

Roe<void> MediaEngine::SetCameraEnabled(bool enabled) {
  // Read on the caller's (UI) thread: iOS orientation is UIKit; the open itself runs elsewhere.
  return SetCameraEnabled(enabled, enabled ? CameraDisplayRotationDegrees() : 0);
}

Roe<void> MediaEngine::SetCameraEnabled(bool enabled, const int display_rotation) {
  const std::shared_ptr<Impl> impl = Session();
  std::lock_guard lock(impl->mutex);
  if (!impl->active) {
    return Error("Media session not active");
  }
  if (!enabled) {
    impl->RequestCameraOffLocked();
    return {};
  }
  if (!impl->spec.capture) {
    return Error("camera needs a capturing session");
  }
  if (impl->skip_device_open_for_test.load(std::memory_order_relaxed)) {
    return Error("camera skipped (test)");
  }
  // Async: the video thread takes the camera lease (opened on the media device thread). A failed
  // open flips IsCameraEnabled back to false and is reported through TakeCameraFailure.
  impl->camera_display_rotation.store(display_rotation, std::memory_order_relaxed);
  impl->camera_enabled.store(true, std::memory_order_relaxed);
  if (!impl->video_thread.joinable()) {
    impl->StartVideoLoop();
  }
  return {};
}

Roe<void> MediaEngine::FlipCamera() {
  const std::shared_ptr<Impl> impl = Session();
  std::lock_guard lock(impl->mutex);
  if (!impl->active) {
    return Error("Media session not active");
  }
  if (!impl->camera_enabled.load(std::memory_order_relaxed)) {
    return Error("camera is off");
  }
  if (!impl->camera_open.load(std::memory_order_acquire)) {
    return Error("camera is still opening");  // the facing is not known yet; a tap this early does nothing
  }
  if (impl->camera_reopen_requested.load(std::memory_order_acquire)) {
    return {};  // one flip is already on its way; a second tap does not undo it
  }
  // The video thread owns the lease: it releases the open camera and takes the other facing, with
  // the display rotation it already has (UpdateCameraDisplayRotation keeps that current). The target
  // is the opposite of the camera actually open, so a device that gave the same camera twice still flips.
  impl->camera_prefer_front.store(!impl->camera_front_facing.load(std::memory_order_relaxed),
                                  std::memory_order_relaxed);
  impl->camera_reopen_requested.store(true, std::memory_order_release);
  return {};
}

bool MediaEngine::CameraFrontFacing() const {
  const std::shared_ptr<Impl> impl = Session();
  return impl->camera_front_facing.load(std::memory_order_relaxed);
}

void MediaEngine::SetVideoLevel(const uint8_t level) {
  const std::shared_ptr<Impl> impl = Session();
  if (!mesh::IsVideoLevel(level)) {
    return;
  }
  impl->video_level.store(level, std::memory_order_relaxed);
  impl->video_level_profile.store(true, std::memory_order_relaxed);
}

uint8_t MediaEngine::VideoLevel() const {
  const std::shared_ptr<Impl> impl = Session();
  return impl->video_level.load(std::memory_order_relaxed);
}

void MediaEngine::UpdateCameraDisplayRotation(const int display_rotation_deg) {
  const std::shared_ptr<Impl> impl = Session();
  impl->camera_display_rotation.store(display_rotation_deg, std::memory_order_relaxed);
}

std::optional<CameraFailure> MediaEngine::TakeCameraFailure() {
  const std::shared_ptr<Impl> impl = Session();
  std::lock_guard lock(impl->camera_failure_mu);
  return std::exchange(impl->camera_failure, std::nullopt);
}

bool MediaEngine::IsCameraEnabled() const {
  const std::shared_ptr<Impl> impl = Session();
  return impl->camera_enabled.load(std::memory_order_relaxed);
}

bool MediaEngine::CameraWaitingForDevice() const {
  const std::shared_ptr<Impl> impl = Session();
  return impl->camera_enabled.load(std::memory_order_relaxed) && impl->camera_waiting.load(std::memory_order_relaxed);
}

bool MediaEngine::HasRemoteVideo() const {
  const std::shared_ptr<Impl> impl = Session();
  return impl->has_remote_video.load(std::memory_order_relaxed);
}

bool MediaEngine::IsRemoteVideoStalling() const {
  const std::shared_ptr<Impl> impl = Session();
  if (!impl->has_remote_video.load(std::memory_order_relaxed)) {
    return false;
  }
  const int64_t last = impl->last_remote_video_ms.load(std::memory_order_relaxed);
  if (last <= 0) {
    return false;
  }
  const int64_t age = util::NowUnixMs() - last;
  return age >= kRemoteVideoStallSoftMs && age < kRemoteVideoStallHardMs;
}

bool MediaEngine::IsRemoteVideoLive(const int64_t within_ms) const {
  const std::shared_ptr<Impl> impl = Session();
  if (!impl->has_remote_video.load(std::memory_order_relaxed)) {
    return false;
  }
  const int64_t last = impl->last_remote_video_ms.load(std::memory_order_relaxed);
  return last > 0 && util::NowUnixMs() - last < within_ms;
}

bool MediaEngine::EverHadRemoteVideo() const {
  const std::shared_ptr<Impl> impl = Session();
  return impl->ever_had_remote_video.load(std::memory_order_relaxed);
}

void MediaEngine::ClearRemoteVideo() {
  const std::shared_ptr<Impl> impl = Session();
  impl->ClearRemoteVideoFrames();
}

void MediaEngine::RefreshRemoteVideoHealth() {
  const std::shared_ptr<Impl> impl = Session();
  if (!impl->active.load()) {
    return;
  }
  const int64_t now = util::NowUnixMs();
  std::string state;
  {
    Impl::TimedGetterLock lock(*impl, "RefreshRemoteVideoHealth");
    state = impl->connection_state;
  }
  if (state == "failed" || state == "closed") {
    impl->ClearRemoteVideoFrames();
    return;
  }
  if (!impl->has_remote_video.load(std::memory_order_relaxed)) {
    return;
  }
  std::lock_guard lock(impl->video_frame_mutex);
  int64_t newest_ms = 0;
  uint32_t newest_stream = 0;
  for (auto it = impl->last_video_ms_by_stream.begin(); it != impl->last_video_ms_by_stream.end();) {
    if (it->second > 0 && (now - it->second) >= kRemoteVideoStallHardMs) {
      impl->remote_video_by_stream.erase(it->first);
      it = impl->last_video_ms_by_stream.erase(it);
      continue;
    }
    if (it->second >= newest_ms) {
      newest_ms = it->second;
      newest_stream = it->first;
    }
    ++it;
  }
  if (impl->last_video_ms_by_stream.empty()) {
    impl->remote_video_frame = {};
    impl->has_remote_video.store(false, std::memory_order_relaxed);
    impl->last_remote_video_ms.store(0, std::memory_order_relaxed);
    return;
  }
  impl->last_remote_video_ms.store(newest_ms, std::memory_order_relaxed);
  auto live = impl->remote_video_by_stream.find(newest_stream);
  if (live != impl->remote_video_by_stream.end()) {
    impl->remote_video_frame = live->second;
  }
}

bool MediaEngine::VideoEncoderAvailable() const {
  // Host capability, not session state: the local encoder only exists while the camera is on.
  return PlatformVideoEncoderSupported();
}

void MediaEngine::NoteUplinkBudget(int64_t per_user_up_bps) {
  const std::shared_ptr<Impl> impl = Session();
  impl->last_uplink_bps.store(per_user_up_bps, std::memory_order_relaxed);
}

int64_t MediaEngine::UplinkBudget() const {
  const std::shared_ptr<Impl> impl = Session();
  return impl->last_uplink_bps.load(std::memory_order_relaxed);
}

void MediaEngine::RequestVideoKeyframe() {
  const std::shared_ptr<Impl> impl = Session();
  impl->video_need_keyframe.store(true, std::memory_order_release);
}

std::vector<uint32_t> MediaEngine::TakePendingVideoRefreshStreamIds() {
  const std::shared_ptr<Impl> impl = Session();
  std::lock_guard lock(impl->video_refresh_mu);
  std::vector<uint32_t> out(impl->pending_video_refresh_streams.begin(), impl->pending_video_refresh_streams.end());
  impl->pending_video_refresh_streams.clear();
  return out;
}

std::vector<uint32_t> MediaEngine::PendingVideoRefreshStreamsForTest() const {
  const std::shared_ptr<Impl> impl = Session();
  std::lock_guard lock(impl->video_refresh_mu);
  return {impl->pending_video_refresh_streams.begin(), impl->pending_video_refresh_streams.end()};
}

bool MediaEngine::IsActive() const {
  const std::shared_ptr<Impl> impl = Session();
  return impl->active.load();
}

std::string MediaEngine::ActiveSessionId() const {
  const std::shared_ptr<Impl> impl = Session();
  Impl::TimedGetterLock lock(*impl, "ActiveSessionId");
  return impl->session_id;
}

int64_t MediaEngine::StartedAtMs() const {
  const std::shared_ptr<Impl> impl = Session();
  return impl->started_at_ms.load(std::memory_order_relaxed);
}

bool MediaEngine::HasLocalCapture() const {
  const std::shared_ptr<Impl> impl = Session();
  Impl::TimedGetterLock lock(*impl, "HasLocalCapture");
  return impl->capture_available;
}

float MediaEngine::LocalInputLevel() const {
  const std::shared_ptr<Impl> impl = Session();
  return impl->local_input_level.load(std::memory_order_relaxed);
}

float MediaEngine::RemoteOutputLevel() const {
  const std::shared_ptr<Impl> impl = Session();
  return impl->remote_output_level.load(std::memory_order_relaxed);
}

bool MediaEngine::CopyLocalVideoFrame(VideoTileFrame& out) const {
  const std::shared_ptr<Impl> impl = Session();
  std::lock_guard lock(impl->video_frame_mutex);
  if (impl->local_video_frame.rgba.empty()) {
    return false;
  }
  out = impl->local_video_frame;
  return true;
}

bool MediaEngine::CopyRemoteVideoFrame(VideoTileFrame& out) const {
  const std::shared_ptr<Impl> impl = Session();
  std::lock_guard lock(impl->video_frame_mutex);
  if (impl->remote_video_frame.rgba.empty()) {
    return false;
  }
  out = impl->remote_video_frame;
  return true;
}

bool MediaEngine::CopyRemoteVideoFrameForStream(uint32_t stream_id, VideoTileFrame& out) const {
  const std::shared_ptr<Impl> impl = Session();
  std::lock_guard lock(impl->video_frame_mutex);
  auto it = impl->remote_video_by_stream.find(stream_id);
  if (it == impl->remote_video_by_stream.end() || it->second.rgba.empty()) {
    return false;
  }
  out = it->second;
  return true;
}

bool MediaEngine::HasRemoteVideoForStream(uint32_t stream_id) const {
  const std::shared_ptr<Impl> impl = Session();
  std::lock_guard lock(impl->video_frame_mutex);
  auto it = impl->remote_video_by_stream.find(stream_id);
  return it != impl->remote_video_by_stream.end() && !it->second.rgba.empty();
}

} // namespace pbr
