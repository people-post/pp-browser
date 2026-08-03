#pragma once

#include "feature/ui/ContactsChromeSync.h"
#include "feature/ui/ShellContactsChromePorts.h"

namespace pbr {

/**
 * App-owned translator: contacts surface snapshot → shell chrome ops.
 * Lives above ContactsController (composition root); CallController stays special-cased.
 */
class ContactsShellBridge {
public:
  void BindApply(ShellContactsChromePorts ports);
  void Clear();

  void OnSurface(const ContactsSurfaceSnapshot& surface);

private:
  ContactsShellProjection synced_{};
  ShellContactsChromePorts apply_ports_;
};

} // namespace pbr
