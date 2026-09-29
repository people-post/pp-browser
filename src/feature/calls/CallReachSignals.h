#pragma once

#include "common/Error.h"
#include "common/Module.h"
#include "domain/mesh/reach/CircuitR1Hint.h"
#include "domain/mesh/reach/SignalingPunchExchange.h"
#include "domain/messaging/CallTypes.h"
#include "feature/calls/CallControlClient.h"

#include <functional>
#include <optional>
#include <utility>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Call-control as the carrier of the mesh's reach signals to the active call's peer: the signaled
 * punch (H012, `SignalingPunchExchange`), our circuit R1 (H011, `CircuitR1Hint`) and our caps after
 * a mobility change (K005). Inbound ones for a call that is not the active one are ignored. Owned by
 * CallSessionManager. Calls owner.
 */
class CallReachSignals : public Module {
public:
  /** The call session side (CallSessionManager binds). */
  struct CallPorts {
    std::function<std::optional<std::string>()> active_call_id;
    /** The 1:1 peer of `call_id` — who the signals go to. */
    std::function<std::optional<std::string>(const std::string& call_id)> call_peer;
    std::function<std::string()> local_identity;
    std::function<std::optional<CallPeerCaps>()> local_caps;
    std::function<std::vector<std::string>()> local_listen_addrs;
    std::function<std::string()> local_peer_id;
    std::function<void(const std::string& key, const std::vector<std::string>& addrs)> register_listen;
    /** The peer's caps changed mid-call. */
    std::function<void(const std::string& call_id, const CallPeerCaps& caps)> on_peer_caps;
  };
  /** The mesh side (the call stack binds). */
  struct MeshPorts {
    std::function<void(const std::vector<std::string>& peer_addrs, int window_ms, SignalingPunchExchange::DoneFn)>
        punch_burst;
    std::function<std::vector<std::string>()> local_punch_addrs;
    std::function<void(const std::string& r1)> prefer_late_reserve;
  };

  explicit CallReachSignals(CallControlClient& control);

  void SetCallPorts(CallPorts ports);
  void SetMeshPorts(MeshPorts ports);

  /** H011: tell the active call's peer our R1 (kept until there is one). */
  void AnnounceCircuitR1(const std::string& r1) { r1_.Announce(r1); }
  void FlushCircuitR1() { r1_.Flush(); }
  /** H012: exchange punch candidates with the active call's peer. */
  void RequestSignalingPunch(const std::string& target_peer_id, const std::vector<std::string>& my_addrs,
                             SignalingPunchExchange::DoneFn done) {
    punch_.Request(target_peer_id, my_addrs, std::move(done));
  }
  /** K005: our caps changed mid-call — tell the active call's peer. */
  void AnnounceCapsUpdate();

  Roe<void> HandleInboundCircuitR1(const std::string& detail_json);
  Roe<void> HandleInboundCapsUpdate(const std::string& detail_json);
  Roe<void> HandleInboundPunchOffer(const std::string& detail_json, const std::string& sender_identity);
  Roe<void> HandleInboundPunchAnswer(const std::string& detail_json);

private:
  /** The active call and its peer; nullopt when there is none. */
  std::optional<std::pair<std::string, std::string>> ActiveCallPeer() const;
  bool IsActiveCall(const std::string& call_id) const;
  Roe<void> SendPunch(CallControlType type, const PunchSignal& signal);
  void BindExchangePorts();

  CallControlClient& control_;
  CallPorts call_;
  MeshPorts mesh_;
  SignalingPunchExchange punch_;
  CircuitR1Hint r1_;
};

} // namespace pbr
