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
 * known to be gone at `now_ms`, so sending the message again would give the recipient nothing to download (B236): past
 * the relay's file lifetime, or missing from `our_files` (our completed files on the relay we upload to, null when not
 * known) when that list is complete. False when it cannot tell (no listing, a full one, no share link — a direct push
 * carried it), or the payload is not an attachment's.
 */
bool SentRelayFileGone(const std::string& payload_json, int64_t sent_at_ms, int64_t now_ms,
                       const RelayFileListResult* our_files);

} // namespace pbr
