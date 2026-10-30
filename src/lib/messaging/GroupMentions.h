#pragma once

#include "lib/messaging/GroupTypes.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

/** One @mention inside a message text: UTF-8 byte range [start, end), including the '@'. */
struct MentionRange {
  std::string account_id; // empty for the @all range
  uint32_t start = 0;
  uint32_t end = 0;
};

/** The `mentions` / `all` part of a Text message's payload_json (see text_mentions, content type 6). */
struct MentionsPayload {
  std::vector<MentionRange> mentions;
  std::optional<MentionRange> all;
  bool Empty() const { return mentions.empty() && !all; }
};

/** Max mentions in one message (group size cap). */
constexpr size_t kMaxMentionsPerMessage = 50;

/** Reads `mentions` / `all` from a Text payload_json; absent or malformed entries are skipped. No validation. */
MentionsPayload ParseMentionsPayload(const std::string& payload_json);

/** Writes `mentions` (always) and `all` (when present) into a payload JSON string with `text`. */
std::string BuildMentionsPayloadJson(const std::string& text, const MentionsPayload& mentions);

/** Range content check: inside `text`, both ends on UTF-8 boundaries, slice starts with '@'. */
bool MentionRangeFitsText(const std::string& text, uint32_t start, uint32_t end);

/** Ranges to highlight: members only; `all` only when the sender is the roster's Owner. */
std::vector<MentionRange> EligibleMentions(const std::string& payload_json, const std::string& sender_account_id,
                                           const std::vector<GroupRosterMember>& roster);

/** True when self is a mentioned roster member, or `all` is present and the sender is the roster's Owner. */
bool MentionsMe(const std::string& payload_json, const std::string& self_account_id,
                const std::string& sender_account_id, const std::vector<GroupRosterMember>& roster);

} // namespace pbr
