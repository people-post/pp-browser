#pragma once

#include "common/Module.h"
#include "domain/messaging/CallHopPlan.h"
#include "domain/messaging/CallSessionStore.h"
#include "domain/messaging/CallTypes.h"
#include "feature/calls/CallHopRanking.h"
#include "feature/calls/CallSessionEvents.h"
#include "foundation/runtime/OwnerOutbox.h"
#include "feature/calls/CallTopologyRelayDeps.h"

#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * V050 group hop planning: the initiator plans one hop for everyone it invites; each invitee probes
 * it (and a couple of other ranked hops) while ringing and reports in its CallAccept; at the first
 * group migrate the plan is kept, adjusted once, or the joiner refused; later hop hints skip hops a
 * member reported unreachable. Per-call state is bounded (one ringing / group call in practice).
 * Calls owner.
 */
class CallHopPlanning : public Module {
public:
  /** At the first group migrate. */
  enum class JoinOutcome {
    Proceed,
    /** No hop every participant reaches: refuse the joiner (no migration now). */
    RefuseJoiner,
  };

  CallHopPlanning(CallSessionStore& sessions, const CallHopRanking& ranking);

  void SetMediaRelayDeps(const CallTopologyMediaRelayDeps* deps) { deps_ = deps; }
  /** Where planning reports its probe answers (its parent binds it). */
  void SetOutbox(OwnerOutbox<PlanningEvent> outbox) { outbox_ = std::move(outbox); }
  /** An answer it reported, back from the calls owner's queue. */
  void Handle(PlanningEvent& event);

  /**
   * The hop to plan for everyone the initiator invites (joined or not) — same ranking as SoftMigrate
   * PickHop with the scope inferred from the invitees (unknown → Wide → org seed). No quote, no
   * attach. nullopt when no candidate.
   */
  std::optional<CallPlannedHop> Plan(const std::vector<std::string>& invitees, const std::string& local_identity) const;
  /**
   * Invitee, ringing: quote the planned hop and up to two other ranked hops; the results feed
   * `ReportForAccept`. A private planned-hop MA off our LAN counts as unreachable (the same rule a
   * guest applies to CallSfuAttach). No-op without a planned hop.
   */
  void ProbeInvite(const std::string& call_id);
  /** Planned-hop reachability + reached hops for our CallAccept (unknown while pending). */
  CallHopReport ReportForAccept(const std::string& call_id) const;
  /** Initiator: what a joiner reported in its CallAccept. */
  void NoteAcceptReport(const std::string& call_id, const std::string& identity, const CallHopReport& report);

  /**
   * The first group migrate with these joined remotes: keep the planned hop, make the one adjustment
   * (replaces the session's planned hop), or refuse the joiner. Settled once per call.
   */
  JoinOutcome ResolveAtJoin(const std::string& call_id, const std::string& joiner_identity,
                            const std::vector<std::string>& joined_remotes);
  /** The hops (in order) usable for every one of `members`, per their reports. */
  std::vector<std::string> UsableForMembers(const std::string& call_id, const std::vector<std::string>& members,
                                            const std::vector<std::string>& hops) const;
  /** The call's group hop is no longer settled (its media stopped). */
  void Forget(const std::string& call_id) { resolved_.erase(call_id); }

private:
  void QuoteProbe(const std::string& call_id, const std::string& hop_peer_id, const std::string& hop_multiaddr);
  void OnProbeAnswered(const planning_event::ProbeAnswered& answer);

  CallSessionStore& sessions_;
  const CallHopRanking& ranking_;
  const CallTopologyMediaRelayDeps* deps_ = nullptr;
  /** Invitee: hop PeerId → quote ok (nullopt while in flight), per call. */
  std::unordered_map<std::string, std::map<std::string, std::optional<bool>>> probes_;
  std::unordered_map<std::string, std::string> probe_planned_hop_;
  /** Initiator: joiner identity → its accept report, per call. */
  std::unordered_map<std::string, std::map<std::string, CallHopReport>> accept_reports_;
  /** Calls whose first group hop is settled (planned kept or the one adjustment made). */
  std::unordered_set<std::string> resolved_;
  /** Per call: the invite's probe round (an answer from an earlier round is stale). */
  std::unordered_map<std::string, uint64_t> probe_round_;
  OwnerOutbox<PlanningEvent> outbox_;
};

} // namespace pbr
