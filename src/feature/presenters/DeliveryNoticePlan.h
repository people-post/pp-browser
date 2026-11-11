#pragma once

#include "common/thread/IThreadStore.h"
#include "common/thread/ThreadTypes.h"
#include "lib/delivery/DeliveryNotice.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

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

/** Messages sent again by a tap (#210 step 2) not yet told as failed: message id -> its thread. */
using ResentMessages = std::unordered_map<std::string, std::string>;

/** What the user is told of a delivery notice besides its line. */
enum class NoticeTell {
  Nothing,
  /** A toast, wherever the user is (no chat to put it in, or an unknown cause). */
  Toast,
};

/**
 * Applies `notice` to `store` (none: messaging not ready) as PlanDeliveryNotice says: its line is written once per
 * message. #210 step 2: a send notice for a message no longer in its chat (an unsent row taken away when it was sent
 * again, its old request ending late) writes nothing and is not told.
 */
NoticeTell ApplyDeliveryNoticeTo(IThreadStore* store, const DeliveryNotice& notice, int64_t now_ms);

/**
 * A message sent again by a tap is told once, when its mark turns red again — whatever turned it (a final failure, or
 * the clock's time over with no failure ever reported: a copy to a peer's device reports none). `red_ids`: the rows of
 * `thread_id` drawn with the red mark now. The first of them still in `resent`; none when there is none.
 */
std::optional<std::string> FirstResentTurnedRed(const ResentMessages& resent, const std::string& thread_id,
                                                const std::vector<std::string>& red_ids);

/** Drops from `resent` the messages delivered by now (their first outcome was not a failure). */
void ForgetDeliveredResends(IThreadStore* store, ResentMessages& resent);

/** The pill for a failed re-sent message shows only in its own chat, open, while the user is looking. */
bool ShowsResentFailurePill(const std::string& notice_thread_id, const std::string& open_thread_id, bool attentive);

} // namespace pbr
