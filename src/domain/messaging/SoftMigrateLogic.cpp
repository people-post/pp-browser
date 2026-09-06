#include "domain/messaging/SoftMigrateLogic.h"

#include "domain/messaging/CallSessionLogic.h"

namespace pbr {

std::string SelectCallInitiator(const std::vector<SoftMigrateJoinedPeer>& joined) {
  std::string best;
  int64_t best_at = 0;
  bool have_stamp = false;
  for (const SoftMigrateJoinedPeer& p : joined) {
    if (p.identity.empty()) {
      continue;
    }
    if (p.joined_at) {
      if (!have_stamp || *p.joined_at < best_at) {
        best_at = *p.joined_at;
        best = p.identity;
        have_stamp = true;
      }
    } else if (!have_stamp && best.empty()) {
      best = p.identity;
    }
  }
  return best;
}

SoftMigrateAction DecideSoftMigrate(const SoftMigrateDecisionInput& in) {
  if (in.is_broadcast) {
    return SoftMigrateAction::NoOp;
  }
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
