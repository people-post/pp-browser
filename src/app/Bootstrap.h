#pragma once

#include "base/data/BootstrapTypes.h"
#include "common/Error.h"
#include "feature/messaging/MessagingHub.h"

namespace pbr {

class Bootstrap {
public:
  static Roe<BootstrapResult> Run(const BootstrapOptions& options, MessagingHub& messaging);
};

} // namespace pbr
