#pragma once

#include "base/data/BootstrapTypes.h"
#include "common/Error.h"
#include "feature/messaging/MessagingHub.h"
#include "common/PbrCompat.h"

namespace pbr {

class ProfileSecretsService;

class Bootstrap {
public:
  static Roe<BootstrapResult> Run(const BootstrapOptions& options, MessagingHub& messaging,
                                  ProfileSecretsService& secrets);
};

} // namespace pbr
