#include "app/ContactsShellBridge.h"

namespace pbr {

void ContactsShellBridge::BindApply(ShellContactsChromePorts ports) {
  apply_ports_ = std::move(ports);
}

void ContactsShellBridge::Clear() {
  apply_ports_ = {};
  synced_ = {};
}

void ContactsShellBridge::OnSurface(const ContactsSurfaceSnapshot& surface) {
  const ContactsShellProjection next = ProjectContactsShell(surface);
  const ContactsChromeUpdate update = ClassifyContactsChromeUpdate(synced_, next);
  synced_ = next;
  if (update == ContactsChromeUpdate::None) {
    return;
  }
  if (apply_ports_.apply_chrome_update) {
    apply_ports_.apply_chrome_update(update);
  }
}

} // namespace pbr
