#pragma once

#include "platform/IAssetLocator.h"

namespace pbr {

class IosAssetLocator : public IAssetLocator {
public:
  std::string Resolve(const std::string& relative) const override;
};

} // namespace pbr
