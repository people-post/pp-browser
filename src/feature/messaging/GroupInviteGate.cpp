#include "feature/messaging/GroupInviteGate.h"

#include "base/people/ContactTypes.h"
#include "common/Utilities.h"
#include "common/PbrCompat.h"

namespace pbr {

GroupInviteGate::GroupInviteGate(ContactsStore& contacts, GroupRosterStore& roster)
    : contacts_(contacts), roster_(roster) {}

void GroupInviteGate::SetInboundPolicy(const GroupInvitePolicy policy) {
  inbound_policy_ = policy;
}

std::optional<Contact> GroupInviteGate::FindContactByIdentity(const std::string& identity) const {
  auto contact = contacts_.FindByIdentity(identity);
  if (!contact || !*contact) {
    return std::nullopt;
  }
  return **contact;
}

Roe<bool> GroupInviteGate::AllowsInboundInvite(const GroupInvitePayload& invite) const {
  if (inbound_policy_ == GroupInvitePolicy::Nobody) {
    return false;
  }
  if (auto contact = FindContactByIdentity(invite.inviter_identity)) {
    if (contact->trust == TrustLevel::Blocked) {
      return false;
    }
  } else if (inbound_policy_ == GroupInvitePolicy::ContactsOnly) {
    return false;
  }
  if (invite.expires_at && *invite.expires_at < util::NowUnixMs()) {
    return false;
  }
  const int64_t since = util::NowUnixMs() - 24LL * 60 * 60 * 1000;
  auto pending_count = roster_.CountPendingInvitesSince(since);
  if (!pending_count) {
    return pending_count.error();
  }
  if (*pending_count >= kMaxPendingInvitesPerDay) {
    return false;
  }
  return true;
}

Roe<bool> GroupInviteGate::CanSendInvite(const MemberRole actor_role) const {
  return RoleHasPermission(actor_role, GroupPermission::kPermInvite);
}

Roe<void> GroupInviteGate::RecordPendingInvite(const PendingGroupInvite& invite) const {
  return roster_.UpsertPendingInvite(invite);
}

} // namespace pbr
