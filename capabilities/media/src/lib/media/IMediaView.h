#pragma once

#include "common/media_engine/MediaEngineHealth.h"
#include "common/Error.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The media engine as the screen sees it: what plays and how it is doing (levels, video frames,
 * health, the path's pressure), and the knobs that only touch this device's own capture and display
 * (flip the camera, reopen the audio route, the display's rotation, the UI tick's video upkeep).
 * Never a session: starting, stopping, packets, mute and the camera's on / off are the feature's
 * (its owner thread) — a screen asks the feature for those. The engine synchronizes itself: any thread.
 */
class IMediaView {
public:
  struct VideoTileFrame {
    int width = 0;
    int height = 0;
    /** Premultiplied RGBA8. */
    std::vector<uint8_t> rgba;
    uint64_t seq = 0;
  };

  virtual ~IMediaView() = default;

  // --- The session, as it is ---
  virtual bool IsActive() const = 0;
  virtual std::string ActiveCallId() const = 0;
  virtual bool IsSfuMode() const = 0;
  virtual bool IsMuted() const = 0;
  virtual MediaEngineHealth HealthSnapshot() const = 0;
  /** 0..1 receiver / path pressure. */
  virtual double PathPressure() const = 0;
  /** The uplink the path last quoted per user (0 = unknown / unbounded). */
  virtual int64_t UplinkBudget() const = 0;
  virtual bool VideoEncoderAvailable() const = 0;
  /** False when the mic open failed (silence sent); for permission hints. */
  virtual bool HasLocalCapture() const = 0;
  virtual float LocalInputLevel() const = 0;
  virtual float RemoteOutputLevel() const = 0;

  // --- The camera ---
  /** The camera is wanted: on, opening, or waiting for the device. */
  virtual bool IsCameraEnabled() const = 0;
  /** The camera was asked for but is held by someone else or not responding; it opens once free. */
  virtual bool CameraWaitingForDevice() const = 0;
  /** Why the last camera request did not open, once. */
  virtual std::optional<std::string> TakeCameraFailure() = 0;
  /** Phones: switch front / rear while the camera is on (error when it is off). */
  virtual Roe<void> FlipCamera() = 0;
  /** The display turned while the camera is on: later frames rotate to match. */
  virtual void UpdateCameraDisplayRotation(int display_rotation_deg) = 0;

  // --- Video on screen ---
  /** UI tick: clear remote video on a hard frame stall or a failed / closed connection. Before the reads below. */
  virtual void RefreshRemoteVideoHealth() = 0;
  /** A fresh remote decoded frame is available (not stalled / cleared). */
  virtual bool HasRemoteVideo() const = 0;
  virtual bool HasRemoteVideoForStream(uint32_t stream_id) const = 0;
  /** Soft stall: frames aged past the soft threshold but not yet cleared. */
  virtual bool IsRemoteVideoStalling() const = 0;
  /** A remote frame arrived within the last `within_ms`. */
  virtual bool IsRemoteVideoLive(int64_t within_ms) const = 0;
  /** At least one remote frame this media session (survives a hard-stall clear). */
  virtual bool EverHadRemoteVideo() const = 0;
  /** Drop the last remote frame (camera off, leave, hard stall, connection dead). */
  virtual void ClearRemoteVideo() = 0;
  /** Copy the latest local preview / remote decoded frames (false if none). */
  virtual bool CopyLocalVideoFrame(VideoTileFrame& out) const = 0;
  virtual bool CopyRemoteVideoFrame(VideoTileFrame& out) const = 0;
  /** Group: the decoded frame for a publisher stream_id (0 = primary remote). */
  virtual bool CopyRemoteVideoFrameForStream(uint32_t stream_id, VideoTileFrame& out) const = 0;
  /** Drain the stream ids whose video needs a key frame (decode fail / first gap). */
  virtual std::vector<uint32_t> TakePendingVideoRefreshStreamIds() = 0;

  // --- The audio route ---
  /** Reopen capture / playback on the capture worker (speaker route change); no-op when not capturing. */
  virtual void RequestAudioDeviceReopen() = 0;
};

} // namespace pbr
