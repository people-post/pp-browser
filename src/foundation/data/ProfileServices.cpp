#include "foundation/data/ProfileServices.h"

#include "common/AtomicFileWrite.h"
#include "common/BoundedRead.h"
#include "common/PlatformLimits.h"
#include "common/ValueJson.h"
#include "foundation/data/ConfigJson.h"

#include <filesystem>
#include <fstream>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

std::filesystem::path ServicesPath(const std::string& profile_data_dir) {
  return std::filesystem::path(profile_data_dir) / "services.json";
}

std::optional<Object> ReadServicesObject(const std::string& profile_data_dir) {
  std::ifstream in(ServicesPath(profile_data_dir));
  if (!in) {
    return std::nullopt;
  }
  auto text = ReadBounded(in, kMaxProfileJsonFileBytes);
  if (!text) {
    return std::nullopt;
  }
  return TryParseObject(*text);
}

bool IsNewer(const Object& root) {
  return root.getNonNegInt("schema_version").value_or(0) > static_cast<uint64_t>(ProfileServices::kSchemaVersion);
}

} // namespace

ProfileServices LoadProfileServices(const std::string& profile_data_dir) {
  ProfileServices services;
  if (profile_data_dir.empty()) {
    return services;
  }
  const auto root = ReadServicesObject(profile_data_dir);
  if (!root || IsNewer(*root)) {
    return services;
  }
  if (const Array* providers = root->getArray("directory_providers")) {
    std::vector<std::string> urls;
    for (const Value& item : providers->elements) {
      if (auto url = asString(item); url && !url->empty()) {
        urls.push_back(*url);
      }
    }
    services.directory_providers = std::move(urls);
  }
  if (auto url = root->getString("mesh_nodes")) {
    services.mesh_nodes_url = *url;
  }
  if (const Object* mcp = root->getObject("promoted_mcp")) {
    McpConfig config;
    McpConfigFromObject(*mcp, config);
    if (config.IsConfigured() || !config.enabled) {
      config.user_set = true;
      services.promoted_mcp = std::move(config);
    }
  }
  return services;
}

Roe<void> SaveProfileServices(const std::string& profile_data_dir, const ProfileServices& services) {
  if (profile_data_dir.empty()) {
    return {};
  }
  if (const auto kept = ReadServicesObject(profile_data_dir); kept && IsNewer(*kept)) {
    return {}; // a newer app's choices stay as they are
  }
  std::error_code ec;
  if (services == ProfileServices{}) {
    std::filesystem::remove(ServicesPath(profile_data_dir), ec);
    return {};
  }
  Object root;
  root.set("schema_version", static_cast<int64_t>(ProfileServices::kSchemaVersion));
  if (services.directory_providers) {
    std::vector<Value> urls;
    for (const std::string& url : *services.directory_providers) {
      urls.emplace_back(url);
    }
    root.set("directory_providers", makeArray(std::move(urls)));
  }
  if (services.mesh_nodes_url) {
    root.set("mesh_nodes", *services.mesh_nodes_url);
  }
  if (services.promoted_mcp) {
    McpConfig mcp = *services.promoted_mcp;
    mcp.user_set = false; // the file is the user's: no marker needed here
    root.set("promoted_mcp", McpConfigToObject(mcp));
  }
  std::filesystem::create_directories(profile_data_dir, ec);
  return AtomicFileWrite::Write(ServicesPath(profile_data_dir).string(), DumpJson(root, 2));
}

void OverlayProfileServices(AppConfig& config, const ProfileServices& services) {
  if (services.promoted_mcp) {
    config.promoted_mcp = *services.promoted_mcp;
    config.promoted_mcp.user_set = true;
  }
}

} // namespace pbr
