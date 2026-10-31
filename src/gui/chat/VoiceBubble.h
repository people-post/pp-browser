#pragma once

#include "feature/conversations/InboxController.h"
#include "lib/media/VoiceFile.h"

#include <string>

namespace pbr {

/** An id safe to use in markup and element lookups: letters, digits, '-' and '_' only. */
std::string VoiceElementKey(const std::string& message_id);

/** What the bubble's number line says: the length, or "position / length" while it is playing or paused. */
std::string VoiceBubbleTimeText(const VoiceBubbleState& state, int64_t named_duration_ms);

/** Fill width in percent (0..100) for a playing / paused bubble, 0 when idle. */
int VoiceBubbleFillPercent(const VoiceBubbleState& state);

/**
 * The play / pause bubble of a received voice message (markup). `duration_ms`: the length from the file name,
 * already clamped to the voice limit. `click_arg`: the message id escaped for a click handler's argument
 * (the transcript's JsArgInAttribute). `state`: what is playing now, so a redraw keeps the progress.
 */
std::string VoiceBubbleRml(const std::string& message_id, const std::string& click_arg, int64_t duration_ms,
                           const VoiceBubbleState& state);

} // namespace pbr
