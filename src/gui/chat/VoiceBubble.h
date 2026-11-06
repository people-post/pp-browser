#pragma once

#include "feature/conversations/InboxController.h"
#include "lib/voice/VoiceFile.h"

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

/**
 * A sound file sent as an attachment (an MP3), played in place: its name, then the same play / pause button, bar and
 * time line as a voice message (the same element ids, so VoicePlayback updates it). `size_text`: what the time line
 * says before it has played (the file's size); the click calls play_audio_file.
 */
std::string AudioFileBubbleRml(const std::string& message_id, const std::string& click_arg, const std::string& name,
                               const std::string& size_text, const VoiceBubbleState& state);

} // namespace pbr
