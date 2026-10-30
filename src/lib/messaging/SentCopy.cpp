#include "lib/messaging/SentCopy.h"

#include "lib/messaging/ChatPayloadCodec.h"

#include "foundation/crypto/CryptoUtil.h"
#include "common/Utilities.h"
#include "common/ValueJson.h"

namespace pbr {
namespace sent_copy {

bool Mirrored(const ThreadMessage& sent) {
  switch (sent.content_type) {
    case ChatContentType::System:
    case ChatContentType::Annotation:  // reactions and deletes apply effects on arrival: later
      return false;
    default:
      return !sent.id.empty();
  }
}

Roe<ThreadMessage> BuildNotice(const ThreadMessage& sent, const SentCopyTarget& target) {
  if (target.peer_account.empty() == target.group_id.empty()) {
    return Error("sent copy: one target, a peer account or a group");
  }
  auto row = ChatPayloadCodec::EncodeToRow(sent);
  if (!row) {
    return row.error();
  }
  // A system payload carries `control_type` and one `detail` string on the wire (ChatPayloadCodec): the copy's
  // fields go in the detail, as JSON.
  Object detail;
  if (!target.peer_account.empty()) {
    detail.set("to_account", target.peer_account);
  } else {
    detail.set("to_group", target.group_id);
  }
  detail.set("message_id", sent.id);
  detail.set("timestamp", sent.timestamp);
  detail.set("row_b64", Base64Encode(*row));
  Object payload;
  payload.set("control_type", kSentCopyControlType);
  payload.set("detail", DumpJson(detail));
  ThreadMessage notice;
  notice.id = util::GenerateUuid();
  notice.content_type = ChatContentType::System;
  notice.text = "Sent copy";
  notice.payload_json = DumpJson(payload);
  notice.timestamp = util::NowUnixMs();
  return notice;
}

Roe<std::optional<SentCopy>> Parse(const ThreadMessage& notice) {
  if (notice.content_type != ChatContentType::System) {
    return std::optional<SentCopy>{};
  }
  auto payload = TryParseObject(notice.payload_json);
  if (!payload || payload->getString("control_type").value_or("") != kSentCopyControlType) {
    return std::optional<SentCopy>{};
  }
  auto detail = TryParseObject(payload->getString("detail").value_or(""));
  if (!detail) {
    return Error("sent copy: malformed");
  }
  SentCopy copy;
  copy.target.peer_account = detail->getString("to_account").value_or("");
  copy.target.group_id = detail->getString("to_group").value_or("");
  copy.message.id = detail->getString("message_id").value_or("");
  copy.message.timestamp = detail->getIf<int64_t>("timestamp").value_or(0);
  auto row = Base64Decode(detail->getString("row_b64").value_or(""));
  if (copy.target.peer_account.empty() == copy.target.group_id.empty() || copy.message.id.empty() || !row ||
      row->empty()) {
    return Error("sent copy: malformed");
  }
  if (auto applied = ChatPayloadCodec::ApplyRowToMessage(*row, copy.message); !applied) {
    return applied.error();
  }
  if (!Mirrored(copy.message)) {
    return Error("sent copy: not chat content");
  }
  return std::optional<SentCopy>(std::move(copy));
}

} // namespace sent_copy
} // namespace pbr
