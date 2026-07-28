#pragma once

#include "common/Error.h"
#include "common/Module.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace pbr {

/**
 * 1:1 voice media via libdatachannel + Opus + SDL (V014).
 * Signaling (SDP/ICE) is emitted via callbacks; CallSessionManager ships them.
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

  bool IsActive() const;
  bool IsConnected() const;
  std::string ActiveCallId() const;
  std::string ConnectionState() const;
  /** Unix ms when media reached Connected; 0 if not connected yet. */
  int64_t ConnectedAtMs() const;

  /** Smoothed peak level 0..1 from local capture (mic pickup indicator). */
  float LocalInputLevel() const;
  /** Smoothed peak level 0..1 from decoded remote frames. */
  float RemoteOutputLevel() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace pbr
