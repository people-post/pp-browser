#include "feature/conversations/QueuedResend.h"

#include <algorithm>
#include <utility>
#include <vector>

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
  std::vector<std::pair<int64_t, QueuedRow>> resend; // with the time each was written: sent again in that order
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
      resend.emplace_back((*row)->timestamp, std::move(entry));
    }
  }
  // The outbox lists by when each record was last written (a resend rewrites it): the chat's order is the messages'
  // own (B236: they arrive in the order they were sent).
  std::stable_sort(resend.begin(), resend.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  for (auto& [at, row] : resend) {
    plan.resend.push_back(std::move(row));
  }
  return plan;
}

} // namespace pbr
