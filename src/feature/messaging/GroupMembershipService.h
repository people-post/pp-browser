#pragma once

#include "base/messaging/GroupRosterStore.h"
#include "base/messaging/IThreadStore.h"
#include "base/people/ContactsStore.h"
#include "base/people/IdentityStore.h"
#include "feature/messaging/GroupInviteGate.h"
#include "feature/messaging/P2pMessagingService.h"

#include "common/Module.h"

#include <functional>
#include <string>
#include <vector>

namespace pbr {

class GroupMembershipService : public Module {
public:
  GroupMembershipService(IThreadStore& store, ContactsStore& contacts, IdentityStore& identity,
                         GroupRosterStore& roster, GroupInviteGate& invite_gate, P2pMessagingService& p2p);

  void SetInboundPolicy(GroupInvitePolicy policy);

  Roe<Thread> CreateGroup(const std::string& title, const std::vector<std::string>& member_contact_ids);
  Roe<void> InviteMember(const std::string& group_id, const std::string& invitee_contact_id);
  Roe<Thread> AcceptInvite(const std::string& invite_nonce);
  Roe<void> DeclineInvite(const std::string& invite_nonce);
  Roe<void> RemoveMember(const std::string& group_id, const std::string& member_contact_id);
  Roe<void> LeaveGroup(const std::string& group_id);
  Roe<Thread> ForkGroup(const std::string& group_id, const std::string& new_title,
                        const std::vector<std::string>& member_contact_ids);

  Roe<void> HandleInboundInvitePayload(const GroupInvitePayload& invite);
  Roe<std::vector<GroupRosterMember>> ListRoster(const std::string& group_id) const;

private:
  Roe<std::string> ResolveRelayIdentity(const std::string& contact_id) const;
  Roe<std::string> LocalRelayIdentity() const;
  Roe<GroupMetadata> LoadMetadataOrError(const std::string& group_id) const;
  Roe<void> AppendMembershipSystemEvent(const std::string& thread_id, GroupMembershipControlType type,
                                        const std::string& text, const std::string& detail_json,
                                        const std::string& sender_identity);
  Roe<void> SendInviteDirectMessage(const GroupInvitePayload& invite, const std::string& invitee_contact_id);

  IThreadStore& store_;
  ContactsStore& contacts_;
  IdentityStore& identity_;
  GroupRosterStore& roster_;
  GroupInviteGate& invite_gate_;
  P2pMessagingService& p2p_;
};

} // namespace pbr
