#pragma once

#include <cstdint>
#include <string>

namespace pbr {

/**
 * The time shown above a chat message, in local time and the user's language, relative to `now_ms`:
 * today "18:32", yesterday "Yesterday 18:32", this year "Oct 28, 18:32", older "Oct 28 2025, 18:32".
 * Both arguments are Unix milliseconds (`ThreadMessage::timestamp`). Empty for an unknown time (<= 0).
 */
std::string MessageTimeLabel(int64_t message_ms, int64_t now_ms);

} // namespace pbr
