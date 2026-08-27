#include "base/people/ProfileIconCache.h"

#include "base/people/ContactIdentity.h"
#include "common/Utilities.h"
#include "common/ValueJson.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace pbr {

namespace {

constexpr const char kMetaFilename[] = "meta.json";
constexpr const char kDefaultIconFilename[] = "icon.jpg";
constexpr const char kSelfCacheKey[] = "self";

std::filesystem::path MetaPath(const std::string& profile_dir, const std::string& cache_key) {
  return std::filesystem::path(ProfileIconCacheRoot(profile_dir, cache_key)) / kMetaFilename;
}

} // namespace

std::string SanitizeProfileIconCacheKey(const std::string& key) {
  if (key.empty()) {
    return key;
  }
  std::string sanitized;
  sanitized.reserve(key.size());
  for (const char ch : key) {
    if (ch == '/' || ch == '\\' || ch == ':') {
      sanitized.push_back('_');
    } else {
      sanitized.push_back(ch);
    }
  }
  return sanitized;
}

std::string ProfileIconCacheKeyForHit(const DirectoryHit& hit) {
  for (const ContactId& id : hit.ids) {
    if (id.kind == ContactIdKind::RelayUser && !id.value.empty()) {
      return SanitizeProfileIconCacheKey(id.value);
    }
  }
  if (hit.account_id && !hit.account_id->empty()) {
    return SanitizeProfileIconCacheKey(*hit.account_id);
  }
  for (const ContactId& id : hit.ids) {
    if (id.kind == ContactIdKind::Account && !id.value.empty()) {
      return SanitizeProfileIconCacheKey(id.value);
    }
  }
  if (!hit.hit_id.empty()) {
    return SanitizeProfileIconCacheKey(hit.hit_id);
  }
  return {};
}

std::string ProfileIconCacheKeyForContact(const Contact& contact) {
  if (const std::string relay = PrimaryIdOfKind(contact, ContactIdKind::RelayUser); !relay.empty()) {
    return SanitizeProfileIconCacheKey(relay);
  }
  if (const std::string account = PrimaryIdOfKind(contact, ContactIdKind::Account); !account.empty()) {
    return SanitizeProfileIconCacheKey(account);
  }
  return {};
}

std::string ProfileIconCacheRoot(const std::string& profile_dir, const std::string& cache_key) {
  const std::string key = cache_key.empty() ? kSelfCacheKey : cache_key;
  return (std::filesystem::path(profile_dir) / "cache" / "icons" / key).string();
}

Roe<ProfileIconCacheMeta> LoadProfileIconCacheMeta(const std::string& profile_dir, const std::string& cache_key) {
  const auto path = MetaPath(profile_dir, cache_key);
  if (!std::filesystem::exists(path)) {
    return Error("Profile icon cache missing");
  }
  std::ifstream in(path);
  if (!in) {
    return Error("Failed to read profile icon cache metadata");
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  auto root = TryParseObject(ss.str());
  if (!root) {
    return Error("Invalid profile icon cache metadata");
  }
  ProfileIconCacheMeta meta;
  if (auto v = root->getString("url")) {
    meta.url = *v;
  }
  if (auto v = root->getString("blob_id")) {
    meta.blob_id = *v;
  }
  if (auto v = root->getString("kind")) {
    meta.kind = *v;
  }
  if (auto v = root->getIf<int64_t>("fetched_at_ms")) {
    meta.fetched_at_ms = *v;
  }
  if (auto v = root->getString("local_filename")) {
    meta.local_filename = *v;
  } else {
    meta.local_filename = kDefaultIconFilename;
  }
  return meta;
}

std::string ProfileIconLocalPath(const std::string& profile_dir, const std::string& cache_key) {
  const auto meta = LoadProfileIconCacheMeta(profile_dir, cache_key);
  if (!meta) {
    return {};
  }
  const std::string filename =
      meta.value().local_filename.empty() ? kDefaultIconFilename : meta.value().local_filename;
  const auto path = std::filesystem::path(ProfileIconCacheRoot(profile_dir, cache_key)) / filename;
  if (!std::filesystem::exists(path)) {
    return {};
  }
  return path.string();
}

Roe<void> SaveProfileIconCache(const std::string& profile_dir, const std::vector<uint8_t>& bytes,
                               const std::string& file_extension, const ProfileIconCacheMeta& meta,
                               const std::string& cache_key) {
  if (bytes.empty()) {
    return Error("Profile icon bytes are empty");
  }
  const std::string ext = file_extension.empty() ? "jpg" : file_extension;
  const std::string filename = "icon." + ext;
  const auto root = std::filesystem::path(ProfileIconCacheRoot(profile_dir, cache_key));
  std::error_code ec;
  std::filesystem::create_directories(root, ec);
  if (ec) {
    return Error("Failed to create profile icon cache directory");
  }

  const auto icon_path = root / filename;
  {
    std::ofstream out(icon_path, std::ios::binary | std::ios::trunc);
    if (!out) {
      return Error("Failed to write profile icon cache file");
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }

  ProfileIconCacheMeta saved = meta;
  saved.local_filename = filename;
  if (saved.fetched_at_ms == 0) {
    saved.fetched_at_ms = util::NowUnixMs();
  }

  Object root_json;
  root_json.set("url", saved.url);
  root_json.set("blob_id", saved.blob_id);
  root_json.set("kind", saved.kind);
  root_json.set("fetched_at_ms", saved.fetched_at_ms);
  root_json.set("local_filename", saved.local_filename);
  const auto meta_path = root / kMetaFilename;
  std::ofstream meta_out(meta_path, std::ios::trunc);
  if (!meta_out) {
    return Error("Failed to write profile icon cache metadata");
  }
  meta_out << DumpJson(root_json, 2);
  return Roe<void>{};
}

Roe<void> ClearProfileIconCache(const std::string& profile_dir, const std::string& cache_key) {
  const auto root = std::filesystem::path(ProfileIconCacheRoot(profile_dir, cache_key));
  if (!std::filesystem::exists(root)) {
    return Roe<void>{};
  }
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  if (ec) {
    return Error("Failed to clear profile icon cache");
  }
  return Roe<void>{};
}

} // namespace pbr
