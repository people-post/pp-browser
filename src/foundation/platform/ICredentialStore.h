#pragma once

#include <string>

namespace pbr {

class ICredentialStore {
public:
  virtual ~ICredentialStore() = default;

  virtual std::string Get(const std::string& key) const = 0;

  static ICredentialStore& Instance();
  static void SetInstance(ICredentialStore* store);
};

} // namespace pbr
