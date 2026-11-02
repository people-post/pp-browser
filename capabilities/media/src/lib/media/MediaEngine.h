#pragma once

#include "lib/media/GuardedVideoCodec.h"
#include "lib/media/IVideoCodec.h"
#include "lib/media/MediaDeviceArbiter.h"
#include "common/media_engine/MediaEngineHealth.h"
#include "common/media_engine/MediaTargets.h"
#include "common/Error.h"
#include "common/Module.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * 1:1 call media via libp2p/SFU packet transport + Opus + SDL (+ platform HW H264, V014/V017/V019).
 * Always encodes Opus + H264; mute/camera are content-only.
 *
 * Each Start makes a new media session; Stop (or a Start for another session) retires the running
 * one, which then joins its threads and releases its devices on a thread of its own (media-client-
 * layers L016). So no caller — the calls owner above all — ever waits on a device or a hardware
 * codec here, and a stuck one holds only the session that used it.
 */
class MediaEngine : public Module {
public:
  struct VideoTileFrame {
    int width = 0;
    int height = 0;
    /** Premultiplied RGBA8. */
    std::vector<uint8_t> rgba;
    uint64_t seq = 0;
  };

  using StateChangedFn = std::function<void(const std::string& state)>;

  /**
   * How long hardware may take before the engine stops waiting for it (L016). The defaults suit a
   * product; tests shorten them. Set before Start.
   */
  struct HardwareLimits {
    VideoCodecBudgets codecs;
    /** A camera whose device stays stuck this long is given up on ("restart the app"). */
    std::chrono::milliseconds device_give_up_after{30000};
  };

  /** Audio devices from `MediaDeviceArbiter::Default()`. */
  MediaEngine();
  /** Audio devices from `devices` (must outlive the engine). */
  explicit MediaEngine(MediaDeviceArbiter& devices);
  ~MediaEngine() override;

  void SetOnStateChanged(StateChangedFn callback);
  void SetHardwareLimits(const HardwareLimits& limits);

  /**
   * SFU encoded packet (app maps to N021 MediaDataFrame).
   * channel: 0 = audio (reliable_ordered), 1 = video_lo (latest_lossy).
   */
  struct SfuPacket {
    /** Publisher stream id (N021); 0 for 1:1 direct. */
    uint32_t stream_id = 0;
    uint16_t channel_id = 0;
    uint32_t seq = 0;
    uint8_t mark = 0;
    std::vector<uint8_t> payload;
  };
  using SfuSendFn = std::function<void(const SfuPacket&)>;
  /** Remote audio tracks (one Opus decoder + jitter buffer per publisher stream_id) kept at once; packets
   *  for further stream ids are dropped (PB-051). Video decoders have their own cap of 4. */
  static constexpr size_t kMaxRemoteAudioTracks = 8;

  /**
   * Which halves of a media session run (media-client-layers L003/L004). Calls are duplex; a
   * broadcaster is capture-only (mic → encode → send); a viewer is playback-only (receive →
   * decode → mix → speaker). Channels stay generic (0 = Opus, 1 = H264).
   */
  struct SessionSpec {
    bool capture = true;
    bool playback = true;

    static SessionSpec Duplex() { return {true, true}; }
    static SessionSpec CaptureOnly() { return {true, false}; }
    static SessionSpec PlaybackOnly() { return {false, true}; }
    bool operator==(const SessionSpec& o) const { return capture == o.capture && playback == o.playback; }
  };

