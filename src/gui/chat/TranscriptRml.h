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

} // namespace pbr
