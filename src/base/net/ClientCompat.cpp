#include "base/net/ClientCompat.h"

#include "base/runtime/AppVersion.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

namespace pbr {

namespace {

SemverCore ParseSemverCoreImpl(std::string_view version) {
  SemverCore out;
  if (version.empty()) {
    return out;
  }
  if (version.front() == 'v' || version.front() == 'V') {
    version.remove_prefix(1);
  }
  const size_t cut = version.find_first_of("-+");
  if (cut != std::string_view::npos) {
    version = version.substr(0, cut);
  }
  if (version.empty()) {
    return out;
  }

  int parts[3] = {0, 0, 0};
  int part_i = 0;
  int value = 0;
  bool in_number = false;
  for (size_t i = 0; i < version.size(); ++i) {
    const char c = version[i];
    if (c >= '0' && c <= '9') {
      in_number = true;
      value = value * 10 + (c - '0');
      continue;
    }
    if (c == '.') {
      if (!in_number || part_i >= 3) {
        return {};
      }
      parts[part_i++] = value;
      value = 0;
      in_number = false;
      continue;
    }
    return {};
  }
  if (!in_number || part_i >= 3) {
    return {};
  }
  parts[part_i] = value;
  out.major = parts[0];
  out.minor = parts[1];
  out.patch = parts[2];
  out.ok = true;
  return out;
}

std::filesystem::path CachePath(const std::string& profile_data_dir) {
  return std::filesystem::path(profile_data_dir) / "client_compat.json";
}

} // namespace

SemverCore ParseSemverCore(std::string_view version) {
  return ParseSemverCoreImpl(version);
}

int CompareSemverCore(const SemverCore& a, const SemverCore& b) {
  if (!a.ok && !b.ok) {
    return 0;
  }
  if (!a.ok) {
    return -1;
  }
  if (!b.ok) {
    return 1;
  }
  if (a.major != b.major) {
    return a.major < b.major ? -1 : 1;
  }
  if (a.minor != b.minor) {
    return a.minor < b.minor ? -1 : 1;
  }
  if (a.patch != b.patch) {
    return a.patch < b.patch ? -1 : 1;
  }
  return 0;
}

int CompareSemverCore(std::string_view a, std::string_view b) {
  return CompareSemverCore(ParseSemverCore(a), ParseSemverCore(b));
}

Roe<ClientCompatDocument> ParseClientCompatDocument(std::string_view json_text) {
  const nlohmann::json root = nlohmann::json::parse(json_text, nullptr, false);
  if (root.is_discarded() || !root.is_object()) {
    return Error("Invalid client-compat JSON");
  }
  if (!root.contains("schema_version") || !root["schema_version"].is_number_integer()) {
    return Error("client-compat missing schema_version");
  }
  const int schema_version = root["schema_version"].get<int>();
  if (schema_version > kClientCompatSchemaVersion) {
    return Error("Unsupported client-compat schema_version");
  }
  if (schema_version < 1) {
    return Error("Invalid client-compat schema_version");
  }

  ClientCompatDocument doc;
  doc.schema_version = schema_version;
  if (root.contains("min_client_version") && root["min_client_version"].is_string()) {
    doc.min_client_version = root["min_client_version"].get<std::string>();
  }
  if (root.contains("latest_client_version") && root["latest_client_version"].is_string()) {
    doc.latest_client_version = root["latest_client_version"].get<std::string>();
  }
  if (root.contains("min_protocol_gen") && root["min_protocol_gen"].is_number_integer()) {
    doc.min_protocol_gen = root["min_protocol_gen"].get<int>();
  }
  if (root.contains("upgrade_url") && root["upgrade_url"].is_string()) {
    doc.upgrade_url = root["upgrade_url"].get<std::string>();
  }
  if (root.contains("message") && root["message"].is_string()) {
    doc.message = root["message"].get<std::string>();
  }
  return doc;
}

std::string ResolvedUpgradeUrl(const ClientCompatDocument& doc) {
  if (!doc.upgrade_url.empty()) {
    return doc.upgrade_url;
  }
  return kDefaultUpgradeUrl;
}

CompatUiAction DecideCompatUiAction(std::string_view local_version, const ClientCompatDocument& doc) {
  if (!doc.min_client_version.empty() && CompareSemverCore(local_version, doc.min_client_version) < 0) {
    return CompatUiAction::UpdateRequired;
  }
  if (!doc.latest_client_version.empty() && CompareSemverCore(local_version, doc.latest_client_version) < 0) {
    return CompatUiAction::SoftUpdateAvailable;
  }
  return CompatUiAction::None;
}

Roe<ClientCompatCacheEntry> LoadClientCompatCache(const std::string& profile_data_dir) {
  if (profile_data_dir.empty()) {
    return Error("Empty profile data dir");
  }
  const auto path = CachePath(profile_data_dir);
  std::ifstream in(path);
  if (!in) {
    return Error("client_compat cache missing");
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const nlohmann::json root = nlohmann::json::parse(ss.str(), nullptr, false);
  if (root.is_discarded() || !root.is_object()) {
    return Error("Invalid client_compat cache");
  }
  if (!root.contains("fetched_at_unix") || !root["fetched_at_unix"].is_number_integer()) {
    return Error("client_compat cache missing fetched_at_unix");
  }
  if (!root.contains("document") || !root["document"].is_object()) {
    return Error("client_compat cache missing document");
  }
  auto parsed = ParseClientCompatDocument(root["document"].dump());
  if (!parsed) {
    return parsed.error();
  }
  ClientCompatCacheEntry entry;
  entry.fetched_at_unix = root["fetched_at_unix"].get<int64_t>();
  entry.document = std::move(*parsed);
  return entry;
}

Roe<void> SaveClientCompatCache(const std::string& profile_data_dir, const ClientCompatCacheEntry& entry) {
  if (profile_data_dir.empty()) {
    return Error("Empty profile data dir");
  }
  std::error_code ec;
  std::filesystem::create_directories(profile_data_dir, ec);
  nlohmann::json document = {{"schema_version", entry.document.schema_version},
                             {"min_client_version", entry.document.min_client_version},
                             {"latest_client_version", entry.document.latest_client_version},
                             {"min_protocol_gen", entry.document.min_protocol_gen},
                             {"upgrade_url", entry.document.upgrade_url},
                             {"message", entry.document.message}};
  const nlohmann::json root = {{"fetched_at_unix", entry.fetched_at_unix}, {"document", std::move(document)}};
  const auto path = CachePath(profile_data_dir);
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    return Error("Failed to write client_compat cache");
  }
  out << root.dump(2);
  return {};
}

bool ClientCompatCacheFresh(const ClientCompatCacheEntry& entry, int64_t now_unix, int64_t ttl_seconds) {
  if (entry.fetched_at_unix <= 0 || now_unix < entry.fetched_at_unix) {
    return false;
  }
  return (now_unix - entry.fetched_at_unix) < ttl_seconds;
}

} // namespace pbr
