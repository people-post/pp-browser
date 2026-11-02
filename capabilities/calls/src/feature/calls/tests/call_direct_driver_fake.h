#pragma once

#include "feature/calls/CallDirectDriver.h"
#include "feature/calls/CallHopDriver.h"
#include "feature/calls/CallMediaCoordinator.h"

#include <memory>
#include <string>

namespace pbr::test {

/** A 1:1 path that does nothing: fakes derive from it and override what they watch. */
struct NullDirectDriver : CallDirectDriver {
  void ScheduleDirectStart(const std::string&, const std::string&, bool) override {}
  void ReleaseDirectTransport(const CallMediaSeat::Token&) override {}
  void ReleaseDirectTransport() override {}
  void StopMeshMedia(const std::string&) override {}
  Roe<void> RetryMeshMedia(const std::string&) override { return {}; }
  Roe<void> ResumeMeshMediaFromInbound(const std::string&) override { return {}; }
  void Handle(DirectPathEvent&) override {}
  void OnMediaKeyReady(const std::string&) override {}
  CallMediaDirectCallbacks AcceptInbound(const CallMediaInboundHello&) override { return {}; }
  void OnPeerGaveUpReaching(const std::string&) override {}
  void OnLocalNetworkChanged() override {}
  void OnPathPolicyChanged(const std::string&) override {}
  void NotePeerIdRelayMapping(const std::string&, const std::string&) override {}
  CallDirectPathStatus Status() const override { return {}; }
  void PrepareForTeardown(int) override {}
  void DetachReach() override {}
  void AttachReach(const CallDirectPathDeps&) override {}
};

/** A call's 1:1 path that forwards to `target`, a driver the test owns (and outlives the paths). */
class ForwardingDirectDriver final : public CallDirectDriver {
public:
  explicit ForwardingDirectDriver(CallDirectDriver& target) : target_(target) {}
  void ScheduleDirectStart(const std::string& c, const std::string& p, bool o) override {
    target_.ScheduleDirectStart(c, p, o);
  }
  void ReleaseDirectTransport(const CallMediaSeat::Token& t) override { target_.ReleaseDirectTransport(t); }
  void ReleaseDirectTransport() override { target_.ReleaseDirectTransport(); }
  void StopMeshMedia(const std::string& c) override { target_.StopMeshMedia(c); }
  Roe<void> RetryMeshMedia(const std::string& c) override { return target_.RetryMeshMedia(c); }
  Roe<void> ResumeMeshMediaFromInbound(const std::string& c) override { return target_.ResumeMeshMediaFromInbound(c); }
  void Handle(DirectPathEvent& e) override { target_.Handle(e); }
  void OnMediaKeyReady(const std::string& c) override { target_.OnMediaKeyReady(c); }
  CallMediaDirectCallbacks AcceptInbound(const CallMediaInboundHello& h) override { return target_.AcceptInbound(h); }
  void OnPeerGaveUpReaching(const std::string& c) override { target_.OnPeerGaveUpReaching(c); }
  void OnLocalNetworkChanged() override { target_.OnLocalNetworkChanged(); }
  void OnPathPolicyChanged(const std::string& c) override { target_.OnPathPolicyChanged(c); }
  void NotePeerIdRelayMapping(const std::string& p, const std::string& r) override {
    target_.NotePeerIdRelayMapping(p, r);
  }
  CallDirectPathStatus Status() const override { return target_.Status(); }
  void PrepareForTeardown(int ms) override { target_.PrepareForTeardown(ms); }
  void DetachReach() override { target_.DetachReach(); }
  void AttachReach(const CallDirectPathDeps& d) override { target_.AttachReach(d); }

private:
  CallDirectDriver& target_;
};

/** Every call's 1:1 path forwards to `target`. */
inline CallDirectPathFactory ForwardTo(CallDirectDriver& target) {
  return [&target](const std::string&, uint64_t, std::shared_ptr<std::atomic<bool>>) {
    return std::make_unique<ForwardingDirectDriver>(target);
  };
}

/** A group path that does nothing (takes no call): fakes derive from it. */
struct NullHopDriver : CallHopDriver {
  bool OnLocalAcceptJoined(const std::string&, size_t, const std::optional<std::string>&) override { return false; }
  bool OnRemoteAcceptJoined(const std::string&, size_t, const std::string&) override { return false; }
  bool IsSfuAttached() const override { return false; }
  bool IsAwaitingSfuRecovery() const override { return false; }
  bool ExpectsGroupMedia(const std::string&) const override { return false; }
  void BeginSfuAttachWait(const std::string&) override {}
  void OnMediaStopped(const std::string&) override {}
  void RefreshAdaptation(const std::string&, bool) override {}
  void Handle(TopologyEvent&) override {}
  void OnMediaKeyInstalled(const std::string&) override {}
  void OnJoinedCountObserved(const std::string&, size_t) override {}
  void ClearSfuAttachWait() override {}
  Roe<void> OnInboundSfuAttach(const std::string&, const CallSfuAttachDetail&, const std::string&) override {
    return {};
  }
  void OnInboundSfuAttachFailed(const CallSfuAttachFailedDetail&) override {}
  void OnInboundHopRefuse(const CallHopRefuseDetail&) override {}
  bool IsOnSfuForCall(const std::string&) const override { return false; }
  CallRecovery Recovery() const override { return CallRecovery::None; }
  CallHopHealth HopHealth() const override { return {}; }
  void OnPeerMediaRelayCapLearned(const std::string&, const std::string&) override {}
  void OnLocalNetworkMoved() override {}
  void UnwatchRelay() override {}
  void OnRelayDepsChanged() override {}
};

/** A call's group path that forwards the coordinator's questions to `target` (a test-owned fake). */
class ForwardingHopDriver final : public NullHopDriver {
public:
  explicit ForwardingHopDriver(CallHopDriver& target) : target_(target) {}
  bool OnLocalAcceptJoined(const std::string& c, size_t n, const std::optional<std::string>& h) override {
    return target_.OnLocalAcceptJoined(c, n, h);
  }
  bool OnRemoteAcceptJoined(const std::string& c, size_t n, const std::string& j) override {
    return target_.OnRemoteAcceptJoined(c, n, j);
  }
  bool IsSfuAttached() const override { return target_.IsSfuAttached(); }
  bool IsAwaitingSfuRecovery() const override { return target_.IsAwaitingSfuRecovery(); }
  bool ExpectsGroupMedia(const std::string& c) const override { return target_.ExpectsGroupMedia(c); }
  void BeginSfuAttachWait(const std::string& c) override { target_.BeginSfuAttachWait(c); }
  void OnMediaStopped(const std::string& c) override { target_.OnMediaStopped(c); }
  void RefreshAdaptation(const std::string& c, bool on) override { target_.RefreshAdaptation(c, on); }

private:
  CallHopDriver& target_;
};

/** Every call's group path forwards to `target`. */
inline CallHopPathFactory ForwardHopTo(CallHopDriver& target) {
  return [&target](const std::string&, uint64_t, std::shared_ptr<std::atomic<bool>>) {
    return std::make_unique<ForwardingHopDriver>(target);
  };
}

} // namespace pbr::test
