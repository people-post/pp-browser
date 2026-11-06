#pragma once

#include "feature/conversations/GroupInviteLinkWorkflow.h"
#include "lib/messaging/GroupLinkJoin.h"
#include "lib/messaging/GroupTypes.h"

#include "common/Error.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {

/** One live invite link as group info lists it for the owner and admins (G021): whose, until when, Revoke. */
struct GroupLinkListRow {
  std::string link_id;
  /** "Your link" / "Alice's link". */
  std::string label;
  /** "Expires in 6 days · used 2 of 10". */
  std::string detail;
  bool can_revoke = false;

  bool operator==(const GroupLinkListRow&) const = default;
};

/** "Invite link…" shows for whoever the preset lets make links (G017: Open, any member; Managed, admins). */
bool ShowsInviteLink(MemberRole role, GroupPreset preset);
/** The group's live links are listed (each with Revoke) for those who revoke anyone's: the owner and admins. */
bool ListsGroupLinks(MemberRole role, GroupPreset preset);

/** "Expires in 7 days" / "in 5 hours" / "in 3 minutes" / "Expired". */
std::string GroupLinkExpiryText(int64_t expires_at, int64_t now_ms);

/** The rows, `member_name` naming each maker (its public name), soonest to expire first as given. */
std::vector<GroupLinkListRow> BuildGroupLinkRows(const std::vector<GroupLinkRow>& links,
                                                 const std::function<std::string(const std::string&)>& member_name,
                                                 const std::string& local_account, int64_t now_ms);

/** The preview sheet's words (G021 4, G019 4): `Join "{name}"? {N} members. Joining shares your profile with {N} people.` */
std::string GroupJoinPreviewText(const std::string& title, uint64_t member_count);

/** A refused join, in plain words. */
std::string GroupJoinRefusalText(GroupJoinRefusal reason);

/**
 * No answer from the link's maker: offline (the link names nobody else to ask), or a mistyped or unknown link — a
 * request for a link its maker's devices do not know, or with a wrong secret, gets no reply (G021).
 */
std::string GroupJoinNoAnswerText(const std::string& maker_name);

/** What the join prompt does once a join by link of ours moves on (the GUI shows it; the probe reads the state). */
struct GroupJoinStep {
  enum class Kind : uint8_t {
    /** Still asking (or confirmed, or cancelled): nothing to show. */
    Wait,
    /** The preview sheet: `title`, `text`, Join / Cancel. */
    Preview,
    /** A line saying how it ended (refused, no answer). */
    Tell,
  };
  Kind kind = Kind::Wait;
  std::string title;
  std::string text;
};
GroupJoinStep NextGroupJoinStep(const GroupJoinState& state, const std::string& maker_name);

/** Why asking to join by a link did not start (a link of our own, one that does not read). */
std::string GroupJoinAskFailureText(const Error& error);

} // namespace pbr
