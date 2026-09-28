#pragma once

#include "domain/media/CallMediaAdaptation.h"
#include "domain/media/IVideoCodec.h"
#include "domain/media/MediaDeviceArbiter.h"
#include "common/media/CallMediaHealth.h"
#include "common/Error.h"
#include "common/Module.h"

#include <atomic>
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
 */
class CallMediaEngine : public Module {
public:
  struct VideoTileFrame {
    int width = 0;
    int height = 0;
    /** Premultiplied RGBA8. */
    std::vector<uint8_t> rgba;
    uint64_t seq = 0;
  };

  using StateChangedFn = std::function<void(const std::string& state)>;

  /** Audio devices from `MediaDeviceArbiter::Default()`. */
  CallMediaEngine();
  /** Audio devices from `devices` (must outlive the engine). */
  explicit CallMediaEngine(MediaDeviceArbiter& devices);
  ~CallMediaEngine() override;

  void SetOnStateChanged(StateChangedFn callback);

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
  /**
   * Drop remote jitter/PLC tracks without stopping capture. SoftMigrate send-swap clears
   * tracks internally; ReleaseDirect must not wipe live media_relay tracks.
   */
  void ClearRemoteAudioTracks();
  bool IsSfuMode() const;
  void Stop();

  void SetMuted(bool muted);
  bool IsMuted() const;

  /**
   * Reopen SDL capture/playback on the capture worker (Android speaker route / SoftMigrate).
   * Safe while media is active; no-op if capture is not running.
   */
  void RequestAudioDeviceReopen();

  /** Apply V024 producer decision (camera gate + Opus target bps). */
  void ApplyAdaptation(const CallAdaptationDecision& decision);

  /** 0..1 receiver/path pressure for adaptation (V032). */
  double PathPressure() const;
  /** Hop/send path observed a drop — raises pressure (V032). */
  void NoteOutboundDrop();
  /** Snapshot for chrome / logs (V032 instrumentation). */
  CallMediaEngineHealth HealthSnapshot() const;

  /**
   * Request the camera on / off (best-effort: never kills voice, V019). Call on the UI thread (reads display rotation). Enabling is
   * asynchronous: the video thread opens the camera on the media device thread; IsCameraEnabled is
   * true from the request until it is turned off or the open fails (then TakeCameraFailure says why).
   */
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
  bool IsCameraEnabled() const;
  /** Why the last camera request did not open, once (UI poll, like TakePendingVideoRefreshStreamIds). */
  std::optional<std::string> TakeCameraFailure();
  /**
   * True when a fresh remote decoded frame is available (not stalled / cleared).
   * Call RefreshRemoteVideoHealth() from the UI tick before reading.
   */
  bool HasRemoteVideo() const;
  /** Soft stall: frames aged past soft threshold but not yet cleared. */
  bool IsRemoteVideoStalling() const;
  /** True after at least one remote frame this media session (survives hard-stall clear). */
  bool EverHadRemoteVideo() const;
  /** Drop last remote frame (camera off, leave, hard stall, connection dead). */
  void ClearRemoteVideo();
  /**
   * UI-tick health: clear remote video on hard frame stall or failed/closed connection.
   */
  void RefreshRemoteVideoHealth();
  bool VideoEncoderAvailable() const;
  /** Probe V024 with camera wanted using last hop A↑ (1:1 unbounded). */
  bool CameraPathAllowsVideo() const;
  /** Last hop quote A↑ (0 = unknown / 1:1 unbounded). */
  void NoteUplinkBudget(int64_t per_user_up_bps);
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

  bool IsActive() const;
  bool IsConnected() const;
  /** Update chrome-facing SFU connection state (e.g. libp2p pending direct stream). */
  void SetConnectionState(const std::string& state);
  std::string ActiveCallId() const;
  /**
   * Bumped on each StartSfu. Posted StopMeshMedia must no-op if this advanced — otherwise
   * AcceptInvite leftover Stop can kill the new call's duplex (dogfood: both sides Calling).
   */
  uint64_t MediaSessionGeneration() const;
  std::string ConnectionState() const;
  int64_t ConnectedAtMs() const;
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
  std::unique_ptr<Impl> impl_;
};

} // namespace pbr
