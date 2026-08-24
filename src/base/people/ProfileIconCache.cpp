#include "base/people/ProfileIconCache.h"

#include "common/Utilities.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

namespace pbr {

namespace {

constexpr const char kMetaFilename[] = "meta.json";
constexpr const char kDefaultIconFilename[] = "icon.jpg";

std::filesystem::path MetaPath(const std::string& profile_dir) {
  return std::filesystem::path(ProfileIconCacheRoot(profile_dir)) / kMetaFilename;
}

} // namespace

std::string ProfileIconCacheRoot(const std::string& profile_dir) {
  return (std::filesystem::path(profile_dir) / "cache" / "icons" / "self").string();
}

Roe<ProfileIconCacheMeta> LoadProfileIconCacheMeta(const std::string& profile_dir) {
  const auto path = MetaPath(profile_dir);
  if (!std::filesystem::exists(path)) {
    return Error("Profile icon cache missing");
  }
  std::ifstream in(path);
  if (!in) {
    return Error("Failed to read profile icon cache metadata");
  }
  nlohmann::json root = nlohmann::json::parse(in, nullptr, false);
  if (root.is_discarded()) {
    return Error("Invalid profile icon cache metadata");
  }
  ProfileIconCacheMeta meta;
  if (root.contains("url") && root["url"].is_string()) {
    meta.url = root["url"].get<std::string>();
  }
  if (root.contains("blob_id") && root["blob_id"].is_string()) {
    meta.blob_id = root["blob_id"].get<std::string>();
  }
  if (root.contains("kind") && root["kind"].is_string()) {
    meta.kind = root["kind"].get<std::string>();
  }
  if (root.contains("fetched_at_ms") && root["fetched_at_ms"].is_number_integer()) {
    meta.fetched_at_ms = root["fetched_at_ms"].get<int64_t>();
  }
  if (root.contains("local_filename") && root["local_filename"].is_string()) {
    meta.local_filename = root["local_filename"].get<std::string>();
  } else {
    meta.local_filename = kDefaultIconFilename;
  }
  return meta;
}

std::string ProfileIconLocalPath(const std::string& profile_dir) {
  const auto meta = LoadProfileIconCacheMeta(profile_dir);
  if (!meta) {
    return {};
  }
  const std::string filename =
      meta.value().local_filename.empty() ? kDefaultIconFilename : meta.value().local_filename;
  const auto path = std::filesystem::path(ProfileIconCacheRoot(profile_dir)) / filename;
  if (!std::filesystem::exists(path)) {
    return {};
  }
  return path.string();
}

Roe<void> SaveProfileIconCache(const std::string& profile_dir, const std::vector<uint8_t>& bytes,
                               const std::string& file_extension, const ProfileIconCacheMeta& meta) {
  if (bytes.empty()) {
    return Error("Profile icon bytes are empty");
  }
  const std::string ext = file_extension.empty() ? "jpg" : file_extension;
  const std::string filename = "icon." + ext;
  const auto root = std::filesystem::path(ProfileIconCacheRoot(profile_dir));
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

  nlohmann::json root_json = {{"url", saved.url},
                              {"blob_id", saved.blob_id},
                              {"kind", saved.kind},
                              {"fetched_at_ms", saved.fetched_at_ms},
                              {"local_filename", saved.local_filename}};
  const auto meta_path = root / kMetaFilename;
  std::ofstream meta_out(meta_path, std::ios::trunc);
  if (!meta_out) {
    return Error("Failed to write profile icon cache metadata");
  }
  meta_out << root_json.dump(2);
  return Roe<void>{};
}

Roe<void> ClearProfileIconCache(const std::string& profile_dir) {
  const auto root = std::filesystem::path(ProfileIconCacheRoot(profile_dir));
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
