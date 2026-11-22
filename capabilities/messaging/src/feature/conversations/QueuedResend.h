#pragma once

#include "common/thread/IThreadStore.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {

/**
 * Sending a stored message of ours again as itself (B236): the same message id, its copies sealed again for the peer's
 * (or each member's) devices as a first send seals them, so a recipient that has it already drops the copy (the
 * receive pipeline drops a message id it holds: IngestDecision::BenignDuplicate) and one that does not gets it.
 *
 * Allowed for a message of ours sent from this device (a sibling's sent copy is that device's to send), meant for the
 * wire, carrying a send mark (text, picture / file / voice, card, payment), in a public 1:1 chat or a group: their
 * device pairs number without a strict count on the receiving side (D046), so a copy numbered anew is read. A private
 * (e2e) chat's strict numbering is not re-sealed.
 */
bool StoredResendAllowed(const Thread& thread, const ThreadMessage& message);

/** How a tap on the red mark of a picture / file / voice / card message went (ConversationsHub::ResendMessageAsync). */
enum class StoredResendAnswer {
  /** On its way again as itself (the same message id): the row shows queued. */
  Sent,
  /** A send of it is still being tried (or this tap's is starting): nothing more is sent. */
  StillTrying,
  /** The picture / file / voice it carries is no longer on the server (SentRelayFileGone): nothing is sent. */
  AttachmentGone,
};

/** A message still queued this long when the app starts again is no longer sent by itself (a day). */
inline constexpr int64_t kResumeQueuedMaxAgeMs = 24LL * 60 * 60 * 1000;

/** One queued row: its thread and message ids. */
struct QueuedRow {
  std::string thread_id;
  std::string message_id;
  bool operator==(const QueuedRow& other) const {
    return thread_id == other.thread_id && message_id == other.message_id;
  }
};

/** What the start does with the rows still queued when the app last stopped (PlanQueuedResume). */
struct QueuedResumePlan {
  /** Sent again as themselves. */
  std::vector<QueuedRow> resend;
  /** Queued longer than kResumeQueuedMaxAgeMs: failed (the red mark; a tap sends them). */
  std::vector<QueuedRow> give_up;
};

/**
 * The store's outbox after a start (our rows still pending, oldest first): each one StoredResendAllowed, written before
 * this process started (`started_ms`: a newer one is on its way by its own send), that nothing of is under way now
 * (`under_way`: DeliveryOutbox::SendStartedMs) is sent again — in the order the messages were written — or given up
 * when too old at `now_ms`. Anything else is
 * left as it is: a row already failed (not in the outbox: no migration of the old marks), delivered, gone, a system
 * line, a private chat's.
 */
QueuedResumePlan PlanQueuedResume(const IThreadStore& store, int64_t now_ms, int64_t started_ms,
                                  const std::function<bool(const std::string& message_id)>& under_way);

} // namespace pbr
