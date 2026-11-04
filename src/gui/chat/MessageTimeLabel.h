#pragma once

#include <cstdint>
#include <string>

namespace pbr {

/**
 * The time shown above a chat message, in local time and the user's language, relative to `now_ms`:
 * today "18:32", yesterday "Yesterday 18:32", this year "Oct 28, 18:32", older "Oct 28, 2025 18:32".
 * Both arguments are Unix milliseconds (`ThreadMessage::timestamp`). Empty for an unknown time (<= 0).
 */
std::string MessageTimeLabel(int64_t message_ms, int64_t now_ms);

/** The clock time alone in local time, "18:32"; the in-bubble time of a message. Empty for an unknown time (<= 0). */
std::string MessageClockLabel(int64_t message_ms);

/**
 * The centred date strip above the first message of a day, relative to `now_ms`: "Today", "Yesterday", "Oct 28",
 * and with the year when it is not this year. Empty for an unknown time (<= 0).
 */
std::string MessageDateLabel(int64_t message_ms, int64_t now_ms);

/** True when both times fall on the same local day. False when either is unknown (<= 0). */
bool SameLocalDay(int64_t a_ms, int64_t b_ms);

} // namespace pbr
