#pragma once

#include "base/data/BootstrapTypes.h"
#include "common/Error.h"

namespace pbr {

class MessagingHub;

class Bootstrap {
public:
  static Roe<BootstrapResult> Run(const BootstrapOptions& options, MessagingHub& messaging);
};

} // namespace pbr
