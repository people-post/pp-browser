#pragma once

#include "feature/ui/PeoplePickerSurfaceSnapshot.h"
#include "feature/ui/ShellChromeOp.h"

namespace pbr {

struct PeoplePickerShellProjection {
  bool overlay_open = false;
};

PeoplePickerShellProjection ProjectPeoplePickerShell(const PeoplePickerSurfaceSnapshot& surface);
ShellChromeOp ClassifyPeoplePickerChromeUpdate(const PeoplePickerShellProjection& synced,
                                               const PeoplePickerShellProjection& next);

} // namespace pbr
