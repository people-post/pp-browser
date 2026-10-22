#pragma once

#include "domain/messaging/GroupRosterStore.h"
#include "domain/messaging/GroupTypes.h"

#include "common/Error.h"

#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** G007 — inbound group invite spam controls. */
class GroupInviteGate {
public:
  static constexpr size_t kMaxPendingInvitesPerDay = 20;

  /** How this device stands with an inviter: a contact, blocked, or neither (people's to say). */
  enum class InviterStanding { Unknown, Contact, Blocked };
  using StandingFn = std::function<InviterStanding(const std::string& identity)>;

  GroupInviteGate(StandingFn standing, GroupRosterStore& roster);

  void SetInboundPolicy(GroupInvitePolicy policy);
  GroupInvitePolicy inbound_policy() const { return inbound_policy_; }

  Roe<bool> AllowsInboundInvite(const GroupInvitePayload& invite) const;
  Roe<bool> CanSendInvite(MemberRole actor_role) const;
  Roe<void> RecordPendingInvite(const PendingGroupInvite& invite) const;

private:
  StandingFn standing_;
  GroupRosterStore& roster_;
  GroupInvitePolicy inbound_policy_ = GroupInvitePolicy::ContactsOnly;
};

} // namespace pbr
