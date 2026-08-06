#pragma once

#include "base/media/CallMediaAdaptation.h"
#include "base/media/CallMediaHealth.h"
#include "common/Error.h"
#include "common/Module.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

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

  CallMediaEngine();
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

  /** Blind SFU backend: capture/encode → SfuSendFn (V021/V024). */
  Roe<void> StartSfu(const std::string& call_id, SfuSendFn send);
  /** Inbound SFU payload (already demuxed to local subscribe; plaintext Opus). */
  void OnSfuPacket(const SfuPacket& packet);
  bool IsSfuMode() const;
  void Stop();

  void SetMuted(bool muted);
  bool IsMuted() const;

  /** Apply V024 producer decision (camera gate + Opus target bps). */
  void ApplyAdaptation(const CallAdaptationDecision& decision);

  /** 0..1 receiver/path pressure for adaptation (V032). */
  double PathPressure() const;
  /** Hop/send path observed a drop — raises pressure (V032). */
  void NoteOutboundDrop();
  /** Snapshot for chrome / logs (V032 instrumentation). */
  CallMediaEngineHealth HealthSnapshot() const;

  /** Open/close SDL camera + encode. Best-effort: fails without killing voice (V019). */
  Roe<void> SetCameraEnabled(bool enabled);
  bool IsCameraEnabled() const;
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

  bool IsActive() const;
  bool IsConnected() const;
  /** Update chrome-facing SFU connection state (e.g. libp2p pending direct stream). */
  void SetConnectionState(const std::string& state);
  std::string ActiveCallId() const;
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

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace pbr
