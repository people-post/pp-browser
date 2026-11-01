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
/**
 * The stream of what one sending device writes to an account (M022: one record per message carries every
 * device's copy): the relay keeps one row per sender (`relay:` id, shared by an account's devices), stream and
 * index, so each sending device has its own stream.
 */
std::string BuildDeviceRelayStreamKey(const std::string& contact_id_a, const std::string& contact_id_b,
                                      ThreadChannel channel, const std::string& sender_device);
/** The same for one member's copies of a group's messages. */
std::string BuildGroupDeviceRelayStreamKey(const std::string& group_id, const std::string& member,
                                           const std::string& sender_device);
/** A relay index unique and rising in this process (ms × 1000 + a counter): one per record sent. */
uint64_t NextRelayOrderKey();

} // namespace pbr
