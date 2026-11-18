#include "feature/presenters/SupportAsk.h"

namespace pbr {

std::optional<SupportMessage> SupportMessageOf(const ThreadMessage& message, const SendMark mark) {
  if (message.content_rml || IsLocalActionMessage(message)) {
    return std::nullopt;
  }
  switch (message.content_type) {
    case ChatContentType::Text:
    case ChatContentType::Attachment:
    case ChatContentType::ContactCard:
      break;
    default:
      return std::nullopt;
  }
  return SupportMessage{.own = message.sender_contact_id == kLocalSelfContactId,
                        .text = message.text,
                        .at_ms = message.timestamp,
                        .mark = mark};
}

bool ShouldAutoSend(const std::vector<SupportMessage>& messages, const std::string& question, const int64_t now_ms) {
  for (const SupportMessage& message : messages) {
    if (message.own && message.text == question && message.mark != SendMark::Failed &&
        now_ms - message.at_ms < kSupportRepeatWindowMs) {
      return false;
    }
  }
  return true;
}

SupportWait ReplyState(const std::vector<SupportMessage>& messages, const int64_t now_ms) {
  size_t first_unanswered = messages.size();
  for (size_t i = messages.size(); i > 0; --i) {
    if (!messages[i - 1].own) {
      break;
    }
    first_unanswered = i - 1;
  }
  if (first_unanswered == messages.size()) {
    return SupportWait::Answered;
  }
  for (size_t i = first_unanswered; i < messages.size(); ++i) {
    if (messages[i].mark != SendMark::None) {
      return SupportWait::NotSent;
    }
  }
  return now_ms - messages[first_unanswered].at_ms >= kSupportSlowAfterMs ? SupportWait::Slow : SupportWait::Waiting;
}

bool ShouldReadSupportThread(const bool attentive, const bool panel_shows_thread) {
  return attentive && panel_shows_thread;
}

bool ShouldSuppressNotice(const bool attentive, const bool panel_shows_thread) {
  return ShouldReadSupportThread(attentive, panel_shows_thread);
}

} // namespace pbr
