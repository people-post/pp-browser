#pragma once

#include "base/messaging/GroupTypes.h"
#include "base/messaging/ThreadTypes.h"

#include "common/Error.h"

#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** Encode/decode group membership system payloads (detail JSON in ChatPayload). */
class GroupMembershipCodec {
public:
  static Roe<std::string> EncodeInvite(const GroupInvitePayload& payload);
  static Roe<GroupInvitePayload> DecodeInvite(const std::string& detail_json);

  static Roe<std::string> EncodeInviteResponse(const std::string& invite_nonce, const std::string& group_id);
  static Roe<std::pair<std::string, std::string>> DecodeInviteResponse(const std::string& detail_json);
  struct InviteResponsePayload {
    std::string invite_nonce;
    std::string group_id;
    GroupMembershipControlType control_type = GroupMembershipControlType::GroupInviteAccept;
  };
  static Roe<InviteResponsePayload> DecodeInviteResponseFromMessage(const ThreadMessage& message);

  struct MemberJoinedEntry {
    std::string member_identity;
    MemberRole role = MemberRole::Member;
  };
  struct MemberJoinedPayload {
    std::string group_id;
    std::string member_identity;
    MemberRole role = MemberRole::Member;
    uint64_t roster_epoch = 0;
    /** Optional full roster snapshot (G006 late-joiner backfill). Empty = primary member only. */
    std::vector<MemberJoinedEntry> members;
  };

  static Roe<std::string> EncodeMemberJoined(const std::string& group_id, const std::string& member_identity,
                                             MemberRole role, uint64_t roster_epoch);
  /** Encode member_joined; optional `members` is a full active-roster snapshot for late joiners. */
  static Roe<std::string> EncodeMemberJoined(const MemberJoinedPayload& payload);
  static Roe<std::string> EncodeMemberLeft(const std::string& group_id, const std::string& member_identity,
                                           uint64_t roster_epoch);
  static Roe<std::string> EncodeMemberRemoved(const std::string& group_id, const std::string& member_identity,
                                              uint64_t roster_epoch);
  static Roe<std::string> EncodeOwnerTransferred(const std::string& group_id, const std::string& new_owner_identity,
                                                 uint64_t roster_epoch, bool leave_previous = false);

  struct MemberLeftPayload {
    std::string group_id;
    std::string member_identity;
    uint64_t roster_epoch = 0;
  };
  struct MemberRemovedPayload {
    std::string group_id;
    std::string member_identity;
    uint64_t roster_epoch = 0;
  };
  struct OwnerTransferredPayload {
    std::string group_id;
    std::string new_owner_identity;
    uint64_t roster_epoch = 0;
    bool leave_previous = false;
  };

  static Roe<MemberJoinedPayload> DecodeMemberJoined(const std::string& detail_json);
  static Roe<MemberJoinedPayload> DecodeMemberJoinedFromMessage(const ThreadMessage& message);
  static Roe<MemberLeftPayload> DecodeMemberLeft(const std::string& detail_json);
  static Roe<MemberLeftPayload> DecodeMemberLeftFromMessage(const ThreadMessage& message);
  static Roe<MemberRemovedPayload> DecodeMemberRemoved(const std::string& detail_json);
  static Roe<MemberRemovedPayload> DecodeMemberRemovedFromMessage(const ThreadMessage& message);
  static Roe<OwnerTransferredPayload> DecodeOwnerTransferred(const std::string& detail_json);
  static Roe<OwnerTransferredPayload> DecodeOwnerTransferredFromMessage(const ThreadMessage& message);

  static Roe<std::string> EncodeGroupRenamed(const std::string& group_id, const std::string& title,
                                            uint64_t roster_epoch);
  struct GroupRenamedPayload {
    std::string group_id;
    std::string title;
    uint64_t roster_epoch = 0;
  };
  static Roe<GroupRenamedPayload> DecodeGroupRenamed(const std::string& detail_json);
  static Roe<GroupRenamedPayload> DecodeGroupRenamedFromMessage(const ThreadMessage& message);
  static Roe<std::string> EncodeGroupForked(const GroupForkPayload& payload);

  static Roe<ThreadMessage> BuildSystemMessage(const std::string& thread_id, GroupMembershipControlType type,
                                               const std::string& display_text, const std::string& detail_json,
                                               const std::string& sender_contact_id);

  static std::vector<TranscriptChatAction> BuildInviteChatActions(const GroupInvitePayload& invite);
  static std::vector<TranscriptChatAction> BuildOwnerUnreachableChatActions(const std::string& group_id,
                                                                            const std::string& owner_identity);
  /** Mark an invite system message as accepted/declined/blocked; clears chat_actions. */
  static void ApplyInviteResolution(ThreadMessage& message, InviteStatus status, const std::string& status_text);
  static std::optional<InviteStatus> InviteResolutionFromMessage(const ThreadMessage& message);

  /** Local advisory card: owner unreachable (payload control_type=group_owner_unreachable). */
  static bool IsOwnerUnreachableAdvisory(const ThreadMessage& message);
  static void ApplyOwnerUnreachableResolution(ThreadMessage& message);
  static bool IsOwnerUnreachableResolved(const ThreadMessage& message);

  static std::optional<GroupMembershipControlType> ControlTypeFromMessage(const ThreadMessage& message);
  static Roe<GroupInvitePayload> DecodeInviteFromMessage(const ThreadMessage& message);

  static Roe<GroupPolicy> DecodeGroupPolicy(const std::string& policy_json);
  static std::string EncodeGroupPolicy(const GroupPolicy& policy);
};

} // namespace pbr
