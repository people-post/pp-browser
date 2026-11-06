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

/** Where along a bar (left edge `left`, `width`, same unit as `x`) a pointer at `x` is, 0..1; 0 for an empty bar. */
double SeekFractionAt(float x, float left, float width);

/** What a sound file's music card shows besides its controls. */
struct AudioCardFace {
  std::string title;      // the file's own title, else its name
  std::string subtitle;   // "Artist · 10.3 MB", or the size alone
  std::string cover_path; // a small JPEG of its embedded picture ("": the music-note placeholder)
  int64_t duration_ms = 0; // its length from its tags (0: unknown, the time stays empty until it plays)
};

/**
 * A sound file sent as an attachment (an MP3), played in place: a small music card. The cover on the left, the title
 * and subtitle (one line each, ellipsised) on the right; under them the play / pause button, a bar that takes the
 * room left and the time in a fixed width ("0:12 / 3:40"), so nothing runs past the card. The button, bar and time
 * keep the voice message's element ids, so VoicePlayback updates them; the click calls play_audio_file.
 */
std::string AudioFileBubbleRml(const std::string& message_id, const std::string& click_arg, const AudioCardFace& face,
                               const VoiceBubbleState& state);

} // namespace pbr
