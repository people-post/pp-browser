#pragma once

#include "common/thread/ThreadTypes.h"

#include <ui/base/Types.h>

namespace pbr {

void DirtyChatChrome();
void DirtyChatTurns();
void DirtyChatHeader();
void DirtyChat();
void DirtyShell();

/** Sidebar / header visual type: ai | private | public | group */
ui::String SessionVisualKind(const Thread& thread);

} // namespace pbr
