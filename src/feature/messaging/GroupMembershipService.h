#pragma once

#include "base/messaging/GroupMembershipCodec.h"
#include "base/messaging/GroupRosterStore.h"
#include "common/thread/IThreadStore.h"
#include "base/people/ContactsStore.h"
#include "base/people/IdentityStore.h"
#include "feature/messaging/GroupInviteGate.h"
#include "feature/messaging/MeshMessagingService.h"

#include "common/Module.h"

#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

class GroupMembershipService : public Module {
public:
  GroupMembershipService(IThreadStore& store, ContactsStore& contacts, IdentityStore& identity,
                         GroupRosterStore& roster, GroupInviteGate& invite_gate, MeshMessagingService& mesh_messaging);

  void SetInboundPolicy(GroupInvitePolicy policy);

  Roe<Thread> CreateGroup(const std::string& title, const std::vector<std::string>& member_contact_ids);
  Roe<void> InviteMember(const std::string& group_id, const std::string& invitee_contact_id);
  Roe<Thread> AcceptInvite(const std::string& invite_nonce);
  Roe<void> DeclineInvite(const std::string& invite_nonce);
  /** Local leave used by session close — stops inbound group recreate without peer notify. */
  Roe<void> DismissLocalGroup(const std::string& group_id);
  /** Update the DM invite card after accept/decline/block. */
  Roe<void> ResolveInviteCard(const std::string& inviter_identity, const std::string& invite_nonce,
                              InviteStatus status, const std::string& status_text);
  Roe<void> RemoveMember(const std::string& group_id, const std::string& member_contact_id);
  /** Owner prune by communicating identity (unreachable members may lack a contact row). */
  Roe<void> RemoveMemberByIdentity(const std::string& group_id, const std::string& member_identity);
  /** Non-owner leave: fan-out member_left then clear local membership. */
  Roe<void> LeaveGroup(const std::string& group_id);
  /** Owner leave with successor: fan-out owner_transferred (leave_previous) then clear local. */
  Roe<void> LeaveAsOwner(const std::string& group_id, const std::string& new_owner_identity);
  Roe<Thread> ForkGroup(const std::string& group_id, const std::string& new_title,
                        const std::vector<std::string>& member_contact_ids);
  /** Owner-only; updates shared metadata.title and fans out group_renamed DMs. Leaves local_title alone. */
  Roe<void> RenameGroupShared(const std::string& group_id, const std::string& title);
  /** Apply an inbound group_renamed control (from peer DM). */
  Roe<void> ApplyInboundGroupRenamed(const GroupMembershipCodec::GroupRenamedPayload& payload,
                                     const std::string& actor_identity);

  Roe<std::vector<GroupRosterMember>> ListRoster(const std::string& group_id) const;
  /** True when local relay identity is the group metadata owner (authoritative for leave UI). */
  Roe<bool> IsLocalOwner(const std::string& group_id) const;
  /** Metadata owner identity for the group. */
  Roe<std::string> OwnerIdentity(const std::string& group_id) const;
  /**
   * After a validated invite accept on the owner device: append local system line and fan out
   * owner-signed member_joined DMs (peer-facing membership commit).
   */
  Roe<void> PublishMemberJoined(const std::string& group_id, const std::string& member_identity,
                                uint64_t roster_epoch);

  void MarkMemberUnreachable(const std::string& group_id, const std::string& member_identity);
  void ClearMemberUnreachable(const std::string& group_id, const std::string& member_identity);
  bool IsMemberUnreachable(const std::string& group_id, const std::string& member_identity) const;
  std::vector<std::string> ListUnreachable(const std::string& group_id) const;
  bool IsOwnerUnreachable(const std::string& group_id) const;

  /** Insert at most one unresolved owner-unreachable advisory on the group thread. */
  Roe<void> EnsureOwnerUnreachableAdvisory(const std::string& group_id);
  Roe<void> ResolveOwnerUnreachableAdvisory(const std::string& group_id);
  Roe<Thread> OpenOwnerDirectMessage(const std::string& owner_identity);

private:
  Roe<std::string> ResolveRelayIdentity(const std::string& contact_id) const;
  Roe<std::string> LocalRelayIdentity() const;
  Roe<GroupMetadata> LoadMetadataOrError(const std::string& group_id) const;
  Roe<void> AppendMembershipSystemEvent(const std::string& thread_id, GroupMembershipControlType type,
                                        const std::string& text, const std::string& detail_json,
                                        const std::string& sender_identity);
  Roe<void> SendInviteDirectMessage(const GroupInvitePayload& invite, const std::string& invitee_contact_id);
  Roe<void> SendInviteResponseDirectMessage(const std::string& inviter_identity, const std::string& invite_nonce,
                                            const std::string& group_id, GroupMembershipControlType response_type);
  Roe<void> SendMembershipDirectMessage(const std::string& peer_identity, GroupMembershipControlType control_type,
                                        const std::string& detail_json, const std::string& display);
  Roe<void> SendRenameDirectMessage(const std::string& member_identity, const std::string& group_id,
                                    const std::string& title, uint64_t roster_epoch);
  Roe<void> RemoveMemberInternal(const std::string& group_id, const std::string& member_identity);
  Roe<void> FanOutMembershipEvent(const std::string& group_id, GroupMembershipControlType control_type,
                                  const std::string& detail_json, const std::string& display,
                                  const std::string& skip_identity,
                                  const std::optional<std::string>& also_notify_identity = std::nullopt);

  IThreadStore& store_;
  ContactsStore& contacts_;
  IdentityStore& identity_;
  GroupRosterStore& roster_;
  GroupInviteGate& invite_gate_;
  MeshMessagingService& mesh_messaging_;

  mutable std::mutex unreachable_mutex_;
  std::set<std::pair<std::string, std::string>> unreachable_;
};

} // namespace pbr
