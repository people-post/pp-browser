#pragma once

#include "feature/ui/ChatChromeSync.h"
#include "feature/ui/ChatSurfaceSnapshot.h"
#include "feature/ui/shell/ShellChromeApplyPorts.h"

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
