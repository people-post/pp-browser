#include "feature/ui/ShellChromeApplyPorts.h"

#include "feature/ui/ShellHost.h"

namespace pbr {

ShellChromeApplyPorts MakeShellChromeApplyPorts(ShellHost& shell, std::string sync_reason) {
  ShellChromeApplyPorts ports;
  ports.apply = [&shell, reason = std::move(sync_reason)](const ShellChromeOp op) {
    switch (op) {
    case ShellChromeOp::None:
      return;
    case ShellChromeOp::DirtyNav:
      shell.DirtyNavChrome();
      return;
    case ShellChromeOp::SyncLayout:
      shell.RequestSyncLayout(false, reason.c_str());
      return;
    }
  };
  return ports;
}

} // namespace pbr
