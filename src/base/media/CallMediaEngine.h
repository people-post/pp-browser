#pragma once

#include "base/media/CallMediaAdaptation.h"
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
 * 1:1 call media via libdatachannel + Opus + SDL (+ platform HW H264, V014/V017/V019).
 * Always negotiates Opus + H264 m-lines; mute/camera are content-only.
 */
class CallMediaEngine : public Module {
public:
  enum class Role { Offerer, Answerer };

  struct LocalDescription {
    std::string type; // "offer" | "answer"
    std::string sdp;
  };

  struct IceCandidate {
    std::string candidate;
    std::string mid;
  };

  struct VideoTileFrame {
    int width = 0;
    int height = 0;
    /** Premultiplied RGBA8. */
    std::vector<uint8_t> rgba;
    uint64_t seq = 0;
  };

  using LocalDescriptionFn = std::function<void(const LocalDescription&)>;
  using IceCandidateFn = std::function<void(const IceCandidate&)>;
  using StateChangedFn = std::function<void(const std::string& state)>;

  CallMediaEngine();
  ~CallMediaEngine() override;

  void SetOnLocalDescription(LocalDescriptionFn callback);
  void SetOnIceCandidate(IceCandidateFn callback);
  void SetOnStateChanged(StateChangedFn callback);

  /**
   * SFU encoded packet (app maps to N021 MediaDataFrame).
   * channel: 0 = audio (reliable_ordered), 1 = video_lo (latest_lossy).
   */
  struct SfuPacket {
    uint16_t channel_id = 0;
    uint32_t seq = 0;
    uint8_t mark = 0;
    std::vector<uint8_t> payload;
  };
  using SfuSendFn = std::function<void(const SfuPacket&)>;

  Roe<void> Start(const std::string& call_id, Role role);
  /** Blind SFU backend: no PeerConnection; capture/encode → SfuSendFn (V021/V024). */
  Roe<void> StartSfu(const std::string& call_id, SfuSendFn send);
  /** Inbound SFU payload (already demuxed to local subscribe). */
  void OnSfuPacket(const SfuPacket& packet);
  bool IsSfuMode() const;

  Roe<void> SetRemoteDescription(const std::string& type, const std::string& sdp);
  Roe<void> AddRemoteIceCandidate(const std::string& candidate, const std::string& mid);
  void Stop();

  void SetMuted(bool muted);
  bool IsMuted() const;

  /** Apply V024 producer decision (camera gate + stored target bps). */
  void ApplyAdaptation(const CallAdaptationDecision& decision);

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
  /** Drop last remote frame (camera off, leave, hard stall, PC dead). */
  void ClearRemoteVideo();
  /**
   * UI-tick health: clear remote video on hard frame stall or failed/closed PC;
   * clear after a short grace when ICE is disconnected.
   */
  void RefreshRemoteVideoHealth();
  bool VideoEncoderAvailable() const;

  bool IsActive() const;
  bool IsConnected() const;
  std::string ActiveCallId() const;
  std::string ConnectionState() const;
  int64_t ConnectedAtMs() const;

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
