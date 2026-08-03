#include "app/PeoplePickerShellBridge.h"

namespace pbr {

void PeoplePickerShellBridge::BindApply(ShellChromeApplyPorts ports) {
  apply_ports_ = std::move(ports);
}

void PeoplePickerShellBridge::Clear() {
  apply_ports_ = {};
  synced_ = {};
}

void PeoplePickerShellBridge::OnSurface(const PeoplePickerSurfaceSnapshot& surface) {
  const PeoplePickerShellProjection next = ProjectPeoplePickerShell(surface);
  const ShellChromeOp update = ClassifyPeoplePickerChromeUpdate(synced_, next);
  synced_ = next;
  if (update == ShellChromeOp::None) {
    return;
  }
  if (apply_ports_.apply) {
    apply_ports_.apply(update);
  }
}

} // namespace pbr
