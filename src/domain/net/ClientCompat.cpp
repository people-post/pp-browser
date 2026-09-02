#include "domain/net/ClientCompat.h"

#include "foundation/runtime/AppVersion.h"
#include "common/ValueJson.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include "common/PbrCompat.h"

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

Object ClientCompatDocumentToObject(const ClientCompatDocument& doc) {
  Object document;
  document.set("schema_version", static_cast<int64_t>(doc.schema_version));
  document.set("min_client_version", doc.min_client_version);
  document.set("latest_client_version", doc.latest_client_version);
  document.set("min_protocol_gen", static_cast<int64_t>(doc.min_protocol_gen));
  document.set("upgrade_url", doc.upgrade_url);
  document.set("message", doc.message);
  if (doc.support) {
    Object support;
    support.set("enabled", doc.support->enabled);
    support.set("account_id", doc.support->account_id);
    support.set("display_name", doc.support->display_name);
    document.set("support", support);
  }
  return document;
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
  auto root = TryParseObject(std::string(json_text));
  if (!root) {
    return Error("Invalid client-compat JSON");
  }
  auto schema_version_opt = root->getIf<int64_t>("schema_version");
  if (!schema_version_opt) {
    if (auto as_u = root->getNonNegInt("schema_version")) {
      schema_version_opt = static_cast<int64_t>(*as_u);
    }
  }
  if (!schema_version_opt) {
    return Error("client-compat missing schema_version");
  }
  const int schema_version = static_cast<int>(*schema_version_opt);
  if (schema_version > kClientCompatSchemaVersion) {
    return Error("Unsupported client-compat schema_version");
  }
  if (schema_version < 1) {
    return Error("Invalid client-compat schema_version");
  }

  ClientCompatDocument doc;
  doc.schema_version = schema_version;
  if (auto v = root->getString("min_client_version")) {
    doc.min_client_version = *v;
  }
  if (auto v = root->getString("latest_client_version")) {
    doc.latest_client_version = *v;
  }
  if (auto v = root->getIf<int64_t>("min_protocol_gen")) {
    doc.min_protocol_gen = static_cast<int>(*v);
  }
  if (auto v = root->getString("upgrade_url")) {
    doc.upgrade_url = *v;
  }
  if (auto v = root->getString("message")) {
    doc.message = *v;
  }
  if (const Object* support = root->getObject("support")) {
    ClientCompatSupport block;
    const auto enabled_opt = support->getIf<bool>("enabled");
    if (enabled_opt) {
      block.enabled = *enabled_opt;
    }
    if (auto account_id = support->getString("account_id")) {
      block.account_id = *account_id;
    }
    if (auto display_name = support->getString("display_name")) {
      block.display_name = *display_name;
    }
    // On only when enabled + non-empty account_id (matches www ClientCompat.ts).
    if (block.enabled && !block.account_id.empty()) {
      doc.support = std::move(block);
    } else if (enabled_opt && !*enabled_opt) {
      block.enabled = false;
      block.account_id.clear();
      doc.support = std::move(block);
    }
    // Malformed / incomplete enabled block: omit (fail-open for Support only).
  }
  // Non-object support: omit (do not fail the whole document).
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
  auto root = TryParseObject(ss.str());
  if (!root) {
    return Error("Invalid client_compat cache");
  }
  auto fetched_at = root->getIf<int64_t>("fetched_at_unix");
  if (!fetched_at) {
    return Error("client_compat cache missing fetched_at_unix");
  }
  const Object* document = root->getObject("document");
  if (!document) {
    return Error("client_compat cache missing document");
  }
  auto parsed = ParseClientCompatDocument(DumpJson(*document));
  if (!parsed) {
    return parsed.error();
  }
  ClientCompatCacheEntry entry;
  entry.fetched_at_unix = *fetched_at;
  entry.document = std::move(*parsed);
  return entry;
}

Roe<void> SaveClientCompatCache(const std::string& profile_data_dir, const ClientCompatCacheEntry& entry) {
  if (profile_data_dir.empty()) {
    return Error("Empty profile data dir");
  }
  std::error_code ec;
  std::filesystem::create_directories(profile_data_dir, ec);
  Object root;
  root.set("fetched_at_unix", entry.fetched_at_unix);
  root.set("document", ClientCompatDocumentToObject(entry.document));
  const auto path = CachePath(profile_data_dir);
  std::ofstream out(path, std::ios::trunc);
  if (!out) {
    return Error("Failed to write client_compat cache");
  }
  out << DumpJson(root, 2);
  return {};
}

bool ClientCompatCacheFresh(const ClientCompatCacheEntry& entry, int64_t now_unix, int64_t ttl_seconds) {
  if (entry.fetched_at_unix <= 0 || now_unix < entry.fetched_at_unix) {
    return false;
  }
  return (now_unix - entry.fetched_at_unix) < ttl_seconds;
}

} // namespace pbr
