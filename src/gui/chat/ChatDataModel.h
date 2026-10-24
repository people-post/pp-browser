#pragma once

#include "common/thread/ThreadTypes.h"
#include "gui/chat/ChatScreenState.h"

#include <ui/base/Types.h>
#include <ui/data/DataModelHandle.h>

namespace pbr {

/** Registers the chat screen's types and binds every field of the "chat" model to `state`. */
void BindChatScreenState(ui::DataModelConstructor& ctor, ChatScreenState& state);
/** The same for the "shell" model's chat part (the chats list, the working set). */
void BindChatShellState(ui::DataModelConstructor& ctor, ChatShellState& state);

void DirtyChatChrome();
void DirtyChatTurns();
void DirtyChatHeader();
void DirtyChat();
void DirtyShell();

/** Sidebar / header visual type: ai | private | public | group */
ui::String SessionVisualKind(const Thread& thread);

} // namespace pbr
