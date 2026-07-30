#pragma once

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

  Roe<void> Start(const std::string& call_id, Role role);
  Roe<void> SetRemoteDescription(const std::string& type, const std::string& sdp);
  Roe<void> AddRemoteIceCandidate(const std::string& candidate, const std::string& mid);
  void Stop();

  void SetMuted(bool muted);
  bool IsMuted() const;

  /** Open/close SDL camera + encode. Best-effort: fails without killing voice (V019). */
  Roe<void> SetCameraEnabled(bool enabled);
  bool IsCameraEnabled() const;
  bool HasRemoteVideo() const;
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
