#pragma once

#include "lib/messaging/GroupRosterStatement.h"
#include "common/thread/ThreadTypes.h"
#include "common/Error.h"

#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** Wire for the roster sync controls (G012 / G013) and the statement roster events carry. */
namespace group_roster_sync_codec {

struct RosterRequest {
  std::string group_id;
  std::string request_id;
  /** The roster epoch the asker holds (informational). */
  uint64_t roster_epoch = 0;
};

struct RosterReply {
  std::string group_id;
  std::string request_id;
  /** The latest owner-signed roster the replier holds; none when it holds none yet. */
  std::optional<GroupRosterStatement> statement;
  /** The replier knows the asker is no longer a member. */
  bool removed = false;
};

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
