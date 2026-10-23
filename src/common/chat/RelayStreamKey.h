#pragma once

#include "common/thread/ThreadChannel.h"

#include <cstdint>
#include <string>

namespace pbr {

/** Canonical opaque stream id for a 1:1 pair + channel + epoch (relay-agnostic). */
std::string BuildCanonicalRelayStreamKey(const std::string& contact_id_a, const std::string& contact_id_b,
                                         ThreadChannel channel, uint32_t session_epoch);
/** The control lane between a 1:1 pair (any epoch): its own relay stream, apart from the chat's. */
std::string BuildControlRelayStreamKey(const std::string& contact_id_a, const std::string& contact_id_b);
/**
 * Opaque group stream scope (D095), one per member: the relay keeps one row per sender, stream and
 * seq, so copies of one message to different members must not share a stream.
 */
std::string BuildGroupRelayStreamKey(const std::string& group_id, uint32_t session_epoch, const std::string& member);

} // namespace pbr
