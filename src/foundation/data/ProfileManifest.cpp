#include "foundation/data/ProfileManifest.h"

#include "common/AtomicFileWrite.h"
#include "common/BoundedRead.h"
#include "common/PlatformLimits.h"
#include "common/SchemaVersion.h"
#include "common/ValueJson.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include "common/PbrCompat.h"

namespace pbr {

namespace {

std::optional<Object> ReadManifest(const std::filesystem::path& path) {
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

} // namespace

Roe<void> EnsureProfileManifest(const std::string& profile_data_dir, const std::string& backend_key) {
  const std::filesystem::path profile_dir(profile_data_dir);
  const std::filesystem::path manifest_path = profile_dir / "manifest.json";
  std::error_code ec;
  std::filesystem::create_directories(profile_dir, ec);
  if (ec) {
    return Roe<void>::error(Error("Failed to create profile data directory"));
  }

  if (!std::filesystem::exists(manifest_path, ec)) {
    if (ec) {
      return Roe<void>::error(Error("Failed to check profile manifest"));
    }
    Object root;
    root.set("schema_version", static_cast<int64_t>(SchemaVersion::kCurrentSchemaVersion));
    if (!backend_key.empty()) {
      root.set("backend", backend_key);
    }
    if (auto written = AtomicFileWrite::Write(manifest_path.string(), DumpJson(root, 2)); !written) {
      return Roe<void>::error(Error(written.error().message));
    }
    return {};
  }

  std::ifstream in(manifest_path);
  if (!in) {
    return Roe<void>::error(Error("Failed to open profile manifest"));
  }
  auto text = ReadBounded(in, kMaxProfileJsonFileBytes);
  if (!text) {
    return Roe<void>::error(Error("Failed to read profile manifest: " + text.error().message));
  }
  auto root = TryParseObject(*text);
  if (!root) {
    return Roe<void>::error(Error("Failed to parse profile manifest"));
  }

  if (auto validated = SchemaVersion::Validate(*root, SchemaVersion::kCurrentSchemaVersion, "manifest.json");
      !validated) {
    return Roe<void>::error(Error(validated.error().message));
  }

  if (auto migrated =
          SchemaVersion::RunForwardMigrators(manifest_path.string(), *root, SchemaVersion::kCurrentSchemaVersion);
      !migrated) {
    return Roe<void>::error(Error(migrated.error().message));
  }

  return {};
}

std::string ReadProfileBackend(const std::string& profile_data_dir) {
  const auto root = ReadManifest(std::filesystem::path(profile_data_dir) / "manifest.json");
  if (!root) {
    return {};
  }
  return root->getString("backend").value_or(std::string());
}

Roe<void> AdoptProfileBackend(const std::string& profile_data_dir, const std::string& backend_key) {
  const std::filesystem::path path = std::filesystem::path(profile_data_dir) / "manifest.json";
  auto root = ReadManifest(path);
  if (!root) {
    return Roe<void>::error(Error("Failed to read profile manifest"));
  }
  if (root->getString("backend").has_value()) {
    return {};
  }
  root->set("backend", backend_key);
  if (auto written = AtomicFileWrite::Write(path.string(), DumpJson(*root, 2)); !written) {
    return Roe<void>::error(Error(written.error().message));
  }
  return {};
}

} // namespace pbr
