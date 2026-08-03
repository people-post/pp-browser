#include "app/ChatShellBridge.h"

namespace pbr {

void ChatShellBridge::BindApply(ShellChromeApplyPorts ports) {
  apply_ports_ = std::move(ports);
}

void ChatShellBridge::Clear() {
  apply_ports_ = {};
  synced_ = {};
}

void ChatShellBridge::OnSurface(const ChatSurfaceSnapshot& surface) {
  const ChatShellProjection next = ProjectChatShell(surface);
  const ShellChromeOp update = ClassifyChatChromeUpdate(synced_, next);
  synced_ = next;
  if (update == ShellChromeOp::None) {
    return;
  }
  if (apply_ports_.apply) {
    apply_ports_.apply(update);
  }
}

} // namespace pbr
