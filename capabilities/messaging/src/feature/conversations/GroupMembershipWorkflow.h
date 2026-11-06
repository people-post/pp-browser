#pragma once

#include "lib/messaging/GroupMembershipCodec.h"
#include "lib/messaging/IGroupRosterStore.h"
#include "lib/messaging/GroupRosterSync.h"
#include "lib/messaging/GroupRosterSyncFamily.h"
#include "lib/people/AccountStateCodec.h"
#include "common/thread/IThreadStore.h"
#include "lib/people/ContactsStore.h"
#include "lib/people/IdentityStore.h"
#include "lib/messaging/GroupInviteGate.h"
#include "feature/conversations/MeshDeliveryOrchestrator.h"
#include "domain/peers/AccountKeyResolver.h"

#include "common/Module.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

class GroupMembershipWorkflow : public Module {
public:
  ~GroupMembershipWorkflow();

  GroupMembershipWorkflow(IThreadStore& store, ContactsStore& contacts, IdentityStore& identity,
                         IGroupRosterStore& roster, GroupInviteGate& invite_gate, MeshDeliveryOrchestrator& mesh_messaging,
                         AccountKeyResolver& account_keys);

  void SetInboundPolicy(GroupInvitePolicy policy);

  Roe<Thread> CreateGroup(const std::string& title, const std::vector<std::string>& member_contact_ids);
  /**
   * G022: a group made during the call `call_id`, we its owner. It starts with us alone and one line ("Group made
   * during a call"); each of `member_identities` (accounts: people met in the call need not be contacts) gets the
   * ordinary invite with `context: call` and the call id. An invite that cannot be sent does not stop the others.
   */
  Roe<Thread> CreateGroupFromCall(const std::string& title, const std::string& call_id,
                                  const std::vector<std::string>& member_identities);
  Roe<void> InviteMember(const std::string& group_id, const std::string& invitee_contact_id);
  /**
   * The ordinary invite to `invitee_identity` (an account: someone met in a call need not be a contact), made during
   * the call `call_id` (G022's call context, so their device takes it from someone it is in that call with). A guest's
   * "Add {name} to the group" in the group's call (calls V062 4).
   */
  Roe<void> InviteFromCall(const std::string& group_id, const std::string& invitee_identity,
                           const std::string& call_id);
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
  /**
   * G016: the owner ends the group for everyone. Signs the dissolve, ends the group here (roster emptied, chat kept
   * read-only with one line), then sends `group_dissolved` to each member — best-effort through the outbox, so a
   * member we cannot reach does not hold the dissolve back. A solo owner has nothing to send. Idempotent.
   */
  Roe<void> DissolveGroup(const std::string& group_id);
  /** G016: the owner ended this group (its chat stays, read-only). */
  bool IsDissolved(const std::string& group_id) const;
  /**
   * G016: send what a dissolve still has to send — each member's `group_dissolved`, each invitee's refusal — through
   * the outbox, forgetting a row once its copy is there; a row that fails is tried again later (30 s, 2 min, 10 min)
   * and at every start. Called when messaging comes up.
   */
  void ResumePendingGroupSends(size_t attempt = 0);
  /**
   * G016: a group ended here without the owner's DM (an answer to our member check, or our own other device): its
   * chat gets the line and the one notice, and invite cards for it are closed.
   */
  void OnGroupDissolvedElsewhere(const GroupDissolvedEnd& end);
  /** G016: the account locker says another device of ours saw this group dissolved — end it here too, if we are in it. */
  void EndDissolvedFromLocker(const AccountStateGroup& group);
  Roe<Thread> ForkGroup(const std::string& group_id, const std::string& new_title,
                        const std::vector<std::string>& member_contact_ids);
  /** Owner-only; updates shared metadata.title and fans out group_renamed DMs. Leaves local_title alone. */
  Roe<void> RenameGroupShared(const std::string& group_id, const std::string& title);
  /**
   * G017, owner-only: make `member_identity` an admin or a member again (never the owner: that is a transfer).
   * Re-signs the roster and fans out `member_role_changed` with it; a role it already has changes nothing.
   */
  Roe<void> SetMemberRole(const std::string& group_id, const std::string& member_identity, MemberRole role);
  /** G017, owner-only: set the group Open or Managed; re-signs the roster and fans out `group_preset_changed`. */
  Roe<void> SetGroupPreset(const std::string& group_id, GroupPreset preset);
  /** The group's preset as this device holds it (G017). */
  Roe<GroupPreset> GroupPresetOf(const std::string& group_id) const;
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
  /** G014: an accept came when the group was full — tell the invitee (`group_invite_refused`, reason group_full). */
  Roe<void> RefuseInvite(const std::string& group_id, const std::string& invite_nonce,
                         const std::string& invitee_identity, const std::string& reason = kInviteRefusedGroupFull);
  Roe<void> PublishMemberJoined(const std::string& group_id, const std::string& member_identity,
                                uint64_t roster_epoch);

