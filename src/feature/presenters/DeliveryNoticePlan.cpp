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

} // namespace pbr
