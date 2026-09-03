#pragma once

#include "feature/ui/contacts/PeoplePickerSurfaceSnapshot.h"
#include "feature/ui/shell/ShellChromeOp.h"

namespace pbr {

struct PeoplePickerShellProjection {
  bool overlay_open = false;
};

PeoplePickerShellProjection ProjectPeoplePickerShell(const PeoplePickerSurfaceSnapshot& surface);
ShellChromeOp ClassifyPeoplePickerChromeUpdate(const PeoplePickerShellProjection& synced,
                                               const PeoplePickerShellProjection& next);

} // namespace pbr
