#pragma once

#include "feature/ui/ChatSurfaceSnapshot.h"
#include "feature/ui/ShellChromeOp.h"

namespace pbr {

struct ChatShellProjection {
  bool has_active_thread = false;
  int sessions_unread = 0;
};

ChatShellProjection ProjectChatShell(const ChatSurfaceSnapshot& surface);
ShellChromeOp ClassifyChatChromeUpdate(const ChatShellProjection& synced, const ChatShellProjection& next);

} // namespace pbr
