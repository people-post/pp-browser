#pragma once

#include "base/data/BootstrapTypes.h"
#include "common/Error.h"

namespace pbr {

class Bootstrap {
public:
  static Roe<BootstrapResult> Run(const BootstrapOptions& options);
};

} // namespace pbr
