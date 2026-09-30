#include "feature/calls/CallReachSignals.h"

#include "domain/messaging/CallControlCodec.h"
#include "domain/messaging/CallMobility.h"

#include "common/PbrCompat.h"

namespace pbr {

CallReachSignals::CallReachSignals(CallControlClient& control) : control_(control) {
  redirectLogger("CallReachSignals");
  BindExchangePorts();
}

void CallReachSignals::SetCallPorts(CallPorts ports) {
  call_ = std::move(ports);
}

void CallReachSignals::SetMeshPorts(MeshPorts ports) {
  mesh_ = std::move(ports);
}

void CallReachSignals::BindExchangePorts() {
  // The exchange's ports read call_ / mesh_ at use: either side may be (re)bound later.
  SignalingPunchExchange::Ports punch;
  punch.send_offer = [this](const PunchSignal& offer) { return SendPunch(CallControlType::CallPunchOffer, offer); };
  punch.send_answer = [this](const PunchSignal& answer) {
    return SendPunch(CallControlType::CallPunchAnswer, answer);
  };
  punch.burst = [this](const std::vector<std::string>& addrs, int window_ms, SignalingPunchExchange::DoneFn done) {
    if (!mesh_.punch_burst) {
      done(Error("signaling punch burst unavailable"));
      return;
    }
    mesh_.punch_burst(addrs, window_ms, std::move(done));
  };
  punch.register_listen = [this](const std::string& key, const std::vector<std::string>& addrs) {
    if (call_.register_listen) {
      call_.register_listen(key, addrs);
    }
  };
  punch.local_candidates = [this]() {
    std::vector<std::string> addrs = mesh_.local_punch_addrs ? mesh_.local_punch_addrs() : std::vector<std::string>{};
    if (addrs.empty() && call_.local_listen_addrs) {
      addrs = call_.local_listen_addrs();
    }
    return addrs;
  };
  punch.local_peer_id = [this]() { return call_.local_peer_id ? call_.local_peer_id() : std::string{}; };
  // The authenticated signalling sender only: the offer's peer_id is self-declared (PR 249 review).
  punch.may_answer = [this](const std::string& sender_key) {
    return !call_.may_learn_our_address || call_.may_learn_our_address(sender_key);
  };
  punch_.SetPorts(std::move(punch));

  CircuitR1Hint::Ports r1;
  r1.send = [this](const std::string& circuit_r1) {
    auto target = ActiveCallPeer();
    if (!target) {
      log().debug << "circuit R1 pending (no active call peer) r1=" << circuit_r1;
      return false;
    }
    CallCircuitR1Detail detail;
    detail.call_id = target->first;
    detail.circuit_r1 = circuit_r1;
    auto encoded = CallControlCodec::EncodeCircuitR1(detail);
    if (!encoded) {
      return false;
    }
    if (auto sent = control_.SendDirect(target->second, CallControlType::CallCircuitR1, *encoded, ""); !sent) {
      log().warning << "CallCircuitR1 send failed call_id=" << target->first << " err=" << sent.error().message;
      return false;
    }
    log().info << "CallCircuitR1 sent call_id=" << target->first << " peer=" << target->second
               << " r1=" << circuit_r1;
    return true;
  };
  r1.prefer_late_reserve = [this](const std::string& circuit_r1) {
    if (mesh_.prefer_late_reserve) {
      mesh_.prefer_late_reserve(circuit_r1);
    }
  };
  r1_.SetPorts(std::move(r1));
}

std::optional<std::pair<std::string, std::string>> CallReachSignals::ActiveCallPeer() const {
  const auto call_id = call_.active_call_id ? call_.active_call_id() : std::nullopt;
  if (!call_id || call_id->empty() || !call_.call_peer) {
    return std::nullopt;
  }
  auto peer = call_.call_peer(*call_id);
  if (!peer || peer->empty()) {
    return std::nullopt;
  }
  return std::make_pair(*call_id, *peer);
}

bool CallReachSignals::IsActiveCall(const std::string& call_id) const {
  const auto active = call_.active_call_id ? call_.active_call_id() : std::nullopt;
  return active && *active == call_id;
}

Roe<void> CallReachSignals::SendPunch(const CallControlType type, const PunchSignal& signal) {
  auto target = ActiveCallPeer();
  if (!target) {
    return Error("signaling punch: no active call peer");
  }
  CallPunchDetail detail;
  detail.call_id = target->first;
  detail.epoch_id = signal.epoch_id;
  detail.window_ms = signal.window_ms;
  detail.addrs = signal.addrs;
  detail.peer_id = signal.peer_id;
  auto encoded = CallControlCodec::EncodePunch(detail);
  if (!encoded) {
    return encoded.error();
  }
  if (auto sent = control_.SendDirect(target->second, type, *encoded, ""); !sent) {
    return sent.error();
  }
  log().info << (type == CallControlType::CallPunchOffer ? "CallPunchOffer" : "CallPunchAnswer") << " sent call_id=" << target->first
             << " peer=" << target->second << " epoch=" << signal.epoch_id;
  return {};
}

void CallReachSignals::AnnounceCapsUpdate() {
  auto target = ActiveCallPeer();
  const auto caps = call_.local_caps ? call_.local_caps() : std::nullopt;
  if (!target || !caps) {
    return;
  }
  CallCapsUpdateDetail detail;
  detail.call_id = target->first;
  if (call_.local_identity) {
    detail.identity = call_.local_identity();
  }
  detail.caps = *caps;
  auto encoded = CallControlCodec::EncodeCapsUpdate(detail);
  if (!encoded) {
    return;
  }
  if (auto sent = control_.SendDirect(target->second, CallControlType::CallCapsUpdate, *encoded, ""); !sent) {
    log().warning << "caps update send failed call_id=" << target->first << " err=" << sent.error().message;
    return;
  }
  log().info << "caps update sent call_id=" << target->first << " mobility=" << MobilityClassWire(detail.caps.mobility);
}

Roe<void> CallReachSignals::HandleInboundCircuitR1(const std::string& detail_json) {
  auto decoded = CallControlCodec::DecodeCircuitR1(detail_json);
  if (!decoded) {
    return decoded.error();
  }
  if (!IsActiveCall(decoded->call_id)) {
    log().info << "CallCircuitR1 ignored; no matching active call call_id=" << decoded->call_id
               << " r1=" << decoded->circuit_r1;
    return {};
  }
  log().info << "CallCircuitR1 inbound call_id=" << decoded->call_id << " r1=" << decoded->circuit_r1;
  r1_.OnInbound(decoded->circuit_r1);
  return {};
}

Roe<void> CallReachSignals::HandleInboundCapsUpdate(const std::string& detail_json) {
  auto decoded = CallControlCodec::DecodeCapsUpdate(detail_json);
  if (!decoded) {
    return decoded.error();
  }
  if (!IsActiveCall(decoded->call_id)) {
    return {};  // not our live call: nothing to re-plan
  }
  log().info << "caps update inbound call_id=" << decoded->call_id
             << " mobility=" << MobilityClassWire(decoded->caps.mobility);
  if (call_.on_peer_caps) {
    call_.on_peer_caps(decoded->call_id, decoded->caps);
  }
  return {};
}

namespace {

PunchSignal SignalFromDetail(CallPunchDetail detail) {
  PunchSignal signal;
  signal.epoch_id = std::move(detail.epoch_id);
  signal.window_ms = detail.window_ms;
  signal.addrs = std::move(detail.addrs);
  signal.peer_id = std::move(detail.peer_id);
  return signal;
}

} // namespace

Roe<void> CallReachSignals::HandleInboundPunchOffer(const std::string& detail_json,
                                                    const std::string& sender_identity) {
  auto decoded = CallControlCodec::DecodePunch(detail_json);
  if (!decoded) {
    return decoded.error();
  }
  if (!IsActiveCall(decoded->call_id)) {
    log().info << "CallPunchOffer ignored; no matching active call call_id=" << decoded->call_id;
    return {};
  }
  return punch_.OnOffer(SignalFromDetail(std::move(*decoded)), sender_identity);
}

Roe<void> CallReachSignals::HandleInboundPunchAnswer(const std::string& detail_json) {
  auto decoded = CallControlCodec::DecodePunch(detail_json);
  if (!decoded) {
    return decoded.error();
  }
  return punch_.OnAnswer(SignalFromDetail(std::move(*decoded)));
}

} // namespace pbr