  /**
   * Start capture + duplex send for Amp 1:1 call-media **or** media_relay hop (V038).
   * Name is historical — not “join SFU” alone; Bridge and Topology both call this.
   */
  Roe<void> StartSfu(const std::string& call_id, SfuSendFn send);
  /**
   * Start a media session with only the halves `spec` asks for. `send` is required when
   * capturing and ignored otherwise. Capture-only never opens the speaker or decodes inbound
   * packets; playback-only never opens the mic, activates the VoIP audio session or sends.
   * `StartSfu` = `Start(call_id, SessionSpec::Duplex(), send)`.
   */
  Roe<void> Start(const std::string& session_id, SessionSpec spec, SfuSendFn send);
  /** Halves of the active session (Duplex when idle). */
  SessionSpec ActiveSpec() const;
  /** Inbound SFU payload (already demuxed to local subscribe; plaintext Opus). */
  void OnSfuPacket(const SfuPacket& packet);
  bool IsSfuMode() const;
  /** Retire the session; returns at once (its cleanup runs on its own thread). Emits "closed". */
  void Stop();
  /**
   * Wait until every retired session has finished cleaning up, at most `budget`. For tests and an
   * orderly quit; false when one is still running (stuck on a device).
   */
  bool WaitForRetiredSessions(std::chrono::milliseconds budget);

  void SetMuted(bool muted);
  bool IsMuted() const;

  /**
   * Reopen SDL capture/playback on the capture worker (Android speaker route / SoftMigrate).
   * Safe while media is active; no-op if capture is not running.
   */
  void RequestAudioDeviceReopen();

  /** What to send at, from the feature's adaptation policy (calls: V024): the camera gate and bitrates. */
  void ApplyTargets(const MediaTargets& targets);

  /** 0..1 receiver/path pressure for adaptation (V032). */
  double PathPressure() const;
  /** Hop/send path observed a drop — raises pressure (V032). */
  void NoteOutboundDrop();
  /** Snapshot for chrome / logs (V032 instrumentation). */
  MediaEngineHealth HealthSnapshot() const;

  /**
   * Request the camera on / off (best-effort: never kills voice, V019). Call on the UI thread (reads display rotation). Enabling is
   * asynchronous: the video thread opens the camera on the media device thread; IsCameraEnabled is
   * true from the request until it is turned off or the open fails (then TakeCameraFailure says why).
   */
  /**
   * Send video at `level` (docs/contracts/MEDIA_CHANNELS.md), sized and paced by its profile
   * (`VideoLevelProfile`; a broadcast publisher's negotiated level). Takes effect when the camera
   * next opens. Calls never set it: they send the default level at the camera's size.
   */
  void SetVideoLevel(uint8_t level);
  uint8_t VideoLevel() const;
  Roe<void> SetCameraEnabled(bool enabled);
  /**
   * Same, with the display rotation read by the caller on UI (`CameraDisplayRotationDegrees`) — for
   * callers off the UI thread (the calls owner); iOS reads orientation from UIKit, main thread only.
   */
  Roe<void> SetCameraEnabled(bool enabled, int display_rotation_degrees);
  /**
   * The display turned while the camera is on (read on UI via `CameraDisplayRotationDegrees`): later
   * frames rotate to match without reopening the camera or reconfiguring the encoder. Any thread.
   */
  void UpdateCameraDisplayRotation(int display_rotation_deg);
  /**
   * Phones: switch between the front and rear camera while the camera is on (error when it is off).
   * Asynchronous like SetCameraEnabled: the video thread releases the camera and opens the other
   * facing; the next call starts with the front camera again. Any thread.
   */
  Roe<void> FlipCamera();
  /** Facing of the camera the engine last opened (false before any open). */
  bool CameraFrontFacing() const;
  /** The camera is wanted: on, opening, or waiting for the device (CameraWaitingForDevice). */
  bool IsCameraEnabled() const;
  /**
   * The camera was asked for but is held by someone else or not responding: it opens by itself when
   * the device is free again (L016). TakeCameraFailure reported it once.
   */
  bool CameraWaitingForDevice() const;
  /** Why the last camera request did not open, once (UI poll, like TakePendingVideoRefreshStreamIds). */
  std::optional<std::string> TakeCameraFailure();
  /**
   * True when a fresh remote decoded frame is available (not stalled / cleared).
   * Call RefreshRemoteVideoHealth() from the UI tick before reading.
   */
  bool HasRemoteVideo() const;
  /** Soft stall: frames aged past soft threshold but not yet cleared. */
  bool IsRemoteVideoStalling() const;
  /** A remote frame arrived within the last within_ms (frames are still flowing). */
  bool IsRemoteVideoLive(int64_t within_ms) const;
  /** True after at least one remote frame this media session (survives hard-stall clear). */
  bool EverHadRemoteVideo() const;
  /** Drop last remote frame (camera off, leave, hard stall, connection dead). */
  void ClearRemoteVideo();
  /**
   * UI-tick health: clear remote video on hard frame stall or failed/closed connection.
   */
  void RefreshRemoteVideoHealth();
  bool VideoEncoderAvailable() const;
  /** The uplink the path last quoted per user (A↑; 0 = unknown / unbounded), for the feature's policy. */
  void NoteUplinkBudget(int64_t per_user_up_bps);
  int64_t UplinkBudget() const;
  /** Force next encoded AU to be an IDR (SoftMigrate / call_video_refresh). */
  void RequestVideoKeyframe();
  /**
   * Test-only: StartSfu / SetCameraEnabled skip SDL mic/camera open (silence TX, no device prompts).
   * Audio leases come from a private device-less arbiter, so several engines in one test process
   * never contend. Product must leave this false. Used by call compose fixtures (PR #216 follow-up).
   * Call before Start.
   */
  void SetSkipDeviceOpenForTest(bool skip);
  /** Test-only: codecs for the local encoder / remote decoders (default: platform HW). Before Start. */
  void SetVideoCodecFactoryForTest(std::function<std::unique_ptr<IVideoCodec>()> make);
  /** Drain stream ids that need an IDR (decode fail / first gap). */
  std::vector<uint32_t> TakePendingVideoRefreshStreamIds();
  /** Test-only: the stream ids waiting for an IDR, without draining them. */
  std::vector<uint32_t> PendingVideoRefreshStreamsForTest() const;

