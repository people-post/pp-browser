#pragma once

#include "common/thread/ThreadChannel.h"

#include <cstdint>
#include <string>

namespace pbr {

/** Canonical opaque stream id for a 1:1 pair of accounts + channel + epoch (relay-agnostic). */
std::string BuildCanonicalRelayStreamKey(const std::string& account_a, const std::string& account_b,
                                         ThreadChannel channel, uint32_t session_epoch);
/** The control lane between a 1:1 pair of accounts (any epoch): its own relay stream, apart from the chat's. */
std::string BuildControlRelayStreamKey(const std::string& account_a, const std::string& account_b);
/**
 * The stream of what one sending device writes to an account (M022: one record per message carries every
 * device's copy): the relay keeps one row per sender (its Account ID, shared by an account's devices), stream and
 * index, so each sending device has its own stream.
 */
std::string BuildDeviceRelayStreamKey(const std::string& account_a, const std::string& account_b, ThreadChannel channel,
                                      const std::string& sender_device);
/**
 * The stream of one sending device's copies of a group's messages to one member (group-chat G018 item 7):
 * `v1:g:` + lowercase hex of BLAKE2b-128 over the tag `pp-browser:group-stream-v1`, the group id and the pair's
 * two account ids, lower first (each LenUtf8), then `:from:{sender_device}` as every stream has. Both accounts and
 * all their devices compute it; the relay, which never sees the group id, cannot — and with the pair in the hash,
 * one sender's streams to two members of one group look unrelated.
 */
std::string BuildGroupRelayStreamKey(const std::string& group_id, const std::string& account_a,
                                     const std::string& account_b, const std::string& sender_device);
/** A relay index unique and rising in this process (ms × 1000 + a counter): one per record sent. */
uint64_t NextRelayOrderKey();

} // namespace pbr
