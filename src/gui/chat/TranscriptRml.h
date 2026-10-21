#pragma once

#include "domain/messaging/TranscriptAssembly.h"
#include "domain/ui/ChatWidgetTypes.h"
#include "feature/conversations/InboxController.h"

#include <vector>

namespace pbr {

/**
 * A transcript's rows as the chat view binds them: each entry's bubble / card / system line in
 * RML, with its asked question, reaction chips and badges. `facts` names senders, words control
 * lines and says what an attachment card can show.
 */
std::vector<MessageDisplayRow> RenderTranscript(const std::vector<TranscriptRow>& entries,
                                                const TranscriptFacts& facts);

} // namespace pbr
