#include "feature/ui/ShellContactsChromePorts.h"

#include "feature/ui/ShellHost.h"

namespace pbr {

ShellContactsChromePorts MakeShellContactsChromePorts(ShellHost& shell) {
  ShellContactsChromePorts ports;
  ports.apply_chrome_update = [&shell](const ContactsChromeUpdate update) {
    switch (update) {
    case ContactsChromeUpdate::None:
      return;
    case ContactsChromeUpdate::DirtyNav:
      shell.DirtyNavChrome();
      return;
    case ContactsChromeUpdate::SyncLayout:
      // SyncLayout already DirtyWindow(); do not also DirtyNav.
      shell.RequestSyncLayout(false, "contacts_chrome");
      return;
    }
  };
  return ports;
}

} // namespace pbr
