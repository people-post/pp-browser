#include "app/node/NodeConfigLoad.h"

#include "foundation/data/Config.h"

namespace pbr {

NodeConfig NodeConfigFrom(const AppConfig& config) {
  NodeConfig node;
  node.mesh = config.mesh;
  node.registration = config.registration;
  node.data_dir = config.data_dir;
  node.relay_content_sha256 = config.relay_content_sha256;
  node.node = config.node;
  return node;
}

Roe<NodeConfig> LoadNodeConfig(int argc, char** argv) {
  auto config = Config::Load(argc, argv, Config::Fallback::None);
  if (!config) {
    return config.error();
  }
  return NodeConfigFrom(*config);
}

std::string NodeConfigPath(int argc, char** argv) {
  return Config::DiscoverConfigPath(argc, argv, Config::Fallback::None);
}

} // namespace pbr
