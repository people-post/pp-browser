#pragma once

#include "base/messaging/ThreadTypes.h"

#include <RmlUi/Core/Types.h>

namespace pbr {

void DirtyChatChrome();
void DirtyChatTurns();
void DirtyChatHeader();
void DirtyChat();
void DirtyShell();

/** Sidebar / header visual type: ai | private | public | group */
Rml::String SessionVisualKind(const Thread& thread);

} // namespace pbr
