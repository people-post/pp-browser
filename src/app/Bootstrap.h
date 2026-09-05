#pragma once

#include "foundation/data/BootstrapTypes.h"
#include "common/Error.h"
#include "feature/conversations/ConversationsHub.h"
#include "common/PbrCompat.h"

namespace pbr {

class ProfileSecretsService;

class Bootstrap {
public:
  static Roe<BootstrapResult> Run(const BootstrapOptions& options, ConversationsHub& messaging,
                                  ProfileSecretsService& secrets);
};

} // namespace pbr
