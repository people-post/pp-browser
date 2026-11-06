#pragma once

#include "lib/messaging/IGroupRosterStore.h"
#include "lib/messaging/GroupTypes.h"

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

  GroupInviteGate(StandingFn standing, IGroupRosterStore& roster);

  void SetInboundPolicy(GroupInvitePolicy policy);
  GroupInvitePolicy inbound_policy() const { return inbound_policy_; }

  Roe<bool> AllowsInboundInvite(const GroupInvitePayload& invite) const;
  /** True when an invite from this inviter is refused only because they are not a contact (policy: contacts only). */
  bool RefusesUnknownInviter(const std::string& inviter_identity) const;
  Roe<bool> CanSendInvite(MemberRole actor_role) const;
  Roe<void> RecordPendingInvite(const PendingGroupInvite& invite) const;

private:
  StandingFn standing_;
  IGroupRosterStore& roster_;
  GroupInvitePolicy inbound_policy_ = GroupInvitePolicy::ContactsOnly;
};

} // namespace pbr
