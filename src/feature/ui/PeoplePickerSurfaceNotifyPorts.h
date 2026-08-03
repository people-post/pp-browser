#pragma once

#include "feature/ui/PeoplePickerSurfaceSnapshot.h"

#include <functional>

namespace pbr {

struct PeoplePickerSurfaceNotifyPorts {
  std::function<void(const PeoplePickerSurfaceSnapshot&)> push_surface;
};

} // namespace pbr
