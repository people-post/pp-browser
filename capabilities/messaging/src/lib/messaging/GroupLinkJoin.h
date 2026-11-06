#pragma once

#include "lib/messaging/GroupInviteLink.h"
#include "lib/messaging/GroupRosterRules.h"
#include "lib/messaging/IGroupRosterStore.h"

#include "common/Error.h"
#include "common/thread/ThreadRecordTypes.h"

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * G021, joining by an invite link: the joiner's device asks the link's maker on the control lane; the maker's device
 * checks the link against the roster it holds and answers with a preview or a refusal; the joiner confirms and the
 * maker's device signs the `add` with the link's id and sends the join bundle (G019). Its own `control_type`s (not
 * membership DMs): a stranger speaks them, never stored, applied once while fresh.
 */
inline constexpr const char kGroupJoinRequestType[] = "group_join_request";
inline constexpr const char kGroupJoinPreviewType[] = "group_join_preview";
inline constexpr const char kGroupJoinRefusedType[] = "group_join_refused";
inline constexpr const char kGroupJoinConfirmType[] = "group_join_confirm";

/** Why a join by link was refused (the wire's `reason`). */
enum class GroupJoinRefusal : uint8_t {
  Expired,
  /** Revoked, or its maker is no longer in the group (the link died with them). */
  Revoked,
  /** It reached its `max_uses`. */
  UsedUp,
  /** The joiner was removed from the group: only an owner's or admin's invite brings them back (G020 5). */
  Removed,
  GroupFull,
  /** The link's answerer took 20 requests for it this hour. */
  Rate,
  /** No such link here, or the secret does not match (the two are not told apart). */
  Unknown,
};

/** `{link_id, secret, request_id}`: the joiner → the link's maker. `secret` is lowercase hex. */
struct GroupJoinRequest {
  std::string request_id;
  std::string link_id;
  ByteVector secret;
};

/**
 * The answerer's preview, once the link checks out: the group's name and member count (G021 4; never the members), and
 * what the joiner needs to take the join bundle — the group's id and owner (the statement must verify for it, as an
 * invite's `owner_identity`) and the answering device (the confirm names it, so one of the maker's devices admits).
 */
struct GroupJoinPreview {
  std::string request_id;
  std::string group_id;
  std::string owner_identity;
  std::string group_title;
  uint64_t member_count = 0;
  std::string device;
};

struct GroupJoinRefused {
  std::string request_id;
  GroupJoinRefusal reason = GroupJoinRefusal::Unknown;
};

/** The joiner says yes to a preview: `device` (the preview's) signs the add. */
struct GroupJoinConfirm {
  std::string request_id;
  std::string device;
};

using GroupJoinMessage = std::variant<GroupJoinRequest, GroupJoinPreview, GroupJoinRefused, GroupJoinConfirm>;

namespace group_link_join {

/** A join request's answerer takes at most this many an hour per link (G021 5). */
inline constexpr size_t kRequestsPerHour = 20;
inline constexpr int64_t kRateWindowMs = 60 * 60 * 1000;
/** The joiner waits this long for an answer (the maker may be offline: G021's stated limit). */
inline constexpr int64_t kAnswerTimeoutMs = 15'000;
/** A preview the joiner has not confirmed within this is forgotten on both sides. */
inline constexpr int64_t kPreviewLifetimeMs = 10 * 60 * 1000;

std::string RefusalToString(GroupJoinRefusal reason);
/** Unknown words read as Unknown. */
GroupJoinRefusal RefusalFromString(const std::string& value);

/** The system message carrying `message` (its `control_type`, a JSON `detail`). */
Roe<ThreadMessage> Build(const GroupJoinMessage& message);
/** Nullopt when `message` is none of the four; an error when it is a broken one. */
Roe<std::optional<GroupJoinMessage>> Parse(const ThreadMessage& message);
/** `message` is one of the four (what the family owns). */
bool IsJoinMessage(const ThreadMessage& message);

/** One join request's facts, as the answering device holds them (JudgeJoin). */
struct JoinCase {
  /** The roster of the group that knows the link (FindLinkGroup); none: no group here knows it. */
  const GroupRosterView* view = nullptr;
  /** This device made the link and keeps it, until when it lives; none: it did not (or forgot it). */
  std::optional<int64_t> made_here_expires_at;
  std::string link_id;
  ByteVector secret;
  std::string joiner;
  std::string local_identity;
  int64_t now_ms = 0;
  /** The rate limiter let this request through. */
  bool within_rate = true;
};

/**
 * G021 4, the answerer's checks, in order: a link known here (else Unknown), the rate (Rate), the secret (Unknown: a
 * wrong secret and no link look the same), revoked or its maker out (Revoked; a link this device made that the
 * roster no longer names — folded away after its revoke, or the group gone from here — reads Revoked, or Expired past
 * its expiry), expired, our
 * own standing (we must be in the group and still allowed to add: else Revoked), its uses (UsedUp), the removed list
 * (Removed) and the size limit (GroupFull; someone already in is let through, they get the bundle again). None: the
 * preview may go.
 */
std::optional<GroupJoinRefusal> JudgeJoin(const JoinCase& join);

/** The group whose roster knows the link `link_id`, live or ended, and that roster; none when no group here does. */
struct LinkGroup {
  std::string group_id;
  GroupRosterView view;
};
std::optional<LinkGroup> FindLinkGroup(IGroupRosterStore& roster, const std::string& link_id,
                                       const GroupCardLookup& cards, int64_t now_ms);

/**
 * The joiner's record of a confirmed join (G021): an accepted invite from `answerer` under the request id, naming the
 * preview's group and owner — so the join bundle is taken from the answerer of a join we confirmed, for it alone, and
 * its statement must verify for that owner (RelayReceivePipeline::ApplyJoinBundle, as for an invite).
 */
PendingGroupInvite JoinRecord(const std::string& request_id, const std::string& group_id, const std::string& title,
                              const std::string& owner_identity, const std::string& answerer,
                              const std::string& local_identity, int64_t now_ms);

/** At most kRequestsPerHour join requests per link in any hour (G021 5); in memory. Thread-safe. */
class JoinRequestLimiter {
public:
  bool Allow(const std::string& link_id, int64_t now_ms);

private:
  std::mutex mutex_;
  std::map<std::string, std::deque<int64_t>> seen_;
};

} // namespace group_link_join

} // namespace pbr
