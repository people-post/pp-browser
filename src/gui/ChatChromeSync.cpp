#include "gui/ChatChromeSync.h"

namespace pbr {

ChatShellProjection ProjectChatShell(const ChatSurfaceSnapshot& surface) {
  ChatShellProjection projection;
  projection.has_active_thread = surface.has_active_thread;
  projection.sessions_unread = surface.sessions_unread < 0 ? 0 : surface.sessions_unread;
  return projection;
}

ShellChromeOp ClassifyChatChromeUpdate(const ChatShellProjection& synced, const ChatShellProjection& next) {
  if (synced.has_active_thread != next.has_active_thread) {
    return ShellChromeOp::SyncLayout;
  }
  if (synced.sessions_unread != next.sessions_unread) {
    return ShellChromeOp::DirtyNav;
  }
  return ShellChromeOp::None;
}

} // namespace pbr
