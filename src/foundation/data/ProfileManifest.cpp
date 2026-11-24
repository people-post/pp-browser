#include "foundation/data/ProfileManifest.h"

#include "common/AtomicFileWrite.h"
#include "common/BoundedRead.h"
#include "common/PlatformLimits.h"
#include "common/ValueJson.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

std::filesystem::path ManifestPath(const std::string& profile_data_dir) {
  return std::filesystem::path(profile_data_dir) / "manifest.json";
}

std::optional<Object> ReadManifestObject(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in) {
    return std::nullopt;
  }
  auto text = ReadBounded(in, kMaxProfileJsonFileBytes);
  if (!text) {
    return std::nullopt;
  }
  return TryParseObject(*text);
}

Roe<void> WriteManifest(const std::filesystem::path& path, std::string_view network) {
  Object root;
  root.set("schema_version", static_cast<int64_t>(kProfileManifestSchemaVersion));
  root.set("network", std::string(network));
  if (auto written = AtomicFileWrite::Write(path.string(), DumpJson(root, 2)); !written) {
    return Roe<void>::error(Error(written.error().message));
  }
  return {};
}

} // namespace

bool IsKnownProfileNetwork(const std::string_view network) {
  return network == profile_network::kMain || network == profile_network::kStaging ||
         network == profile_network::kDev || network == profile_network::kTestCustom ||
         network == profile_network::kNode;
}

ProfileManifestRead ReadProfileManifest(const std::string& profile_data_dir) {
  const std::filesystem::path path = ManifestPath(profile_data_dir);
  std::error_code ec;
  if (!std::filesystem::exists(path, ec) && !ec) {
    return {ProfileManifestState::kMissing, {}};
  }
  const auto root = ReadManifestObject(path);
  if (!root) {
    return {ProfileManifestState::kUnreadable, {}};
  }
  const auto version = root->getNonNegInt("schema_version");
  if (!version) {
    return {ProfileManifestState::kUnreadable, {}};
  }
  if (*version > static_cast<uint64_t>(kProfileManifestSchemaVersion)) {
    return {ProfileManifestState::kNewer, {}};
  }
  if (*version < static_cast<uint64_t>(kProfileManifestSchemaVersion)) {
    return {ProfileManifestState::kOldFormat, {}};
  }
  std::string network = root->getString("network").value_or(std::string());
  if (!IsKnownProfileNetwork(network)) {
    return {ProfileManifestState::kUnreadable, {}};
  }
  return {ProfileManifestState::kCurrent, std::move(network)};
}

Roe<void> WriteNewProfileManifest(const std::string& profile_data_dir, const std::string_view network) {
  if (!IsKnownProfileNetwork(network)) {
    return Roe<void>::error(Error("unknown profile network: " + std::string(network)));
  }
  const std::filesystem::path path = ManifestPath(profile_data_dir);
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) {
    return Roe<void>::error(Error("Failed to create profile data directory"));
  }
  if (std::filesystem::exists(path, ec) || ec) {
    return Roe<void>::error(Error("profile manifest already exists: " + path.string()));
  }
  return WriteManifest(path, network);
}

Roe<void> ConvertOldProfileManifestToNode(const std::string& profile_data_dir) {
  if (ReadProfileManifest(profile_data_dir).state != ProfileManifestState::kOldFormat) {
    return Roe<void>::error(Error("not an old-format profile manifest: " + profile_data_dir));
  }
  return WriteManifest(ManifestPath(profile_data_dir), profile_network::kNode);
}

} // namespace pbr
