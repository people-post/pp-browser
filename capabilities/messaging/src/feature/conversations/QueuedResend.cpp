#include "feature/conversations/QueuedResend.h"

namespace pbr {

bool StoredResendAllowed(const Thread& thread, const ThreadMessage& message) {
  if (message.sender_contact_id != kLocalSelfContactId || message.sender_device || !message.relay_visible ||
      message.ai_invoke_mode || message.content_rml) {
    return false;
  }
  switch (message.content_type) {
    case ChatContentType::Text:
    case ChatContentType::Attachment:
    case ChatContentType::ContactCard:
    case ChatContentType::CryptoTx:
      break;
    default:
      return false;
  }
  return (thread.kind == ThreadKind::Direct && thread.channel == ThreadChannel::E2ePublic) ||
         (thread.kind == ThreadKind::Group && thread.group_id.has_value());
}

QueuedResumePlan PlanQueuedResume(const IThreadStore& store, const int64_t now_ms, const int64_t started_ms,
                                  const std::function<bool(const std::string& message_id)>& under_way) {
  QueuedResumePlan plan;
  auto queued = store.ListPendingOutbox();
  if (!queued) {
    return plan;
  }
  for (const auto& [message_id, thread_id] : *queued) {
    if (under_way && under_way(message_id)) {
      continue; // already on its way in this process: never a second send beside it
    }
    auto thread = store.GetThread(thread_id);
    auto row = store.GetMessage(thread_id, message_id);
    if (!thread || !*thread || !row || !*row || (*row)->delivery != MessageDelivery::Pending ||
        (*row)->timestamp >= started_ms || !StoredResendAllowed(**thread, **row)) {
      continue;
    }
    QueuedRow entry{.thread_id = thread_id, .message_id = message_id};
    if (now_ms - (*row)->timestamp > kResumeQueuedMaxAgeMs) {
      plan.give_up.push_back(std::move(entry));
    } else {
      plan.resend.push_back(std::move(entry));
    }
  }
  return plan;
}

} // namespace pbr
