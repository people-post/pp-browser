#include "feature/presenters/DeliveryNoticePlan.h"

namespace pbr {

DeliveryNoticePlan PlanDeliveryNotice(const DeliveryNotice& notice, const int64_t now_ms) {
  DeliveryNoticePlan plan;
  switch (notice.kind) {
    case DeliveryNotice::Kind::ReceiveFailed:
      // With a thread id its line is already in the chat (MeshDeliveryOrchestrator writes it with the notice);
      // without one nothing was written, so the user hears of it as a toast.
      if (notice.thread_id.empty()) {
        plan.action = DeliveryNoticePlan::Action::Toast;
      }
      return plan;
    case DeliveryNotice::Kind::SendFailed:
      if (notice.send == SendFailure::Unknown || notice.thread_id.empty() || notice.message_id.empty()) {
        plan.action = DeliveryNoticePlan::Action::Toast;
        return plan;
      }
      // The line is not drawn any more (#210), but it stays the stored reason a tap on the message's red mark tells
      // (SendFailureReason): keep writing it.
      plan.action = DeliveryNoticePlan::Action::TranscriptRow;
      plan.row = MakeSendFailureRow(notice.thread_id, notice.message_id, SendFailureWord(notice.send).c_str(), now_ms);
      return plan;
    case DeliveryNotice::Kind::GroupMembersMissed:
      if (notice.thread_id.empty() || notice.message_id.empty()) {
        plan.action = DeliveryNoticePlan::Action::Toast;
        return plan;
      }
      plan.action = DeliveryNoticePlan::Action::TranscriptRow;
      plan.row = MakeSendFailureRow(notice.thread_id, notice.message_id, kGroupMembersMissedWire, now_ms);
      return plan;
  }
  return plan;
}

NoticeTell ApplyDeliveryNoticeTo(IThreadStore* store, const DeliveryNotice& notice, const int64_t now_ms) {
  const bool send_notice =
      notice.kind == DeliveryNotice::Kind::SendFailed || notice.kind == DeliveryNotice::Kind::GroupMembersMissed;
  if (send_notice && store && !notice.thread_id.empty() && !notice.message_id.empty()) {
    if (auto present = store->HasMessageId(notice.thread_id, notice.message_id); present && !*present) {
      return NoticeTell::Nothing; // the row was taken away (sent again as a new message): its old request ended late
    }
  }
  const DeliveryNoticePlan plan = PlanDeliveryNotice(notice, now_ms);
  switch (plan.action) {
    case DeliveryNoticePlan::Action::Nothing:
      return NoticeTell::Nothing;
    case DeliveryNoticePlan::Action::Toast:
      return NoticeTell::Toast;
    case DeliveryNoticePlan::Action::TranscriptRow:
      break;
  }
  if (!store || !plan.row) {
    return NoticeTell::Toast;
  }
  // The same failure reported twice (each group member that missed, a retry) is one line.
  if (auto present = store->HasMessageId(plan.row->thread_id, plan.row->id); present && *present) {
    return NoticeTell::Nothing;
  }
  return store->AppendMessage(*plan.row) ? NoticeTell::Nothing : NoticeTell::Toast;
}

std::optional<std::string> FirstResentTurnedRed(const ResentMessages& resent, const std::string& thread_id,
                                                const std::vector<std::string>& red_ids) {
  for (const std::string& id : red_ids) {
    if (const auto it = resent.find(id); it != resent.end() && it->second == thread_id) {
      return id;
    }
  }
  return std::nullopt;
}

void ForgetDeliveredResends(IThreadStore* store, ResentMessages& resent) {
  if (!store) {
    return;
  }
  std::erase_if(resent, [store](const auto& entry) {
    auto row = store->GetMessage(entry.second, entry.first);
    return row && *row && (*row)->delivery == MessageDelivery::Relayed;
  });
}

bool ShowsResentFailurePill(const std::string& notice_thread_id, const std::string& open_thread_id,
                            const bool attentive) {
  return attentive && !notice_thread_id.empty() && notice_thread_id == open_thread_id;
}

} // namespace pbr
