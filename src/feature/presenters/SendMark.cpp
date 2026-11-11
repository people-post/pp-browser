#include "feature/presenters/SendMark.h"

namespace pbr {

bool MayShowSendMark(const ThreadMessage& message) {
  if (message.sender_contact_id != kLocalSelfContactId || !message.relay_visible || message.content_rml ||
      IsLocalActionMessage(message)) {
    return false;
  }
  switch (message.content_type) {
    case ChatContentType::Text:
    case ChatContentType::Attachment:
    case ChatContentType::ContactCard:
    case ChatContentType::CryptoTx:
      return true;
    default:
      return false;
  }
}

SendMark MarkOf(const ThreadMessage& message, const std::optional<int64_t> started_ms, const int64_t now_ms,
                const bool no_recipients) {
  if (no_recipients || !MayShowSendMark(message)) {
    return SendMark::None;
  }
  switch (message.delivery) {
    case MessageDelivery::Local:
    case MessageDelivery::Relayed:
      return SendMark::None;
    case MessageDelivery::Pending:
    case MessageDelivery::Failed:
      break;
  }
  if (started_ms) {
    return now_ms - *started_ms <= kQueuedMarkLimitMs ? SendMark::Queued : SendMark::Failed;
  }
  // Only a row still Pending: a Failed one has had its outcome.
  if (message.delivery == MessageDelivery::Pending && now_ms - message.timestamp <= kQueuedMarkGraceMs) {
    return SendMark::Queued;
  }
  return SendMark::Failed;
}

void NoteMarkChange(std::optional<int64_t>& earliest, const ThreadMessage& message,
                    const std::optional<int64_t> started_ms, const int64_t now_ms, const bool no_recipients) {
  if (MarkOf(message, started_ms, now_ms, no_recipients) != SendMark::Queued) {
    return;
  }
  const int64_t at = started_ms ? *started_ms + kQueuedMarkLimitMs + 1 : message.timestamp + kQueuedMarkGraceMs + 1;
  if (!earliest || at < *earliest) {
    earliest = at;
  }
}

bool GroupRowHadRecipients(const std::vector<int64_t>& other_members_joined_at, const int64_t sent_at) {
  for (const int64_t joined_at : other_members_joined_at) {
    if (joined_at <= sent_at) {
      return true;
    }
  }
  return false;
}

std::optional<SendFailure> SendFailureReason(const std::optional<ThreadMessage>& failure_row, const bool group_thread) {
  if (group_thread || !failure_row) {
    return std::nullopt;
  }
  const std::optional<std::string> reason = SendFailureRowReason(*failure_row);
  if (!reason || *reason == kGroupMembersMissedWire) {
    return std::nullopt;
  }
  const std::optional<SendFailure> failure = SendFailureFromWire(*reason);
  if (!failure || *failure == SendFailure::Unknown) {
    return std::nullopt;
  }
  return failure;
}

} // namespace pbr
