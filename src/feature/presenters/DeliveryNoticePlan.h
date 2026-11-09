#pragma once

#include "common/thread/ThreadTypes.h"
#include "lib/delivery/DeliveryNotice.h"

#include <cstdint>
#include <optional>

namespace pbr {

/**
 * Where a delivery notice is told to the user (#182). A send that did not go out is a line in the chat it
 * belongs to, under the message (a toast would say it a second time, away from the chat); a notice with no
 * chat, or whose cause is unknown, is a toast; an unreadable inbound message already has its line (written
 * with the notice), so it is neither.
 */
struct DeliveryNoticePlan {
  enum class Action { Nothing, Toast, TranscriptRow };
  Action action = Action::Nothing;
  /** TranscriptRow: the system row to store (stored without words; the transcript words it). */
  std::optional<ThreadMessage> row;
};

DeliveryNoticePlan PlanDeliveryNotice(const DeliveryNotice& notice, int64_t now_ms);

} // namespace pbr
