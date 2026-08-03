#include "app/ContactsShellBridge.h"

namespace pbr {

void ContactsShellBridge::BindApply(ShellChromeApplyPorts ports) {
  apply_ports_ = std::move(ports);
}

void ContactsShellBridge::Clear() {
  apply_ports_ = {};
  synced_ = {};
  last_surface_ = {};
}

void ContactsShellBridge::OnSurface(const ContactsSurfaceSnapshot& surface) {
  last_surface_ = surface;
  const ContactsShellProjection next = ProjectContactsShell(surface);
  const ShellChromeOp update = ClassifyContactsChromeUpdate(synced_, next);
  synced_ = next;
  if (update == ShellChromeOp::None) {
    return;
  }
  if (apply_ports_.apply) {
    apply_ports_.apply(update);
  }
}

} // namespace pbr
