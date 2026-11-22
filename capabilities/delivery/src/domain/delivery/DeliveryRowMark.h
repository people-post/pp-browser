#pragma once

#include "common/thread/IThreadTranscript.h"
#include "domain/delivery/DeliveryOutbox.h"

#include "common/PbrCompat.h"

namespace pbr {

/**
 * What a send's outcome does to the transcript row it carries (B236): a message shows "queued" while the outbox still
 * tries and "failed" only once it gives up, so
 * - a failure the outbox tries again after (`retrying`) leaves the row as it is (queued);
 * - a delivery marks it delivered (one copy or member out is enough);
 * - a final failure marks it failed — unless another request of the same message is still under way
 *   (`still_under_way`: another copy, another member, a re-send), which then decides, and never over a delivered row
 *   nor over one already failed (so it is failed, and its reason told, once).
 */
struct RowMarkResult {
  /** The row's stored state changed. */
  bool changed = false;
  /** It changed to failed now: its reason is told (the "did not go out" line). */
  bool failed = false;
  /** It is delivered now and was not before: the first copy out (what follows a delivery happens once). */
  bool delivered_now = false;
};

RowMarkResult ApplyOutcomeToRow(IThreadTranscript& store, const DeliveryOutcome& outcome, bool still_under_way);

} // namespace pbr
