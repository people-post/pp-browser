#include "lib/messaging/GroupRosterSyncCodec.h"

#include "common/ValueJson.h"
#include "common/PbrCompat.h"

namespace pbr {
namespace group_roster_sync_codec {
namespace {

Roe<Object> ParseDetail(const std::string& detail_json) {
  auto detail = TryParseObject(detail_json);
  if (!detail) {
    return Error("roster sync: detail is not a JSON object");
  }
  return *detail;
}

} // namespace

std::string EncodeStatement(const GroupRosterStatement& statement) {
  Object detail;
  detail.set("group_id", statement.group_id);
  detail.set("roster_statement", group_roster_statement::ToJson(statement));
  return DumpJson(detail);
}

Roe<GroupRosterStatement> DecodeStatement(const std::string& detail_json) {
  auto detail = ParseDetail(detail_json);
  if (!detail) {
    return detail.error();
  }
  const Object* json = detail->getObject("roster_statement");
  if (!json) {
    return Error("roster sync: no roster_statement");
  }
  auto statement = group_roster_statement::FromJson(*json);
  if (statement && statement->group_id != detail->getString("group_id").value_or("")) {
    return Error("roster sync: statement is for another group");
  }
  return statement;
}

std::string EncodeChange(const GroupChange& change) {
  Object detail;
  detail.set("group_id", change.group_id);
  detail.set("change", group_change::ToJson(change));
  return DumpJson(detail);
}

Roe<GroupChange> DecodeChange(const std::string& detail_json) {
  auto detail = ParseDetail(detail_json);
  if (!detail) {
    return detail.error();
  }
  const Object* json = detail->getObject("change");
  if (!json) {
    return Error("group_change: no change");
  }
  auto change = group_change::FromJson(*json);
  if (change && change->group_id != detail->getString("group_id").value_or("")) {
    return Error("group_change: the change is for another group");
  }
  return change;
}

std::string EncodeRequest(const RosterRequest& request) {
  Object detail;
  detail.set("group_id", request.group_id);
  detail.set("request_id", request.request_id);
  detail.setJsonUInt("roster_epoch", request.roster_epoch);
  detail.setJsonUInt("statement_epoch", request.statement_epoch);
  if (!request.changes_hash.empty()) {
    detail.set("changes_hash", request.changes_hash);
  }
  if (!request.after.empty()) {
    detail.set("after", request.after);
  }
  return DumpJson(detail);
}

Roe<RosterRequest> DecodeRequest(const std::string& detail_json) {
  auto detail = ParseDetail(detail_json);
  if (!detail) {
    return detail.error();
  }
  RosterRequest request;
  request.group_id = detail->getString("group_id").value_or("");
  request.request_id = detail->getString("request_id").value_or("");
  request.roster_epoch = detail->getNonNegInt("roster_epoch").value_or(0);
  request.statement_epoch = detail->getNonNegInt("statement_epoch").value_or(0);
  request.changes_hash = detail->getString("changes_hash").value_or("");
  request.after = detail->getString("after").value_or("");
  if (request.group_id.empty() || request.request_id.empty()) {
    return Error("roster request: missing group_id or request_id");
  }
  return request;
}

std::string EncodeReply(const RosterReply& reply) {
  Object detail;
  detail.set("group_id", reply.group_id);
  detail.set("request_id", reply.request_id);
  if (reply.statement) {
    detail.set("roster_statement", group_roster_statement::ToJson(*reply.statement));
  }
  detail.set("removed", reply.removed);
  if (!reply.changes.empty()) {
    std::vector<Value> changes;
    for (const GroupChange& change : reply.changes) {
      changes.push_back(ObjectValue(group_change::ToJson(change)));
    }
    detail.set("changes", ArrayValue(std::move(changes)));
  }
  if (reply.more) {
    detail.set("more", true);
  }
  return DumpJson(detail);
}

Roe<RosterReply> DecodeReply(const std::string& detail_json) {
  auto detail = ParseDetail(detail_json);
  if (!detail) {
    return detail.error();
  }
  RosterReply reply;
  reply.group_id = detail->getString("group_id").value_or("");
  reply.request_id = detail->getString("request_id").value_or("");
  if (reply.group_id.empty() || reply.request_id.empty()) {
    return Error("roster reply: missing group_id or request_id");
  }
  if (auto removed = detail->getIf<bool>("removed")) {
    reply.removed = *removed;
  }
  if (const Object* json = detail->getObject("roster_statement")) {
    auto statement = group_roster_statement::FromJson(*json);
    if (!statement) {
      return statement.error();
    }
    if (statement->group_id != reply.group_id) {
      return Error("roster reply: statement is for another group");
    }
    reply.statement = std::move(*statement);
  }
  if (const Array* changes = detail->getArray("changes")) {
    for (const Value& item : changes->elements) {
      const Object* json = asObject(item);
      auto change = json ? group_change::FromJson(*json) : Roe<GroupChange>(Error("not an object"));
      if (change && change->group_id == reply.group_id) {
        reply.changes.push_back(std::move(*change)); // one that does not parse is skipped, not the reply
      }
    }
  }
  reply.more = detail->getIf<bool>("more").value_or(false);
  return reply;
}

Roe<std::string> WithStatement(const std::string& detail_json, const GroupRosterStatement& statement) {
  auto detail = ParseDetail(detail_json);
  if (!detail) {
    return detail.error();
  }
  detail->set("roster_statement", group_roster_statement::ToJson(statement));
  return DumpJson(*detail);
}

Roe<std::string> WithTransfer(const std::string& detail_json, const GroupOwnerTransfer& transfer) {
  auto detail = ParseDetail(detail_json);
  if (!detail) {
    return detail.error();
  }
  detail->set("owner_transfer", group_roster_statement::TransferToJson(transfer));
  return DumpJson(*detail);
}

std::optional<GroupRosterStatement> StatementIn(const std::string& detail_json) {
  auto detail = TryParseObject(detail_json);
  const Object* json = detail ? detail->getObject("roster_statement") : nullptr;
  if (!json) {
    return std::nullopt;
  }
  auto statement = group_roster_statement::FromJson(*json);
  if (!statement) {
    return std::nullopt;
  }
  return std::move(*statement);
}

std::optional<GroupOwnerTransfer> TransferIn(const std::string& detail_json) {
  auto detail = TryParseObject(detail_json);
  const Object* json = detail ? detail->getObject("owner_transfer") : nullptr;
  if (!json) {
    return std::nullopt;
  }
  auto transfer = group_roster_statement::TransferFromJson(*json);
  if (!transfer) {
    return std::nullopt;
  }
  return std::move(*transfer);
}

std::optional<std::string> DetailOf(const ThreadMessage& message) {
  auto payload = TryParseObject(message.payload_json);
  if (!payload) {
    return std::nullopt;
  }
  return payload->getString("detail");
}

} // namespace group_roster_sync_codec
} // namespace pbr