  void MarkMemberUnreachable(const std::string& group_id, const std::string& member_identity);
  void ClearMemberUnreachable(const std::string& group_id, const std::string& member_identity);
  bool IsMemberUnreachable(const std::string& group_id, const std::string& member_identity) const;
  std::vector<std::string> ListUnreachable(const std::string& group_id) const;
  bool IsOwnerUnreachable(const std::string& group_id) const;
  /** Invites this device sent for the group that are still waiting (not expired), latest per invitee. */
  std::vector<PendingGroupInvite> ListInvitedWaiting(const std::string& group_id) const;

  /** Insert at most one unresolved owner-unreachable advisory on the group thread. */
  Roe<void> EnsureOwnerUnreachableAdvisory(const std::string& group_id);
  Roe<void> ResolveOwnerUnreachableAdvisory(const std::string& group_id);
  Roe<Thread> OpenOwnerDirectMessage(const std::string& owner_identity);

  /**
   * G012: sign the group's roster as its owner and hold it — with `handed`, the transfer that made us
   * owner, added to the owner chain. Errors when we do not own the group.
   */
  Roe<GroupRosterStatement> SignRoster(const std::string& group_id,
                                       const std::optional<GroupOwnerTransfer>& handed = std::nullopt);
  /** G012: sign the roster and send it to the members (after a member left, or once we took the group over). */
  Roe<void> SignAndSendRoster(const std::string& group_id,
                              const std::optional<GroupOwnerTransfer>& handed = std::nullopt);
  /** G013: answer a member's roster check; as the owner, with a statement signed now. */
  Roe<void> AnswerRosterRequest(const group_roster_sync_codec::RosterRequest& request, const std::string& asker);
  /**
   * G013: check the group's members — ask the owner and every member we know; no sends until answered. A check still
   * unanswered is asked again after each of kRosterReaskDelaysMs (an answer can be lost: sealed for a device the
   * member did not know yet, on a lane that does not resend).
   */
  Roe<void> StartRosterCheck(const std::string& group_id);
  static constexpr int64_t kRosterReaskDelaysMs[] = {15'000, 60'000, 300'000};
  /** Groups as the account locker keeps them (multi-device M023 §4). */
  Roe<std::vector<AccountStateGroup>> StateGroups() const;
  /**
   * A group from the account locker: metadata, roster, the statement held and its chat (no messages),
   * checking its members (G013). The caller then starts the check once messaging is up.
   */
  Roe<void> RestoreGroup(const AccountStateGroup& group);

private:
  Roe<void> StartRosterCheckAttempt(const std::string& group_id, size_t attempt);
  /** Ask again after kRosterReaskDelaysMs[attempt] unless answered, or a newer check (with its own timer) began. */
  void ScheduleRosterReask(const std::string& group_id, const std::string& request_id, size_t attempt);
  Roe<std::string> ResolveRelayIdentity(const std::string& contact_id) const;
  Roe<std::string> LocalRelayIdentity() const;
  Roe<GroupMetadata> LoadMetadataOrError(const std::string& group_id) const;
  Roe<void> AppendMembershipSystemEvent(const std::string& thread_id, GroupMembershipControlType type,
                                        const std::string& text, const std::string& detail_json,
                                        const std::string& sender_identity);
  /**
   * A new group with us as owner and its first line (`first_line_context`: `call` for G022's line). `participants`:
   * the thread's contact ids; `invitee_count`: how many will be invited (the size limit).
   */
  Roe<Thread> StartGroup(const std::string& title, const std::vector<std::string>& participants, size_t invitee_count,
                         const std::string& first_line_context);
  /** One invite to `invitee_identity` (its contact, if known); `call_id`: made during that call (G022). */
  Roe<void> InviteIdentity(const std::string& group_id, const std::string& invitee_identity,
                           const std::string& invitee_contact_id, const std::string& call_id);
  /** The invite DM: to the contact `invitee_contact_id`, else to the invitee's account. */
  Roe<void> SendInviteDirectMessage(const GroupInvitePayload& invite, const std::string& invitee_contact_id);
  Roe<void> SendInviteResponseDirectMessage(const std::string& inviter_identity, const std::string& invite_nonce,
                                            const std::string& group_id, GroupMembershipControlType response_type);
  /** `stored_detail_json`: what our copy of the DM keeps, when it should not hold all of `detail_json`. */
  Roe<void> SendMembershipDirectMessage(const std::string& peer_identity, GroupMembershipControlType control_type,
                                        const std::string& detail_json, const std::string& display,
                                        const std::optional<std::string>& stored_detail_json = std::nullopt);
  /** G013: a roster request / reply, on the control lane. */
  Roe<void> SendRosterControl(const std::string& peer_identity, GroupMembershipControlType control_type,
                              const std::string& detail_json, const std::string& display,
                              const std::string& local_identity);
  Roe<void> SendRenameDirectMessage(const std::string& member_identity, const std::string& group_id,
                                    const std::string& title, uint64_t roster_epoch, uint64_t title_version);
  Roe<void> RemoveMemberInternal(const std::string& group_id, const std::string& member_identity);
  /**
   * The owner's change to the group's settings (a role, the preset): bump the roster epoch, append `display` to the
   * group's chat and fan the event out with the newly signed roster. `apply` makes the change in the store, under the
   * roster lock, and returns false when there is nothing to change.
   */
  Roe<void> ChangeGroupSetting(const std::string& group_id, GroupMembershipControlType type, const std::string& display,
                               const std::function<Roe<bool>(GroupMetadata& metadata)>& apply,
                               const std::function<Roe<std::string>(uint64_t roster_epoch)>& detail);
  struct RosterParts {
    std::vector<GroupRosterStatementMember> rows;
    std::vector<GroupOwnerTransfer> chain;
  };
  /** The members as statement rows and the owner chain (the held one, plus the transfer that made us owner). */
  RosterParts RosterPartsOf(const GroupMetadata& metadata, const std::vector<GroupRosterMember>& members,
                            const std::optional<GroupOwnerTransfer>& handed = std::nullopt) const;
  /** Our account signing key (base64), or why there is none. */
  Roe<std::string> SigningKey() const;
  /** G016: the dissolve statement, signed by us as owner at `epoch`, over `members` as stated. */
  Roe<GroupRosterStatement> SignDissolved(const GroupMetadata& metadata, const std::vector<GroupRosterMember>& members,
                                          uint64_t epoch);
  /** G016: the group's chat gets its one "dissolved" line, and the thread list its preview. */
  void AppendDissolvedLine(const std::string& group_id, const std::string& owner_identity,
                           const std::string& sender_identity);
  /** `detail_json` with the roster we now sign as owner added (G012); unchanged if it cannot be signed. */
  std::string WithSignedRoster(const std::string& group_id, const std::string& detail_json);
  Roe<void> FanOutMembershipEvent(const std::string& group_id, GroupMembershipControlType control_type,
                                  const std::string& detail_json, const std::string& display,
                                  const std::string& skip_identity,
                                  const std::optional<std::string>& also_notify_identity = std::nullopt);

  IThreadStore& store_;
  ContactsStore& contacts_;
  IdentityStore& identity_;
  IGroupRosterStore& roster_;
  GroupInviteGate& invite_gate_;
  MeshDeliveryOrchestrator& mesh_messaging_;
  AccountKeyResolver& account_keys_;

  mutable std::mutex unreachable_mutex_;
  std::set<std::pair<std::string, std::string>> unreachable_;

  /** False once destroyed: the retry one-shots of ResumePendingGroupSends check it before touching `this`. */
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
};

} // namespace pbr
