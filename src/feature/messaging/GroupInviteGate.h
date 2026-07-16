#pragma once

#include "base/messaging/GroupRosterStore.h"
#include "base/messaging/GroupTypes.h"
#include "base/people/ContactTypes.h"
#include "base/people/ContactsStore.h"

#include "common/Error.h"

#include <optional>
#include <string>

namespace pbr {

/** G007 — inbound group invite spam controls. */
class GroupInviteGate {
public:
  static constexpr size_t kMaxPendingInvitesPerDay = 20;

  GroupInviteGate(ContactsStore& contacts, GroupRosterStore& roster);

  void SetInboundPolicy(GroupInvitePolicy policy);
  GroupInvitePolicy inbound_policy() const { return inbound_policy_; }

  Roe<bool> AllowsInboundInvite(const GroupInvitePayload& invite) const;
  Roe<bool> CanSendInvite(MemberRole actor_role) const;
  Roe<void> RecordPendingInvite(const PendingGroupInvite& invite) const;

private:
  std::optional<Contact> FindContactByIdentity(const std::string& identity) const;

  ContactsStore& contacts_;
  GroupRosterStore& roster_;
  GroupInvitePolicy inbound_policy_ = GroupInvitePolicy::ContactsOnly;
};

} // namespace pbr
