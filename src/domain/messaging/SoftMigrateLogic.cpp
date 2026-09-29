#include "domain/messaging/SoftMigrateLogic.h"

#include "domain/messaging/CallSessionLogic.h"

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

} // namespace pbr
