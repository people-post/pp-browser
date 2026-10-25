#include "lib/calls/SoftMigrateLogic.h"

#include "lib/calls/CallMediaPlannerSelectLogic.h"
#include "lib/calls/CallSessionLogic.h"

#include <algorithm>

namespace pbr {

std::string SelectCallInitiator(const std::vector<SoftMigrateJoinedPeer>& joined) {
  const SoftMigrateJoinedPeer* best = nullptr;
  auto earlier = [](const SoftMigrateJoinedPeer& a, const SoftMigrateJoinedPeer& b) {
    if (a.joined_at.has_value() != b.joined_at.has_value()) {
      return a.joined_at.has_value();
    }
    if (a.joined_at && *a.joined_at != *b.joined_at) {
      return *a.joined_at < *b.joined_at;
    }
    return a.identity < b.identity;
  };
  for (const SoftMigrateJoinedPeer& p : joined) {
    if (!p.identity.empty() && (!best || earlier(p, *best))) {
      best = &p;
    }
  }
  return best ? best->identity : std::string();
}

SoftMigrateAction DecideSoftMigrate(const SoftMigrateDecisionInput& in) {
  if (in.already_on_sfu) {
    return SoftMigrateAction::NoOp;
  }
  if (in.local_identity.empty()) {
    return SoftMigrateAction::NoOp;
  }

  switch (in.trigger) {
  case SoftMigrateTrigger::LocalJoinedWithoutHint:
    return SoftMigrateAction::WaitForAttach;

  case SoftMigrateTrigger::RemoteAcceptObserved:
  case SoftMigrateTrigger::JoinedCountObserved:
    // V038/V050: a group forms at 3 joined — re-checked when the decision runs, since the join
    // that triggered it may have been refused or left meanwhile (never SoftMigrate a 1:1).
    if (in.joined_identities.size() < 3) {
      return SoftMigrateAction::NoOp;
    }
    if (!in.sfu_hint_empty) {
      return SoftMigrateAction::WaitForAttach;
    }
    if (!in.initiator_identity.empty() && in.local_identity == in.initiator_identity) {
      return SoftMigrateAction::PickHop;
    }
    return SoftMigrateAction::WaitForAttach;

  case SoftMigrateTrigger::IceRecover: {
    const auto coordinator = CallSessionLogic::SelectEpochCoordinator(in.joined_identities);
    if (coordinator && *coordinator == in.local_identity) {
      return SoftMigrateAction::PickHop;
    }
    return SoftMigrateAction::WaitForAttach;
  }
  }
  return SoftMigrateAction::NoOp;
}

SoftMigrateArming DecideSoftMigrateArming(const SoftMigrateArmingInput& in) {
  if (!in.arming_bound) {
    return SoftMigrateArming::Proceed;
  }
  const bool group = ShouldArmHopPlanner(in.n_joined);
  const bool recover_or_named = in.trigger == SoftMigrateTrigger::IceRecover || in.prefer_named;
  if (in.soft_migrate_may_arm) {
    return group || recover_or_named ? SoftMigrateArming::ArmAndProceed : SoftMigrateArming::StayDirect;
  }
  return in.migrate_ops_allowed ? SoftMigrateArming::Proceed : SoftMigrateArming::NotArmed;
}

RepickOnHopAction DecideRepickOnHop(const RepickOnHopInput& in) {
  if (!in.current_hop.empty() && (in.prefer_hop.empty() || in.prefer_hop == in.current_hop)) {
    return in.prefer_hop == in.current_hop ? RepickOnHopAction::StayOnNamedHop : RepickOnHopAction::KeepCurrentHop;
  }
  const bool named = !in.prefer_hop.empty() && !in.local_peer_id.empty();
  const bool named_self = named && in.prefer_hop == in.local_peer_id;
  const bool named_other = named && in.prefer_hop != in.local_peer_id;
  if ((named_self || in.local_hop_attached) && !named_other) {
    return RepickOnHopAction::KeepPreferLocal;
  }
  if (in.prefer_hop.empty() || !in.prefer_dialable) {
    return RepickOnHopAction::KeepNotDialable;
  }
  return RepickOnHopAction::Detach;
}

HopPickFailure ClassifyHopPickFailures(const std::vector<std::string>& failures) {
  if (failures.empty()) {
    return HopPickFailure::NoneTried;
  }
  auto all = [&](const char* needle) {
    return std::all_of(failures.begin(), failures.end(),
                       [&](const std::string& f) { return f.find(needle) != std::string::npos; });
  };
  if (all("payment_unavailable")) {
    return HopPickFailure::AllPayment;
  }
  return all("hop not dialable") ? HopPickFailure::AllUndialable : HopPickFailure::Other;
}

} // namespace pbr
