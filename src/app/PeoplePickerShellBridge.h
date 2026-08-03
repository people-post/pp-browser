#pragma once

#include "feature/ui/PeoplePickerChromeSync.h"
#include "feature/ui/PeoplePickerSurfaceSnapshot.h"
#include "feature/ui/ShellChromeApplyPorts.h"

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
