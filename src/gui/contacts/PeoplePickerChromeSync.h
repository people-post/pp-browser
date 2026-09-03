#pragma once

#include "gui/contacts/PeoplePickerSurfaceSnapshot.h"
#include "gui/shell/ShellChromeOp.h"

namespace pbr {

struct PeoplePickerShellProjection {
  bool overlay_open = false;
};

PeoplePickerShellProjection ProjectPeoplePickerShell(const PeoplePickerSurfaceSnapshot& surface);
ShellChromeOp ClassifyPeoplePickerChromeUpdate(const PeoplePickerShellProjection& synced,
                                               const PeoplePickerShellProjection& next);

} // namespace pbr
