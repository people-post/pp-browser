#pragma once

#include "common/thread/ThreadRecordTypes.h"

namespace pbr {

/**
 * "Delete for everyone" only makes sense for our own message that went out to the peer / group: not a
 * local-only row (a notice, an in-chat @ai question kept on this device), not a system line, not a
 * reaction, not one already deleted.
 */
inline bool CanDeleteForEveryone(const ThreadMessage& message) {
  // Went out: numbered here, or sent by another of our devices (a sent copy names that device, M021 d3).
  const bool went_out = message.sender_seq.has_value() || message.sender_device.has_value();
  return message.sender_contact_id == kLocalSelfContactId && message.relay_visible && went_out &&
         message.content_type != ChatContentType::System && message.content_type != ChatContentType::Annotation &&
         message.content_type != ChatContentType::Deleted;
}

} // namespace pbr
