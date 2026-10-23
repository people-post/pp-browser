#pragma once

#include "lib/ai/conversation/ConversationTypes.h"

#include <string>

namespace pbr {

std::string FormatUserContentForLlm(const TranscriptEntry& entry);

} // namespace pbr
