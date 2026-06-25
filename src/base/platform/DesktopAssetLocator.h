#pragma once

#include "base/platform/IAssetLocator.h"

namespace pbr {

class DesktopAssetLocator : public IAssetLocator {
public:
  std::string Resolve(const std::string& relative) const override;
};

} // namespace pbr
