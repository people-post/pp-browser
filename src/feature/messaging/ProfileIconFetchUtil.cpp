#include "feature/messaging/ProfileIconFetchUtil.h"

#include "domain/net/HttpClient.h"
#include "common/Utilities.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

std::string ExtensionFromProfileIcon(const ProfileIconRef& icon) {
  if (!icon.kind.empty()) {
    if (icon.kind == "image/jpeg" || icon.kind == "image/jpg") {
      return "jpg";
    }
    if (icon.kind == "image/png") {
      return "png";
    }
    if (icon.kind == "image/webp") {
      return "webp";
    }
  }
  const auto pos = icon.url.rfind('.');
  if (pos != std::string::npos && pos + 1 < icon.url.size()) {
    const std::string ext = icon.url.substr(pos + 1);
    if (ext.size() <= 4) {
      return ext;
    }
  }
  return "jpg";
}

std::vector<uint8_t> BytesFromHttpBody(const std::string& body) {
  return std::vector<uint8_t>(body.begin(), body.end());
}

} // namespace

bool ProfileIconNeedsFetch(const std::string& profile_dir, const std::string& cache_key,
                           const ProfileIconRef& icon) {
  if (icon.url.empty() || cache_key.empty()) {
    return false;
  }
  const auto meta = LoadProfileIconCacheMeta(profile_dir, cache_key);
  if (!meta) {
    return true;
  }
  const ProfileIconCacheMeta& cached = meta.value();
  if (!icon.url.empty() && !cached.url.empty() && cached.url != icon.url) {
    return true;
  }
  if (!icon.blob_id.empty() && cached.blob_id != icon.blob_id) {
    return true;
  }
  return ProfileIconLocalPath(profile_dir, cache_key).empty();
}

Roe<void> FetchProfileIcon(const std::string& profile_dir, const std::string& cache_key,
                           const ProfileIconRef& icon) {
  if (icon.url.empty()) {
    return Error("Profile icon URL is required");
  }
  if (cache_key.empty()) {
    return Error("Profile icon cache key is required");
  }
  const auto response = HttpClient::Get(icon.url);
  if (!response) {
    return response.error();
  }
  const HttpResponse& http = response.value();
  if (http.status_code < 200 || http.status_code >= 300) {
    return Error("Profile icon download failed with status " + std::to_string(http.status_code));
  }
  if (http.body.empty()) {
    return Error("Profile icon download returned empty body");
  }

  ProfileIconCacheMeta meta;
  meta.url = icon.url;
  meta.blob_id = icon.blob_id;
  meta.kind = icon.kind;
  meta.fetched_at_ms = util::NowUnixMs();
  return SaveProfileIconCache(profile_dir, BytesFromHttpBody(http.body), ExtensionFromProfileIcon(icon), meta,
                              cache_key);
}

Roe<void> FetchProfileIconIfNeeded(const std::string& profile_dir, const std::string& cache_key,
                                   const ProfileIconRef& icon) {
  if (icon.url.empty() || cache_key.empty()) {
    return Roe<void>{};
  }
  if (!ProfileIconNeedsFetch(profile_dir, cache_key, icon)) {
    return Roe<void>{};
  }
  return FetchProfileIcon(profile_dir, cache_key, icon);
}

} // namespace pbr
