#pragma once

#include "feature/calls/CallDirectDriver.h"
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
  return [&target](const std::string&, uint64_t) { return std::make_unique<ForwardingDirectDriver>(target); };
}

} // namespace pbr::test
