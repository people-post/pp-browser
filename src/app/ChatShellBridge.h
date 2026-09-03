#pragma once

#include "gui/ChatChromeSync.h"
#include "gui/ChatSurfaceSnapshot.h"
#include "gui/shell/ShellChromeApplyPorts.h"

namespace pbr {

class ChatShellBridge {
public:
  void BindApply(ShellChromeApplyPorts ports);
  void Clear();
  void OnSurface(const ChatSurfaceSnapshot& surface);

private:
  ChatShellProjection synced_{};
  ShellChromeApplyPorts apply_ports_;
};

} // namespace pbr
