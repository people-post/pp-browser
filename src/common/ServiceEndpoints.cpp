#include "common/ServiceEndpoints.h"

#include <utility>

namespace pbr {

std::vector<ServiceEndpointConfig> EffectiveDirectoryProviders(const DirectoryConfig& config) {
  if (!config.providers.empty()) {
    std::vector<ServiceEndpointConfig> out;
    out.reserve(config.providers.size());
    for (const ServiceEndpointConfig& provider : config.providers) {
      if (provider.base_url.empty()) {
        continue;
      }
      ServiceEndpointConfig row = provider;
      if (row.transport.empty()) {
        row.transport = "http";
      }
      out.push_back(std::move(row));
    }
    return out;
  }
  if (config.base_url.empty()) {
    return {};
  }
  ServiceEndpointConfig row;
  row.base_url = config.base_url;
  row.transport = config.transport.empty() ? "http" : config.transport;
  return {std::move(row)};
}

void NormalizeDirectoryConfig(DirectoryConfig& config) {
  if (config.base_url.empty() && !config.providers.empty()) {
    for (const ServiceEndpointConfig& provider : config.providers) {
      if (provider.base_url.empty()) {
        continue;
      }
      config.base_url = provider.base_url;
      config.transport = provider.transport.empty() ? "http" : provider.transport;
      break;
    }
  }
  if (config.transport.empty()) {
    config.transport = "http";
  }
}

} // namespace pbr
