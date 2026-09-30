#include "feature/calls/CallHopPlanning.h"

#include "common/directory/MeshHopDial.h"
#include "domain/mesh/media_plane/MediaRelayAttach.h"
#include "feature/calls/CallsThread.h"

#include "common/PbrCompat.h"

namespace pbr {

namespace {

/** One ringing / group call at a time in practice: keep the per-call maps bounded. */
constexpr size_t kMaxTrackedCalls = 4;
/** Ranked hops an invitee probes besides the planned one. */
constexpr size_t kOtherProbedHops = 2;

} // namespace

CallHopPlanning::CallHopPlanning(CallSessionStore& sessions, const CallHopRanking& ranking)
    : sessions_(sessions), ranking_(ranking) {
  redirectLogger("CallHopPlanning");
}

CallHopPlanning::~CallHopPlanning() {
  probe_self_.Invalidate();
}

std::optional<CallPlannedHop> CallHopPlanning::Plan(const std::vector<std::string>& invitees,
                                                    const std::string& local_identity) const {
  if (!deps_ || !deps_->relay) {
    return std::nullopt;
  }
  std::vector<std::string> remotes;
  for (const std::string& identity : invitees) {
    if (!identity.empty() && identity != local_identity) {
      remotes.push_back(identity);
    }
  }
  const std::string local_peer_id = ranking_.LocalPeerId();
  const std::string local_ma = ranking_.LocalAdvertiseMa(local_peer_id);
  const CallHopScope scope = ranking_.ScopeForPeers(remotes);
  const bool lan_ok = ranking_.LanConfirmedForPeers(remotes);
  const bool prefer_local = deps_->prefer_local_as_hop && deps_->relay->IsStarted() && !local_peer_id.empty();
  const auto ranked = SelectCallMediaHop(ranking_.Ranked(), scope, local_peer_id, prefer_local, local_ma, lan_ok);
  for (const MeshHopCandidate& hop : ranked) {
    if (!hop.peer_id.empty()) {
      CallPlannedHop planned{hop.peer_id, hop.multiaddr.empty() ? ranking_.HopMultiaddr(hop.peer_id) : hop.multiaddr};
      log().info << "planned hop=" << planned.peer_id << " scope=" << static_cast<int>(scope)
                 << " invitees=" << remotes.size();
      return planned;
    }
  }
  return std::nullopt;
}

void CallHopPlanning::ProbeInvite(const std::string& call_id) {
  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value() || !(*session)->planned_hop || !deps_ || !deps_->relay) {
    return;
  }
  const CallPlannedHop planned = *(*session)->planned_hop;
  if (probes_.size() > kMaxTrackedCalls) {
    probes_.clear();
    probe_planned_hop_.clear();
  }
  probes_[call_id].clear();
  probe_planned_hop_[call_id] = planned.peer_id;
  const bool private_off_lan =
      !planned.multiaddr.empty() && MultiaddrHasPrivateIpv4Host(planned.multiaddr) &&
      !(GuestMayDialPrivateHopMa(planned.multiaddr, ranking_.LocalAdvertiseMas()) && deps_->peer_lan_confirmed &&
        deps_->peer_lan_confirmed(planned.peer_id));
  if (private_off_lan) {
    probes_[call_id][planned.peer_id] = false;
  } else {
    QuoteProbe(call_id, planned.peer_id, planned.multiaddr);
  }
  const std::string local_peer_id = ranking_.LocalPeerId();
  size_t others = 0;
  for (const MeshHopCandidate& hop : ranking_.Ranked()) {
    if (others >= kOtherProbedHops) {
      break;
    }
    if (hop.peer_id.empty() || hop.peer_id == planned.peer_id || hop.peer_id == local_peer_id || !hop.dialable) {
      continue;
    }
    QuoteProbe(call_id, hop.peer_id, hop.multiaddr);
    ++others;
  }
}

void CallHopPlanning::QuoteProbe(const std::string& call_id, const std::string& hop_peer_id,
                                 const std::string& hop_multiaddr) {
  probes_[call_id][hop_peer_id] = std::nullopt;
  MediaRelayAttachRequest request;
  request.hop_peer_id = hop_peer_id;
  request.hop_multiaddr = hop_multiaddr;
  request.session_id = call_id;
  request.quote.session_id = call_id;
  request.quote.participants = 3;  // a group is what this hop would serve
  auto done = [this, token = probe_self_.token(), snap = probe_self_.Snapshot(), call_id,
               hop_peer_id](Roe<MediaRelayQuote> quote) {
    const bool ok = quote && quote->ok;
    const std::string error = ok ? std::string() : (quote ? quote->error : quote.error().message);
    CallsThread::Post([this, token, snap, call_id, hop_peer_id, ok, error]() {
      if (!DeferredSelf::Alive(token, snap)) {
        return;
      }
      auto it = probes_.find(call_id);
      if (it != probes_.end()) {
        it->second[hop_peer_id] = ok;
        log().info << "hop probe call_id=" << call_id << " hop=" << hop_peer_id << " ok=" << (ok ? 1 : 0)
                   << (ok ? "" : " err=" + error);
      }
    });
  };
  // Same reach steps as a real attach (register, circuit when not dialable), then quote only.
  QuoteMediaRelayAsync(deps_->AttachPorts(), std::move(request), std::move(done));
}

