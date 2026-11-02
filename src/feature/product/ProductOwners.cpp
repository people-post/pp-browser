#include "feature/product/ProductOwners.h"

namespace pbr {

std::vector<pp::OwnerThreadSpec> ProductOwnerThreads() {
  return {MediaSessionsOwnerThread()};
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
