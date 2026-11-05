#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * One attachment (or AI-question image) of a chat whose bytes this device holds, for Me → Storage → Manage storage.
 * One per (chat, content hash): the newest message that refers to it.
 */
struct StoredAttachmentItem {
  std::string thread_id;
  std::string message_id;
  std::vector<uint8_t> content_hash;
  std::string mime;
  std::string filename;
  uint64_t bytes = 0;            // actual bytes held on this device (CAS), never the sender's declared size
  int64_t timestamp_ms = 0;
  bool has_server_copy = false;  // fields.url non-empty
};

/** What a local delete did: items taken off, bytes freed, items whose bytes stay because something else uses them. */
struct StoredAttachmentDeleteResult {
  size_t deleted = 0;
  uint64_t freed_bytes = 0;
  size_t kept_shared = 0;
};

/** An item to delete or weigh: (chat, content hash). */
using StoredAttachmentRef = std::pair<std::string, std::vector<uint8_t>>;

} // namespace pbr
