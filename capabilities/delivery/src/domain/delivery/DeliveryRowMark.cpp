#include "domain/delivery/DeliveryRowMark.h"

#include <optional>

namespace pbr {

RowMarkResult ApplyOutcomeToRow(IThreadTranscript& store, const DeliveryOutcome& outcome, const bool still_under_way) {
  RowMarkResult result;
  if (outcome.thread_id.empty() || outcome.message_id.empty()) {
    return result; // the control lane: no row
  }
  if (outcome.ok) {
    auto marked = store.UpdateMessageDelivery(outcome.thread_id, outcome.message_id, MessageDelivery::Relayed,
                                              outcome.transport, /*unless_delivered=*/false);
    result.changed = marked && *marked;
    return result;
  }
  if (outcome.retrying || still_under_way) {
    return result; // still trying: the row stays queued
  }
  auto row = store.GetMessage(outcome.thread_id, outcome.message_id);
  if (!row || !*row || (*row)->delivery == MessageDelivery::Relayed || (*row)->delivery == MessageDelivery::Failed) {
    return result;
  }
  auto marked = store.UpdateMessageDelivery(outcome.thread_id, outcome.message_id, MessageDelivery::Failed,
                                            std::nullopt, /*unless_delivered=*/true);
  result.changed = marked && *marked;
  result.failed = result.changed;
  return result;
}

} // namespace pbr
