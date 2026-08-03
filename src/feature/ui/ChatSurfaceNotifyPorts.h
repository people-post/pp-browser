#pragma once

#include "feature/ui/ChatSurfaceSnapshot.h"

#include <functional>

namespace pbr {

struct ChatSurfaceNotifyPorts {
  std::function<void(const ChatSurfaceSnapshot&)> push_surface;
};

} // namespace pbr