  bool IsActive() const;
  bool IsConnected() const;
  /** Update chrome-facing SFU connection state (e.g. libp2p pending direct stream). */
  void SetConnectionState(const std::string& state);
  std::string ActiveCallId() const;
  std::string ConnectionState() const;
  /** Wall time when StartSfu succeeded (0 if inactive). */
  int64_t StartedAtMs() const;
  /** False when mic open failed (silence sent); used for permission hints. */
  bool HasLocalCapture() const;

  float LocalInputLevel() const;
  float RemoteOutputLevel() const;

  /** Copy latest local preview / remote decoded frames (empty if none). */
  bool CopyLocalVideoFrame(VideoTileFrame& out) const;
  bool CopyRemoteVideoFrame(VideoTileFrame& out) const;
  /** Group: copy decoded frame for a publisher stream_id (0 = primary remote). */
  bool CopyRemoteVideoFrameForStream(uint32_t stream_id, VideoTileFrame& out) const;
  bool HasRemoteVideoForStream(uint32_t stream_id) const;

private:
  struct Impl;
  struct Shared;
  struct Retirements;
  /** How long the destructor lets retired sessions finish before leaving them to the process exit. */
  static constexpr std::chrono::milliseconds kRetiredWaitOnDestroy{5000};

  std::shared_ptr<Impl> Session() const;
  /** A fresh idle session becomes current; returns the one it replaced. */
  std::shared_ptr<Impl> ReplaceSession();
  /** Retire `session` and clean it up on its own thread; false when it had nothing running. */
  bool Retire(std::shared_ptr<Impl> session);
  /** Replace an idle session so it takes settings changed before Start. */
  void RefreshIdleSession();
  void SwapSend(Impl& impl, std::shared_ptr<SfuSendFn> abandoned_send);

  std::shared_ptr<Shared> shared_;
  std::shared_ptr<Retirements> retirements_;
  mutable std::mutex session_mu_;
  std::shared_ptr<Impl> impl_;
};

} // namespace pbr
