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
};

/** The phone's picture bubble box (Kenneth 2026-11-04: pictures spilled out of their frame). */
inline constexpr int kPictureBubbleMaxWidthDp = 200;
inline constexpr int kPictureBubbleMaxHeightDp = 240;

/**
 * A `width_px` x `height_px` picture fitted inside the bubble box, its shape kept, never enlarged (at least 1dp a
 * side); {0, 0} when the size is unknown.
 */
PictureBubbleSize FitPictureInBubble(int width_px, int height_px);

} // namespace pbr
