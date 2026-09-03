#pragma once

#include "gui/contacts/PeoplePickerChromeSync.h"
#include "gui/contacts/PeoplePickerSurfaceSnapshot.h"
#include "gui/shell/ShellChromeApplyPorts.h"

namespace pbr {

class PeoplePickerShellBridge {
public:
  void BindApply(ShellChromeApplyPorts ports);
  void Clear();
  void OnSurface(const PeoplePickerSurfaceSnapshot& surface);

private:
  PeoplePickerShellProjection synced_{};
  ShellChromeApplyPorts apply_ports_;
};

} // namespace pbr
