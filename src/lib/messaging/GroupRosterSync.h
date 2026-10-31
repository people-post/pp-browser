#pragma once

#include "lib/messaging/GroupRosterStatement.h"
#include "lib/messaging/GroupRosterStore.h"
#include "lib/messaging/GroupRosterSyncCodec.h"
#include "common/Error.h"

#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The rules of the owner-signed roster (G012) and of a restored group's member check (G013), over the
 * roster store. The workflow sends; these decide.
 */
namespace group_roster_sync {

/** The statement a group holds, if any. */
std::optional<GroupRosterStatement> Held(const GroupMetadata& metadata);

/**
 * Keep `statement` as the group's latest when it is valid for the owner we know and wins over the one
 * held (Pick: a former owner cannot outbid its successor, then the highest epoch). True when kept.
 */
Roe<bool> KeepStatement(GroupRosterStore& roster, const GroupRosterStatement& statement);

/**
 * Make the roster the statement's — its members (keeping the contact ids we had), its owner and its
 * epoch — and hold it. The caller has verified it.
 */
Roe<void> Adopt(GroupRosterStore& roster, const GroupRosterStatement& statement);

/**
 * A statement from the owner outside a roster event (`group_roster_statement`): kept when it wins, and
 * adopted when it is newer than our roster (we missed the event it follows). True when kept.
 */
Roe<bool> KeepAndCatchUp(GroupRosterStore& roster, const GroupRosterStatement& statement);

/** A member check to send: the request, and who to ask (the owner and the members we know, not us). */
struct Check {
  group_roster_sync_codec::RosterRequest request;
  std::vector<std::string> ask;
};

/** Start (or restart) the group's member check: state checking, a new request id. */
Roe<Check> BeginCheck(GroupRosterStore& roster, const std::string& group_id, const std::string& local_identity,
                      int64_t now_ms);

/**
 * A member's answer to `asker`'s check: the statement it holds, and removed when its roster no longer
 * has the asker. Nullopt when we hold nothing to say (not a group we know). A member still checking
 * itself answers with its statement only (its roster may be stale).
 */
Roe<std::optional<group_roster_sync_codec::RosterReply>> Answer(GroupRosterStore& roster,
                                                                const group_roster_sync_codec::RosterRequest& request,
                                                                const std::string& asker);

enum class ReplyOutcome {
  /** Not for our open check, unverifiable, or it tells us nothing yet. */
  Ignored,
  Active,
  Removed,
};

/**
 * Apply `replier`'s reply to our check (G013): only one for our latest request; a statement must verify
 * for the owner we know. The winning statement becomes the roster when it is not older than ours; the
 * check ends active when it lists us, removed when not. With no statement, only the owner's `removed`
 * counts. Later replies to the same request still apply (the owner's own reply wins when it arrives).
 */
Roe<ReplyOutcome> ApplyReply(GroupRosterStore& roster, const group_roster_sync_codec::RosterReply& reply,
                             const std::string& local_identity, const std::string& replier);

/** An unanswered check is asked again when a group message shows a member is online (G013 amendment). */
inline constexpr int64_t kCheckRetryMs = 15000;

/** Why a group send was refused (i18n keys the GUI words). */
inline constexpr const char kGroupCheckingError[] = "chat.group.checking_members";
inline constexpr const char kGroupRemovedError[] = "chat.group.removed";
/** A group the owner ended (G016) refuses sends. */
inline constexpr const char kGroupDissolvedError[] = "chat.group.dissolved";

/** A group may send only while active (not checking its members, not removed, not dissolved). */
bool MaySend(const GroupMetadata& metadata);

} // namespace group_roster_sync

} // namespace pbr
