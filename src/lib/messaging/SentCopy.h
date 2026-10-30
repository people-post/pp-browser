#pragma once

#include "common/thread/ThreadRecordTypes.h"

#include "common/Error.h"

#include <optional>
#include <string>

#include "common/PbrCompat.h"

namespace pbr {

/** `control_type` of a sent-message copy to our own devices (multi-device M021 d3). */
inline constexpr const char kSentCopyControlType[] = "sent_copy";

/** Where a message we sent went: a peer account's public chat, or a group. Exactly one is set. */
struct SentCopyTarget {
  std::string peer_account;
  std::string group_id;
};

/** A message one of our devices sent, as its siblings file it. */
struct SentCopy {
  SentCopyTarget target;
  /** Its id, timestamp and content (type, text, payload) as sent; no thread, numbering or delivery. */
  ThreadMessage message;
};

/**
 * Sent-message copies (M021 d3): what one of our devices sent shows on the others. The copy is a control-lane
 * notice to our own account (`ControlLane::NotifySiblings`): one record in our shared mailbox, sealed to our
 * account KEM, signed by the sending device — no session or numbering between siblings. It carries the message's
 * stored row (ChatPayload) and where it went.
 */
namespace sent_copy {

/** Whether a message we sent is copied to our siblings: chat content, not control or reactions / deletes. */
bool Mirrored(const ThreadMessage& sent);
/** The notice for `sent` (a system message with `control_type` sent_copy). */
Roe<ThreadMessage> BuildNotice(const ThreadMessage& sent, const SentCopyTarget& target);
/** The copy a notice carries; nullopt when it is not a sent-copy notice; an error when it is a broken one. */
Roe<std::optional<SentCopy>> Parse(const ThreadMessage& notice);

} // namespace sent_copy

} // namespace pbr
