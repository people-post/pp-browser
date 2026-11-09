#pragma once

#include "lib/messaging/TranscriptAssembly.h"
#include "lib/ui/ChatWidgetTypes.h"
#include "feature/conversations/GroupMentions.h"
#include "feature/conversations/InboxController.h"

#include <optional>
#include <vector>

namespace pbr {

/**
 * A transcript's rows as the chat view binds them: each entry's bubble / card / system line in
 * RML, with its asked question, reaction chips and badges. `facts` names senders, words control
 * lines and says what an attachment card can show. `mentions`: a group's roster, against which the
 * messages' mention / `@everyone` spans are highlighted (G015); none (a direct chat): they show as plain text.
 */
std::vector<MessageDisplayRow> RenderTranscript(const std::vector<TranscriptRow>& entries,
                                                const TranscriptFacts& facts,
                                                const std::optional<MentionRoster>& mentions = std::nullopt);

/** The size a picture shows at in its chat bubble, in dp. */
struct PictureBubbleSize {
  int width_dp = 0;
  int height_dp = 0;
  int image_height_dp = 0;  ///< the drawn picture's height (== height_dp unless cropped)
  int image_offset_dp = 0;  ///< how far above the clip's top the picture starts (0 unless cropped)
};

/** The phone's picture bubble box: about three quarters of a 390dp phone's transcript (Kenneth 2026-11-09: like
 *  WhatsApp, "two thirds to three quarters"), and no taller than about 1.25 of that. */
inline constexpr int kPictureBubbleMaxWidthDp = 292;
inline constexpr int kPictureBubbleMaxHeightDp = 360;
/** A small picture is enlarged just enough for its short side to reach this, never past the box. */
inline constexpr int kPictureBubbleMinShortSideDp = 120;

/**
 * A `width_px` x `height_px` picture laid out for the bubble: scaled to the box width (never enlarged past what its
 * short side needs to reach kPictureBubbleMinShortSideDp; at least 1dp a side). A picture taller than the box keeps
 * the full width and is cropped to the box height, its middle showing (`image_height_dp` > `height_dp`, the picture
 * drawn `image_offset_dp` above the clip). {0, 0} when the size is unknown.
 */
PictureBubbleSize FitPictureInBubble(int width_px, int height_px);

} // namespace pbr
