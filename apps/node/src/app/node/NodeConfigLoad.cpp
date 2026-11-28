#include "app/node/NodeConfigLoad.h"

#include "common/BoundedRead.h"
#include "common/Logger.h"
#include "common/PlatformLimits.h"
#include "common/ValueJson.h"
#include "foundation/data/ConfigSectionsJson.h"
#include "foundation/data/MeshRole.h"
#include "foundation/platform/BackendTable.h"
#include "foundation/platform/DeploymentProfile.h"

#include <cstdlib>
#include <cstring>
#include <fstream>

namespace pbr {
namespace {

/** What a node starts from with `table` as its backend (the app's defaults, for the node's keys). */
NodeConfig NodeDefaults(const BackendTable& table) {
  NodeConfig config;
  config.registration.base_url = table.registration_url;
  config.registration.relay_id = table.registration_relay_id;
  config.relay_content_sha256 = CompiledRelayContentSha256();
  NormalizeMeshConfig(config.mesh);
  return config;
}

/** The file's `key` object merged over `defaults` the way the app merges a config: objects recurse, the rest replace. */
Object MergedSection(Object defaults, const Object& file, const char* key) {
  if (const Object* section = file.getObject(key)) {
    DeepMergeObject(defaults, *section);
  }
  return defaults;
}

} // namespace

std::string NodeConfigPath(int argc, char** argv) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
      return argv[i + 1];
    }
  }
  if (const char* env = std::getenv("PP_BROWSER_CONFIG")) {
    return env;
  }
  return {};
}

std::optional<std::string> IgnoredRegistrationNote(const Object& file, const NodeConfig& loaded,
                                                   const std::string& backend) {
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
  return "registration in the config file is ignored: backend '" + backend + "' uses its own (" +
         (loaded.registration.base_url.empty() ? std::string("none") : loaded.registration.base_url) +
         "); set PP_NODE_REGISTRATION_BASE_URL / PP_NODE_REGISTRATION_RELAY_ID, or name a custom backend "
         "(backend.env \"custom\")";
}

Roe<NodeConfig> LoadNodeConfig(int argc, char** argv) {
  auto log = logging::getLogger("pp-node");
  const std::string path = NodeConfigPath(argc, argv);
  if (path.empty()) {
    log.info << "No config file named; the defaults and the environment";
    return NodeDefaults(DefaultBackendTable());
  }

  std::ifstream in(path);
  if (!in) {
    return Error("Failed to open config file: " + path);
  }
  auto text = ReadBounded(in, kMaxProfileJsonFileBytes);
  if (!text) {
    return Error(text.error().message + ": " + path);
  }
  auto root = TryParseObject(*text);
  if (!root) {
    return Error("Failed to parse config: " + path);
  }
  if (auto version = root->getIf<int64_t>("config_version")) {
    if (*version > kConfigFileVersion) {
      return Error("Unsupported config version " + std::to_string(*version) + " in " + path);
    }
  } else if (auto version_u = root->getNonNegInt("config_version")) {
    if (*version_u > static_cast<uint64_t>(kConfigFileVersion)) {
      return Error("Unsupported config version " + std::to_string(*version_u) + " in " + path);
    }
  }

  // The backend the node runs, as the app settles it (a custom backend's own services come from the file).
  BackendEnv saved = FallbackBackendIn(BuildCompiledOrigins());
  if (const Object* backend = root->getObject("backend")) {
    if (auto env = backend->getString("env")) {
      saved = ParseBackendEnv(*env);
    }
  }
  const BackendEnv runs = ResolveLatchedBackend(saved, BuildCompiledOrigins());
  LatchActiveBackend(runs, CustomServicesFromObject(*root));

  const NodeConfig defaults = NodeDefaults(DefaultBackendTable());
  NodeConfig config = defaults;
  MeshConfigFromObject(MergedSection(MeshConfigToObject(defaults.mesh), *root, "mesh"), config.mesh);
  ServiceEndpointFromObject(MergedSection(ServiceEndpointToObject(defaults.registration), *root, "registration"),
                            config.registration);
  if (auto data_dir = root->getString("data_dir")) {
    config.data_dir = *data_dir;
  }
  if (const Object* relay = root->getObject("relay")) {
    if (auto content_sha256 = relay->getIf<bool>("content_sha256")) {
      config.relay_content_sha256 = *content_sha256;
    }
  }
  if (const Object* node = root->getObject("node")) {
    config.node = *node;
  }

  // A built-in backend's registration is its own (the file's runs on a custom backend only).
  if (OriginEnvOverrideActive() || ActiveBackend() != BackendEnv::Custom) {
    const BackendTable table = ActiveBackendTable();
    config.registration.base_url = table.registration_url;
    config.registration.relay_id = table.registration_relay_id;
  }
  if (config.registration.base_url.empty()) {
    config.registration.base_url = defaults.registration.base_url;
  }
  NormalizeMeshConfig(config.mesh);

  log.info << "Loaded config from " << path;
  // Say when the file's registration is not what runs, unless the environment names the registration anyway.
  const char* env_url = std::getenv("PP_NODE_REGISTRATION_BASE_URL");
  if (!(env_url && *env_url)) {
    if (auto note = IgnoredRegistrationNote(*root, config, BackendEnvName(runs))) {
      log.warning << *note;
    }
  }
  return config;
}

} // namespace pbr
