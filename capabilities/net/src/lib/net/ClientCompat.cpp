#include "lib/net/ClientCompat.h"
#include "common/PlatformLimits.h"
#include "common/BoundedRead.h"

#include "common/ValueJson.h"

#include "foundation/error/AppError.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
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

/** The version without a leading `v` and without build metadata (`+…`). */
std::string_view WithoutBuild(std::string_view version) {
  if (!version.empty() && (version.front() == 'v' || version.front() == 'V')) {
    version.remove_prefix(1);
  }
  return version.substr(0, version.find('+'));
}

/** The prerelease part of a version (after the first `-`, before any `+`); empty for a release. */
std::string_view PrereleaseOf(std::string_view version) {
  version = WithoutBuild(version);
  const size_t dash = version.find('-');
  return dash == std::string_view::npos ? std::string_view{} : version.substr(dash + 1);
}

bool AllDigits(std::string_view text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

/** Two all-digit identifiers, numerically (no overflow: by length, then by text, leading zeros dropped). */
int CompareNumeric(std::string_view a, std::string_view b) {
  const auto strip = [](std::string_view v) {
    while (v.size() > 1 && v.front() == '0') {
      v.remove_prefix(1);
    }
    return v;
  };
  a = strip(a);
  b = strip(b);
  if (a.size() != b.size()) {
    return a.size() < b.size() ? -1 : 1;
  }
  return a < b ? -1 : (a == b ? 0 : 1);
}

/** One prerelease identifier against another (CompareReleaseVersions). */
int CompareIdentifier(std::string_view a, std::string_view b) {
  const bool a_num = AllDigits(a);
  const bool b_num = AllDigits(b);
  if (a_num && b_num) {
    return CompareNumeric(a, b);
  }
  if (a_num != b_num) {
    return a_num ? -1 : 1; // numbers below words (semver 2.0)
  }
  // Letters then digits ("rc16"): the letters, then the number — how our tags are written (rc2 before rc16).
  const auto split = [](std::string_view v) {
    size_t i = v.size();
    while (i > 0 && v[i - 1] >= '0' && v[i - 1] <= '9') {
      --i;
    }
    return std::pair{v.substr(0, i), v.substr(i)};
  };
  const auto [a_word, a_number] = split(a);
  const auto [b_word, b_number] = split(b);
  if (!a_number.empty() && !b_number.empty() && a_word == b_word) {
    return CompareNumeric(a_number, b_number);
  }
  return a < b ? -1 : (a == b ? 0 : 1);
}

/** Dot-separated prerelease identifiers (both non-empty). */
int ComparePrerelease(std::string_view a, std::string_view b) {
  while (true) {
    const size_t a_dot = a.find('.');
    const size_t b_dot = b.find('.');
    if (const int c = CompareIdentifier(a.substr(0, a_dot), b.substr(0, b_dot)); c != 0) {
      return c;
    }
    if (a_dot == std::string_view::npos || b_dot == std::string_view::npos) {
      if (a_dot == b_dot) {
        return 0;
      }
      return a_dot == std::string_view::npos ? -1 : 1; // fewer identifiers first
    }
    a.remove_prefix(a_dot + 1);
    b.remove_prefix(b_dot + 1);
  }
}

/** A switch name the relay answers (SPECS § client-compat `switches`). */
bool IsSwitchName(const std::string& name) {
  if (name.empty() || name.size() > 64) {
    return false;
  }
  const auto lower_or_digit = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
  if (!lower_or_digit(name.front())) {
    return false;
  }
  return std::all_of(name.begin(), name.end(),
                     [&](char c) { return lower_or_digit(c) || c == '_' || c == '.' || c == '-'; });
}

std::map<std::string, bool> ParseSwitches(const Object* switches) {
  std::map<std::string, bool> out;
  if (!switches) {
    return out;
  }
  for (const auto& [name, value] : switches->fields()) {
    if (out.size() >= kMaxRemoteSwitches) {
      break;
    }
    const bool* on = std::get_if<bool>(&value);
    if (!on || !IsSwitchName(name)) {
      continue; // dropped, as the relay drops it
    }
    out[name] = *on;
  }
  return out;
}

/** A range bound: a release version (`X.Y.Z`, numbers only) or nothing (`null`, absent, anything else). */
std::string RangeBound(const Object& range, const char* key) {
  const auto bound = range.getString(key);
  if (!bound || bound->empty() || bound->find_first_not_of("0123456789.") != std::string::npos ||
      !ParseSemverCore(*bound).ok) {
    return {};
  }
  return *bound;
}

std::optional<ClientCompatRange> ParseRange(const Object* range) {
  if (!range) {
    return std::nullopt;
  }
  return ClientCompatRange{.min = RangeBound(*range, "min"), .max = RangeBound(*range, "max")};
}

Object RangeToObject(const ClientCompatRange& range) {
  Object out;
  for (const auto& [key, bound] : {std::pair{"min", &range.min}, std::pair{"max", &range.max}}) {
    if (bound->empty()) {
      out.set(key, Null{});
    } else {
      out.set(key, *bound);
    }
  }
  return out;
}

/** The platforms `upgrade_urls` may name (Platform::OsName()). */
constexpr const char* kUpgradePlatforms[] = {"ios", "android", "macos", "windows", "linux"};

/** Shown as an "update now" link: never anything but https (as `upgrade_url`). */
bool IsHttpsLink(const std::string& url) {
  return url.rfind("https://", 0) == 0 && url.size() > 8 && url.find_first_of(" \t\r\n") == std::string::npos;
}

std::map<std::string, std::string> ParseUpgradeUrls(const Object* urls) {
  std::map<std::string, std::string> out;
  if (!urls) {
    return out;
  }
  for (const char* platform : kUpgradePlatforms) {
    if (auto url = urls->getString(platform); url && IsHttpsLink(*url)) {
      out[platform] = *url;
    }
  }
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
  if (!doc.upgrade_urls.empty()) {
    Object urls;
    for (const auto& [platform, url] : doc.upgrade_urls) {
      urls.set(platform, url);
    }
    document.set("upgrade_urls", urls);
  }
  document.set("message", doc.message);
  if (!doc.crash_reports_url.empty()) {
    document.set("crash_reports_url", doc.crash_reports_url);
  }
  if (doc.support) {
    Object support;
    support.set("enabled", doc.support->enabled);
    support.set("account_id", doc.support->account_id);
    support.set("display_name", doc.support->display_name);
    document.set("support", support);
  }
  if (!doc.relay_version.empty()) {
    document.set("relay_version", doc.relay_version);
  }
  if (doc.app_range) {
    document.set("app_range", RangeToObject(*doc.app_range));
  }
  if (doc.node_range) {
    document.set("node_range", RangeToObject(*doc.node_range));
  }
  document.set("min_node_version", doc.min_node_version);
  Object switches;
  for (const auto& [name, on] : doc.switches) {
    switches.set(name, on);
  }
  document.set("switches", switches);
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

int CompareReleaseVersions(std::string_view a, std::string_view b) {
  const SemverCore core_a = ParseSemverCore(a);
  const SemverCore core_b = ParseSemverCore(b);
  if (const int core = CompareSemverCore(core_a, core_b); core != 0 || !core_a.ok || !core_b.ok) {
    return core;
  }
  const std::string_view pre_a = PrereleaseOf(a);
  const std::string_view pre_b = PrereleaseOf(b);
  if (pre_a.empty() || pre_b.empty()) {
    if (pre_a.empty() == pre_b.empty()) {
      return 0;
    }
    return pre_a.empty() ? 1 : -1; // a prerelease is below its release
  }
  return ComparePrerelease(pre_a, pre_b);
}

std::string ClientHeaderVersion(std::string_view version) {
  const SemverCore core = ParseSemverCore(version);
  if (!core.ok) {
    return {};
  }
  std::string out = std::to_string(core.major) + "." + std::to_string(core.minor) + "." + std::to_string(core.patch);
  const std::string_view pre = PrereleaseOf(version);
  const bool readable = !pre.empty() && pre.size() <= 16 && std::all_of(pre.begin(), pre.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.';
  });
  if (readable) {
    out += "-" + std::string(pre);
  }
  return out;
}

std::string RelayClientHeaderValue(std::string_view product, std::string_view version, std::string_view os) {
  const std::string core = ClientHeaderVersion(version);
  if (core.empty()) {
    return {};
  }
  const auto lower = [](std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
  };
  return lower(product) + "/" + core + " " + lower(os);
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
  // A newer schema_version is read for the fields this reader knows (T15); the relay keeps them meaning the same.
  if (*schema_version_opt < 1 || *schema_version_opt > INT32_MAX) {
    return Error("Invalid client-compat schema_version");
  }
  const int schema_version = static_cast<int>(*schema_version_opt);

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
    // This URL gets surfaced to the user as an "update now" link — the backend response is
    // otherwise trusted, but still never accept a non-https link here.
    if (v->rfind("https://", 0) == 0) {
      doc.upgrade_url = *v;
    }
  }
  doc.upgrade_urls = ParseUpgradeUrls(root->getObject("upgrade_urls")); // not an object: none
  if (auto v = root->getString("message")) {
    doc.message = *v;
  }
  if (auto v = root->getString("crash_reports_url")) {
    doc.crash_reports_url = *v;
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
  // Release-1 fields (A1, A2, O2): each read alone, a malformed one left out without failing the document.
  doc.relay_version = root->getString("relay_version").value_or("");
  doc.app_range = ParseRange(root->getObject("app_range"));
  doc.node_range = ParseRange(root->getObject("node_range"));
  doc.min_node_version = root->getString("min_node_version").value_or("");
  doc.switches = ParseSwitches(root->getObject("switches"));
  return doc;
}

std::string ResolvedUpgradeUrl(const ClientCompatDocument& doc, const std::string& fallback,
                               const std::string& platform) {
  if (const auto own = doc.upgrade_urls.find(platform); own != doc.upgrade_urls.end() && !own->second.empty()) {
    return own->second;
  }
  if (!doc.upgrade_url.empty()) {
    return doc.upgrade_url;
  }
  return fallback;
}

CompatUiAction DecideCompatUiAction(std::string_view local_version, const ClientCompatDocument& doc) {
  // A floor that is not a version is none (ParseSemverCore fails: CompareSemverCore would put us above it anyway).
  if (const SemverCore floor = ParseSemverCore(doc.min_client_version);
      floor.ok && CompareSemverCore(ParseSemverCore(local_version), floor) < 0) {
    return CompatUiAction::UpdateRequired;
  }
  if (ParseSemverCore(doc.latest_client_version).ok &&
      CompareReleaseVersions(local_version, doc.latest_client_version) < 0) {
    return CompatUiAction::SoftUpdateAvailable;
  }
  return CompatUiAction::None;
}

bool ClientCompatFloorUnreadable(const ClientCompatDocument& doc) {
  return !doc.min_client_version.empty() && !ParseSemverCore(doc.min_client_version).ok;
}

bool NodeBelowFloor(std::string_view node_version, const ClientCompatDocument& doc) {
  const SemverCore floor = ParseSemverCore(doc.min_node_version);
  const SemverCore own = ParseSemverCore(node_version);
  return floor.ok && own.ok && CompareSemverCore(own, floor) < 0;
}

CompatUiAction DecideCompatUiAction(std::string_view local_version, const ClientCompatDocument& doc,
                                    const bool relay_requires_upgrade) {
  return relay_requires_upgrade ? CompatUiAction::UpdateRequired : DecideCompatUiAction(local_version, doc);
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
  // An oversized file reads as unparseable (PB-063).
  const auto text = ReadBounded(in, kMaxProfileJsonFileBytes);
  const auto root = text ? TryParseObject(*text) : std::nullopt;
  if (!root) {
    return Error("Invalid client_compat cache");
  }
  // A file from before A4 names no version (read as it is); a newer app's file is no cache here, and is never written
  // over (SaveClientCompatCache): local data moves forward only (release-compatibility DESIGN § Rollback).
  if (root->getIf<int64_t>("schema_version").value_or(0) > kClientCompatCacheFileVersion) {
    return AppError::NewerFormat("client_compat cache written by a newer app");
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
  const auto path = CachePath(profile_data_dir);
  if (std::ifstream existing(path); existing) {
    const auto text = ReadBounded(existing, kMaxProfileJsonFileBytes);
    const auto held = text ? TryParseObject(*text) : std::nullopt;
    if (held && held->getIf<int64_t>("schema_version").value_or(0) > kClientCompatCacheFileVersion) {
      return AppError::NewerFormat("client_compat cache written by a newer app: kept as it is");
    }
  }
  Object root;
  root.set("schema_version", static_cast<int64_t>(kClientCompatCacheFileVersion));
  root.set("fetched_at_unix", entry.fetched_at_unix);
  root.set("document", ClientCompatDocumentToObject(entry.document));
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

namespace {

/** The cached copy, whatever its age (a failed or impossible fetch: fail open). */
ClientCompatResolution FromCache(const std::string& profile_data_dir, std::string error) {
  ClientCompatResolution out;
  out.error = std::move(error);
  if (auto cached = LoadClientCompatCache(profile_data_dir)) {
    out.document = std::move(cached->document);
    out.fetched_at_unix = cached->fetched_at_unix;
  }
  return out;
}

} // namespace

ClientCompatResolution ResolveClientCompat(IClientCompatClient* client, const std::string& profile_data_dir,
                                           const int64_t now_unix, const CompatCheck mode) {
  if (mode == CompatCheck::CacheFirst) {
    if (auto cached = LoadClientCompatCache(profile_data_dir); cached && ClientCompatCacheFresh(*cached, now_unix)) {
      return ClientCompatResolution{.document = std::move(cached->document),
                                    .fetched_at_unix = cached->fetched_at_unix};
    }
  }
  if (!client) {
    return FromCache(profile_data_dir, "client-compat client unavailable");
  }
  auto fetched = client->Fetch();
  if (!fetched) {
    return FromCache(profile_data_dir, fetched.error().message);
  }
  ClientCompatResolution out{.document = *fetched, .fetched_at_unix = now_unix, .fetched = true};
  if (auto saved = SaveClientCompatCache(profile_data_dir, ClientCompatCacheEntry{now_unix, *fetched}); !saved) {
    out.error = "Failed to cache client-compat: " + saved.error().message; // the document still applies
  }
  return out;
}

bool ClientCompatRecheckDue(const int64_t fetched_at_unix, const int64_t now_unix, const int64_t recheck_seconds) {
  return fetched_at_unix <= 0 || now_unix < fetched_at_unix || now_unix - fetched_at_unix >= recheck_seconds;
}

} // namespace pbr
