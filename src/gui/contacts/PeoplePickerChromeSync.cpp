#include "gui/contacts/PeoplePickerChromeSync.h"

namespace pbr {

PeoplePickerShellProjection ProjectPeoplePickerShell(const PeoplePickerSurfaceSnapshot& surface) {
  PeoplePickerShellProjection projection;
  projection.overlay_open = surface.overlay_open;
  return projection;
}

ShellChromeOp ClassifyPeoplePickerChromeUpdate(const PeoplePickerShellProjection& synced,
                                               const PeoplePickerShellProjection& next) {
  if (synced.overlay_open != next.overlay_open) {
    return ShellChromeOp::SyncLayout;
  }
  return ShellChromeOp::None;
}

} // namespace pbr
