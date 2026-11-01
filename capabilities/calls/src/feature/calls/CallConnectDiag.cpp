#include "feature/calls/CallConnectDiag.h"

#include "mesh/reachability/Reachability.h"

#include <algorithm>

namespace pbr {

AddrCounts CountAddrs(const std::vector<std::string>& multiaddrs) {
  AddrCounts counts;
  for (const std::string& ma : multiaddrs) {
    const std::string host = mesh::IpHostFromMultiaddrPrefix(ma);
    if (host.empty()) {
      continue;
    }
    if (ma.rfind("/ip4/", 0) == 0) {
      (mesh::IsPrivateIpv4(host) ? counts.v4_private : counts.v4_public)++;
    } else {
      (mesh::IsGlobalIpv6(host) ? counts.v6_global : counts.v6_other)++;
    }
  }
  return counts;
}

int AddrFamily(const std::string& multiaddr) {
  if (multiaddr.rfind("/ip4/", 0) == 0) {
    return 4;
  }
  if (multiaddr.rfind("/ip6/", 0) == 0) {
    return 6;
  }
  return 0;
}

bool CallDiagBudget::Allow(const std::string_view call_id, const CallDiagEvent event) {
  const std::string key = ShortCallId(call_id);
  std::lock_guard lock(mu_);
  auto it = counts_.find(key);
  if (it == counts_.end()) {
    if (order_.size() >= kCalls) {
      counts_.erase(order_.front());
      order_.erase(order_.begin());
    }
    order_.push_back(key);
    it = counts_.emplace(key, Counts{}).first;
  }
  uint8_t& used = it->second[static_cast<size_t>(event)];
  if (used >= kPerEventPerCall) {
    return false;
  }
  ++used;
  return true;
}

CallDiagBudget& CallDiagBudget::Shared() {
  static CallDiagBudget budget;
  return budget;
}

ShareWord LinkKindWord(const mesh::PeerLinkKind kind) {
  switch (kind) {
  case mesh::PeerLinkKind::Direct:
    return "direct";
  case mesh::PeerLinkKind::Punched:
    return "punched";
  case mesh::PeerLinkKind::Relayed:
    return "relayed";
  case mesh::PeerLinkKind::Unknown:
    break;
  }
  return "unknown";
}

ShareWord ReachErrWord(const mesh::PeerReachCoordinator::Err code) {
  using E = mesh::PeerReachCoordinator::Err;
  switch (code) {
  case E::Ok:
    return "ok";
  case E::Unavailable:
    return "unavailable";
  case E::BadRequest:
    return "bad_request";
  case E::Cancelled:
    return "cancelled";
  case E::NoEndpoint:
    return "no_endpoint";
  case E::DialInBackoff:
    return "dial_backoff";
  case E::DialBusy:
    return "dial_busy";
  case E::NoAnswer:
    return "no_answer";
  case E::Refused:
    return "refused";
  case E::TransportFailed:
    return "transport_failed";
  case E::CircuitFailed:
    return "circuit_failed";
  case E::NotConnected:
    return "not_connected";
  case E::Generic:
    break;
  }
  return "generic";
}

ShareWord ReachWhyWord(const std::string& message) {
  const auto has = [&](const char* needle) { return message.find(needle) != std::string::npos; };
  if (has("endpoint not registered")) {
    return "not_registered";
  }
  if (has("bridge timed out")) {
    return "bridge_timeout";
  }
  if (has("nested timeout")) {
    return "nested_timeout";
  }
  if (has("nested")) {
    return "nested_miss";
  }
  if (has("refused")) {
    return "refused";
  }
  if (has("timed out") || has("timeout")) {
    return "timeout";
  }
  return "other";
}

ShareWord PeerStateWord(const mesh::PeerReachState state) {
  switch (state) {
  case mesh::PeerReachState::Live:
    return "live";
  case mesh::PeerReachState::Down:
    return "down";
  case mesh::PeerReachState::Unknown:
    break;
  }
  return "unknown";
}

ShareWord PeerFailWord(const mesh::PeerReachFailure failure) {
  switch (failure) {
  case mesh::PeerReachFailure::NoAnswer:
    return "no_answer";
  case mesh::PeerReachFailure::Refused:
    return "refused";
  case mesh::PeerReachFailure::KeepaliveLost:
    return "keepalive_lost";
  case mesh::PeerReachFailure::Unreachable:
    return "unreachable";
  case mesh::PeerReachFailure::None:
    break;
  }
  return "none";
}

ShareWord MediaErrWord(const CallMediaErr code) {
  switch (code) {
  case CallMediaErr::Ok:
    return "ok";
  case CallMediaErr::NotStarted:
    return "not_started";
  case CallMediaErr::BadRequest:
    return "bad_request";
  case CallMediaErr::Unreachable:
    return "unreachable";
  case CallMediaErr::StaleLink:
    return "stale_link";
  case CallMediaErr::LinkFailed:
    return "link_failed";
  case CallMediaErr::ChannelFailed:
    return "channel_failed";
  case CallMediaErr::HelloRejected:
    return "hello_rejected";
  case CallMediaErr::Timeout:
    return "timeout";
  case CallMediaErr::Closed:
    return "closed";
  case CallMediaErr::StreamClosed:
    return "stream_closed";
  case CallMediaErr::Aborted:
    return "aborted";
  case CallMediaErr::Generic:
    break;
  }
  return "generic";
}

namespace {

ShareWord Role(const bool offerer) { return offerer ? ShareWord("caller") : ShareWord("callee"); }

void AddCounts(DiagLine& line, const AddrCounts& counts) {
  line.Add("v4_private", counts.v4_private)
      .Add("v4_public", counts.v4_public)
      .Add("v6_global", counts.v6_global)
      .Add("v6_other", counts.v6_other);
}

}  // namespace

DiagLine CallNetLine(const std::string_view call_id, const bool offerer, const CallConnectEnv& env) {
  DiagLine line("call.net");
  line.Call("call", call_id).Add("role", Role(offerer));
  line.Add("known", env.attachment_known)
      .Add("online", env.attachment.online)
      .Add("cellular", env.attachment.cellular)
      .Add("expensive", env.attachment.expensive);
  AddCounts(line, env.local);
  line.Add("relays", env.relays_surface)
      .Add("relays_up", env.relays_connected)
      .Add("relays_down", env.relays_held_down)
      .Add("peer_state", PeerStateWord(env.peer_state))
      .Add("peer_fail", PeerFailWord(env.peer_fail))
      .Add("peer_fails", env.peer_fails_in_row);
  return line;
}

DiagLine CallSignalLine(const ShareWord phase, const std::string_view call_id, const std::vector<std::string>& addrs,
                        const int64_t age_ms, const bool implicit) {
  DiagLine line("call.signal");
  line.Call("call", call_id).Add("phase", phase).Add("age_ms", std::max<int64_t>(age_ms, 0)).Add("implicit", implicit);
  AddCounts(line, CountAddrs(addrs));
  return line;
}

DiagLine CallReachOkLine(const std::string_view call_id, const bool offerer, const int attempt,
                         const mesh::PeerLinkKind kind, const bool reused, const int family, const int64_t ms) {
  DiagLine line("call.reach");
  line.Call("call", call_id)
      .Add("role", Role(offerer))
      .Add("attempt", attempt)
      .Add("ok", true)
      .Add("kind", LinkKindWord(kind))
      .Add("family", family)
      .Add("reused", reused)
      .Add("ms", std::max<int64_t>(ms, 0));
  return line;
}

DiagLine CallReachFailLine(const std::string_view call_id, const bool offerer, const int attempt,
                           const mesh::PeerReachCoordinator::Failure& failure, const int64_t ms) {
  DiagLine line("call.reach");
  line.Call("call", call_id)
      .Add("role", Role(offerer))
      .Add("attempt", attempt)
      .Add("ok", false)
      .Add("err", ReachErrWord(failure.GetCode()))
      .Add("why", ReachWhyWord(failure.message))
      .Add("ms", std::max<int64_t>(ms, 0));
  return line;
}

DiagLine CallRelayLine(const ShareWord dir, const std::string_view call_id, const std::string_view relay_peer) {
  DiagLine line("call.relay");
  line.Call("call", call_id).Add("dir", dir).Ref("relay", relay_peer);
  return line;
}

DiagLine CallPunchLine(const ShareWord phase, const std::string_view call_id, const int window_ms, const size_t addrs,
                       const int64_t ms) {
  DiagLine line("call.punch");
  line.Call("call", call_id)
      .Add("phase", phase)
      .Add("window_ms", window_ms)
      .Add("addrs", addrs)
      .Add("ms", std::max<int64_t>(ms, 0));
  return line;
}

DiagLine CallResultLine(const std::string_view call_id, const CallResultFacts& facts, const CallConnectEnv& env) {
  DiagLine line("call.result");
  line.Call("call", call_id)
      .Add("role", Role(facts.offerer))
      .Add("ok", facts.ok)
      .Add("attempts", facts.attempts)
      .Add("ms", std::max<int64_t>(facts.ms, 0))
      .Add("kind", LinkKindWord(facts.kind))
      .Add("family", facts.family);
  if (!facts.ok) {
    line.Add("err", MediaErrWord(facts.media_err))
        .Add("reach_err", facts.reach_failed ? ReachErrWord(facts.reach_err) : ShareWord("none"))
        .Add("why", facts.why)
        .Add("relays", env.relays_surface)
        .Add("relays_up", env.relays_connected)
        .Add("peer_state", PeerStateWord(env.peer_state))
        .Add("peer_fail", PeerFailWord(env.peer_fail))
        .Add("peer_fails", env.peer_fails_in_row);
  }
  return line;
}

} // namespace pbr
