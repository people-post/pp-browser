#pragma once

#include <string>

namespace pbr {

class IAssetLocator {
public:
  virtual ~IAssetLocator() = default;

  virtual std::string Resolve(const std::string& relative) const = 0;

  static IAssetLocator& Instance();
  static void SetInstance(IAssetLocator* locator);
};

} // namespace pbr
