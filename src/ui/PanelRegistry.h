#pragma once

#include "ui/SplitTree.h"

#include <string>

namespace pbr {

class PanelRegistry {
public:
  static std::string Body(PanelKind kind);
};

} // namespace pbr
