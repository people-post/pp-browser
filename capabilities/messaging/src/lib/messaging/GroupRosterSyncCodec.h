#pragma once

#include "lib/messaging/GroupChange.h"
#include "lib/messaging/GroupRosterStatement.h"
#include "common/thread/ThreadTypes.h"
#include "common/Error.h"

#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** Wire for the roster sync controls (G012 / G013) and the statement roster events carry. */
namespace group_roster_sync_codec {

struct RosterRequest {
  std::string group_id;
  std::string request_id;
  /** The roster epoch the asker holds (informational). */
  uint64_t roster_epoch = 0;
  /** G018 item 5: the epoch of the owner's statement the asker holds, and the hash of its unfolded changes. */
  uint64_t statement_epoch = 0;
  std::string changes_hash;
  /** A reply that said `more`: send the changes after this id (in id order). Empty: from the first. */
  std::string after;
};

struct RosterReply {
  std::string group_id;
  std::string request_id;
  /** The latest owner-signed roster the replier holds; none when it holds none yet. */
  std::optional<GroupRosterStatement> statement;
  /** The replier knows the asker is no longer a member. */
  bool removed = false;
  /**
   * G018 item 5: the unfolded changes the replier holds (after the request's `after`, in id order), when its changes
   * hash differs from the asker's; bounded (kMaxReplyChanges, kReplyChangesBudgetBytes).
   */
  std::vector<GroupChange> changes;
  /** More changes than one reply carries: ask again `after` the last one. */
  bool more = false;
};

/** At most this many changes ride one member check reply. */
inline constexpr size_t kMaxReplyChanges = 64;
/** And at most this many bytes of them (a change with its card is ~15 KB; an envelope is at most 256 KB). */
inline constexpr size_t kReplyChangesBudgetBytes = 96 * 1024;

/**
 * G019: who an inviter's `add` brings in, riding the `group_change` that carries it to the members — their account
 * card (as the inviter keeps it; unchecked here: kept once it proves itself and the add counts), so members key the
 * newcomer with no lookup (its Account ID is its relay inbox, multi-relay MR001). Not signed.
 */
struct ChangeNewcomer {
  std::string member_identity;
  std::optional<Object> card;
};

/** `group_change` detail (G018): `{ group_id, change, newcomer? }`, the change as GroupChange JSON. */
std::string EncodeChange(const GroupChange& change, const std::optional<ChangeNewcomer>& newcomer = std::nullopt);
Roe<GroupChange> DecodeChange(const std::string& detail_json);
/** The newcomer a `group_change` detail carries, if any. */
std::optional<ChangeNewcomer> NewcomerIn(const std::string& detail_json);

/** G019: changes carried by a join bundle (`member_joined` to the newcomer), `more` when not all fit. */
struct CarriedChanges {
  std::vector<GroupChange> changes;
  bool more = false;
};
/** A detail with `changes` (and `more`) added. */
Roe<std::string> WithChanges(const std::string& detail_json, const CarriedChanges& carried);
/** The changes a detail carries for `group_id` (one that does not parse is skipped). */
CarriedChanges ChangesIn(const std::string& detail_json, const std::string& group_id);

/** `group_roster_statement` detail: `{ group_id, roster_statement }`. */
std::string EncodeStatement(const GroupRosterStatement& statement);
Roe<GroupRosterStatement> DecodeStatement(const std::string& detail_json);

std::string EncodeRequest(const RosterRequest& request);
Roe<RosterRequest> DecodeRequest(const std::string& detail_json);

std::string EncodeReply(const RosterReply& reply);
Roe<RosterReply> DecodeReply(const std::string& detail_json);

/** A roster event's detail with the owner's newly signed statement added as `roster_statement`. */
Roe<std::string> WithStatement(const std::string& detail_json, const GroupRosterStatement& statement);
/** An `owner_transferred` detail with the giving owner's signed transfer added as `owner_transfer`. */
Roe<std::string> WithTransfer(const std::string& detail_json, const GroupOwnerTransfer& transfer);
/** The statement / transfer an event's detail carries, if any (not checked here). */
std::optional<GroupRosterStatement> StatementIn(const std::string& detail_json);
std::optional<GroupOwnerTransfer> TransferIn(const std::string& detail_json);

/** The `detail` JSON a membership system message carries. */
std::optional<std::string> DetailOf(const ThreadMessage& message);

} // namespace group_roster_sync_codec

} // namespace pbr
