#include "foundation/runtime/ProductOwners.h"

namespace pbr {

std::vector<pp::OwnerThreadSpec> ProductOwnerThreads() {
  return {{kMediaSessionsOwner, "pp-media-sess"}};
}

AppRuntimeConfig ProductRuntimeConfig() {
  AppRuntimeConfig config;
  config.owners = ProductOwnerThreads();
  return config;
}

AppRuntimeConfig ManualOwnerRuntimeConfig() {
  return pp::ManualOwnerRuntimeConfig(ProductOwnerThreads());
}

} // namespace pbr
