#pragma once

#include "lib/net/BlobClient.h"

#include <cstddef>
#include <cstdint>
#include <string>

#include "common/PbrCompat.h"

namespace pbr {

/** How long the relay keeps a file after its upload completed (RelayFileUrl: 7 days after `complete`). */
inline constexpr int64_t kRelayFileLifetimeMs = 7LL * 24 * 60 * 60 * 1000;
/** The most files one `blob/list` answer names (the relay's cap): a longer list may leave ours out. */
inline constexpr size_t kRelayFileListCap = 500;

/**
 * Whether the server copy a message of ours sent at `sent_at_ms` points at (its attachment payload's share link) is
 * known to be gone at `now_ms`, so sending the message again would give the recipient nothing to download (B236). From
 * `our_files` (our completed files on the relay we upload to; null when not known): listed — gone once its
 * `expires_at` has passed; missing from a complete list — gone (deleted or expired). Not known from the relay — gone
 * past kRelayFileLifetimeMs after the send. False when it cannot tell otherwise (no share link — a direct push carried
 * it), or the payload is not an attachment's. The link's download count (WIRE_SCHEMAS: 10) is not known here.
 */
bool SentRelayFileGone(const std::string& payload_json, int64_t sent_at_ms, int64_t now_ms,
                       const RelayFileListResult* our_files);

} // namespace pbr
