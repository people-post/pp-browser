#pragma once

#include "common/chat/ChatActionTypes.h"

#include <optional>
#include <string>

namespace pbr {

/**
 * Locale key for the button label of a group system card action, chosen by the `type` in its payload;
 * nullopt for any other action. The stored `label` is English and old messages keep it.
 */
std::optional<std::string> ChatActionLabelKey(const TranscriptChatAction& action);

/** What the button shows: the localized label for a known system card action, else the stored label. */
std::string ChatActionLabel(const TranscriptChatAction& action);

} // namespace pbr
