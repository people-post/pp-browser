#include "app/node/NodeConfigLoad.h"

#include "common/BoundedRead.h"
#include "common/Logger.h"
#include "common/ValueJson.h"
#include "foundation/data/Config.h"
#include "common/PlatformLimits.h"

#include <cstdlib>
#include <fstream>

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

std::optional<std::string> IgnoredRegistrationNote(const Object& file, const AppConfig& loaded) {
  const Object* registration = file.getObject("registration");
  if (!registration) {
    return std::nullopt;
  }
  const std::string url = registration->getString("base_url").value_or("");
  const std::string relay_id = registration->getString("relay_id").value_or("");
  const bool url_replaced = !url.empty() && url != loaded.registration.base_url;
  const bool id_replaced = !relay_id.empty() && relay_id != loaded.registration.relay_id;
  if (!url_replaced && !id_replaced) {
    return std::nullopt;
  }
  return "registration in the config file is ignored: backend '" + loaded.backend.env + "' uses its own (" +
         (loaded.registration.base_url.empty() ? std::string("none") : loaded.registration.base_url) +
         "); set PP_NODE_REGISTRATION_BASE_URL / PP_NODE_REGISTRATION_RELAY_ID, or name a custom backend "
         "(backend.env \"custom\")";
}

Roe<NodeConfig> LoadNodeConfig(int argc, char** argv) {
  auto config = Config::Load(argc, argv, Config::Fallback::None);
  if (!config) {
    return config.error();
  }
  // Say when the file's registration is not what runs, unless the environment names the registration anyway.
  const std::string path = NodeConfigPath(argc, argv);
  const char* env_url = std::getenv("PP_NODE_REGISTRATION_BASE_URL");
  if (!path.empty() && !(env_url && *env_url)) {
    std::ifstream in(path);
    if (auto text = ReadBounded(in, kMaxProfileJsonFileBytes)) {
      if (auto root = TryParseObject(*text)) {
        if (auto note = IgnoredRegistrationNote(*root, *config)) {
          logging::getLogger("pp-node").warning << *note;
        }
      }
    }
  }
  return NodeConfigFrom(*config);
}

std::string NodeConfigPath(int argc, char** argv) {
  return Config::DiscoverConfigPath(argc, argv, Config::Fallback::None);
}

} // namespace pbr
