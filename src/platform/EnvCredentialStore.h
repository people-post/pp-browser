#pragma once

#include "platform/ICredentialStore.h"

namespace pbr {

class EnvCredentialStore : public ICredentialStore {
public:
  std::string Get(const std::string& key) const override;
};

} // namespace pbr
