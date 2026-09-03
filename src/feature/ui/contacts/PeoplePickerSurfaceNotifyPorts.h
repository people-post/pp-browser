#pragma once

#include "feature/ui/contacts/PeoplePickerSurfaceSnapshot.h"

#include <functional>

namespace pbr {

struct PeoplePickerSurfaceNotifyPorts {
  std::function<void(const PeoplePickerSurfaceSnapshot&)> push_surface;
};

} // namespace pbr
