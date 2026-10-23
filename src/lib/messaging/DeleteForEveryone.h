#pragma once

#include "common/thread/ThreadRecordTypes.h"

namespace pbr {

/**
 * "Delete for everyone" only makes sense for our own message that went out to the peer / group: not a
 * local-only row (a notice, an in-chat @ai question kept on this device), not a system line, not a
 * reaction, not one already deleted.
 */
inline bool CanDeleteForEveryone(const ThreadMessage& message) {
  return message.sender_contact_id == kLocalSelfContactId && message.relay_visible && message.sender_seq.has_value() &&
         message.content_type != ChatContentType::System && message.content_type != ChatContentType::Annotation &&
         message.content_type != ChatContentType::Deleted;
}

} // namespace pbr