CallHopReport CallHopPlanning::ReportForAccept(const std::string& call_id) const {
  CallHopReport report;
  auto probes = probes_.find(call_id);
  auto planned = probe_planned_hop_.find(call_id);
  if (probes == probes_.end() || planned == probe_planned_hop_.end()) {
    return report;
  }
  for (const auto& [hop, ok] : probes->second) {
    if (ok) {
      (*ok ? report.reachable_hops : report.unreachable_hops).push_back(hop);
    }
    if (hop == planned->second) {
      report.planned_hop_ok = ok;  // nullopt while the quote is still in flight
    }
  }
  return report;
}

void CallHopPlanning::NoteAcceptReport(const std::string& call_id, const std::string& identity,
                                       const CallHopReport& report) {
  if (call_id.empty() || identity.empty()) {
    return;
  }
  if (accept_reports_.size() > kMaxTrackedCalls && accept_reports_.count(call_id) == 0) {
    accept_reports_.clear();
  }
  accept_reports_[call_id][identity] = report;
}

CallHopPlanning::JoinOutcome CallHopPlanning::ResolveAtJoin(const std::string& call_id,
                                                            const std::string& joiner_identity,
                                                            const std::vector<std::string>& joined_remotes) {
  if (resolved_.count(call_id) != 0) {
    return JoinOutcome::Proceed;  // one resolution (and at most one adjustment) per call
  }
  auto session = sessions_.LoadSession(call_id);
  if (!session || !session->has_value() || !(*session)->planned_hop) {
    return JoinOutcome::Proceed;
  }
  if (resolved_.size() > kMaxTrackedCalls) {
    resolved_.clear();
  }
  GroupHopJoinInput in;
  in.planned_hop = (*session)->planned_hop->peer_id;
  for (const MeshHopCandidate& hop : ranking_.Ranked()) {
    in.ranked_hops.push_back(hop.peer_id);
  }
  if (auto reports = accept_reports_.find(call_id); reports != accept_reports_.end()) {
    for (const std::string& identity : joined_remotes) {
      if (auto it = reports->second.find(identity); it != reports->second.end()) {
        in.reports[identity] = it->second;
      }
    }
  }
  const GroupHopJoinDecision decision = DecideGroupHopAtJoin(in);
  if (decision.action != GroupHopAtJoin::RefuseJoiner) {
    // The group's hop is settled; later joins go through the attach-failure repick instead.
    resolved_.insert(call_id);
  }
  switch (decision.action) {
  case GroupHopAtJoin::UsePlanned:
    return JoinOutcome::Proceed;
  case GroupHopAtJoin::UseAlternative: {
    // The one adjustment replaces the plan (not a re-pick): SoftMigrate ranks the planned hop first.
    log().info << "group hop adjustment call_id=" << call_id << " planned=" << in.planned_hop << " → " << decision.hop
               << " (a participant cannot reach the planned hop)";
    CallSession adjusted = **session;
    adjusted.planned_hop = CallPlannedHop{decision.hop, ranking_.HopMultiaddr(decision.hop)};
    (void)sessions_.UpsertSession(adjusted);
    return JoinOutcome::Proceed;
  }
  case GroupHopAtJoin::RefuseJoiner:
    log().warning << "group hop: no hop every participant reaches — refuse joiner=" << joiner_identity
                  << " call_id=" << call_id << " planned=" << in.planned_hop << " ranked=" << in.ranked_hops.size()
                  << " reports=" << in.reports.size();
    return JoinOutcome::RefuseJoiner;
  }
  return JoinOutcome::Proceed;
}

std::vector<std::string> CallHopPlanning::UsableForMembers(const std::string& call_id,
                                                           const std::vector<std::string>& members,
                                                           const std::vector<std::string>& hops) const {
  auto reports = accept_reports_.find(call_id);
  if (reports == accept_reports_.end()) {
    return hops;
  }
  std::map<std::string, CallHopReport> reported;
  for (const std::string& identity : members) {
    if (auto it = reports->second.find(identity); it != reports->second.end()) {
      reported[identity] = it->second;
    }
  }
  std::vector<std::string> out;
  for (const std::string& hop : hops) {
    if (HopUsableForAllReporters(hop, reported)) {
      out.push_back(hop);
    } else {
      log().info << "hop hint: " << hop << " skipped — a member reported it unreachable call_id=" << call_id;
    }
  }
  return out;
}

} // namespace pbr
