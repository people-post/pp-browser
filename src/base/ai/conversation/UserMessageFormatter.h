#pragma once

#include "agent/conversation/ConversationTypes.h"

#include <string>

namespace pbr {

std::string FormatUserContentForLlm(const TranscriptEntry& entry);

} // namespace pbr
