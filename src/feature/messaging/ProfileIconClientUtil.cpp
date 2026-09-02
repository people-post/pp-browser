#include "feature/messaging/ProfileIconClientUtil.h"

#include "domain/people/ProfileIconCache.h"
#include "common/Utilities.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

Roe<std::string> RequireRegisteredRelayUserId(IdentityStore& identity) {
  auto loaded = identity.Get();
  if (!loaded) {
    return loaded.error();
  }
  if (!loaded->registered || loaded->relay_user_id.empty()) {
    return Error("Register on the network before changing your profile icon");
  }
  return loaded->relay_user_id;
}

} // namespace

Roe<BlobPresignResult> UploadPreparedProfileIcon(IBlobClient& blob, IdentityStore& identity,
                                                 const std::string& profile_dir,
                                                 const PreparedProfileIcon& prepared) {
  auto relay_user_id = RequireRegisteredRelayUserId(identity);
  if (!relay_user_id) {
    return relay_user_id.error();
  }
  if (prepared.bytes.empty()) {
    return Error("Profile icon is empty");
  }

  const std::string body(reinterpret_cast<const char*>(prepared.bytes.data()), prepared.bytes.size());
  auto uploaded =
      UploadRelayBlobBytes(blob, *relay_user_id, prepared.content_type, BlobPurpose::Icon, body);
  if (!uploaded) {
    return uploaded.error();
  }

  if (auto attached = blob.SetProfileIcon(*relay_user_id, "", uploaded.value().blob_id, prepared.kind); !attached) {
    return attached.error();
  }

  ProfileIconCacheMeta meta;
  meta.url = uploaded.value().public_url;
  meta.blob_id = uploaded.value().blob_id;
  meta.kind = prepared.kind;
  meta.fetched_at_ms = util::NowUnixMs();
  if (auto saved = SaveProfileIconCache(profile_dir, prepared.bytes, prepared.file_extension, meta); !saved) {
    return saved.error();
  }
  return uploaded.value();
}

Roe<void> ClearHostedProfileIcon(IBlobClient& blob, IdentityStore& identity, const std::string& profile_dir) {
  auto relay_user_id = RequireRegisteredRelayUserId(identity);
  if (!relay_user_id) {
    return relay_user_id.error();
  }
  if (auto cleared = blob.SetProfileIcon(*relay_user_id, "", "", ""); !cleared) {
    return cleared.error();
  }
  if (auto cache = ClearProfileIconCache(profile_dir); !cache) {
    return cache.error();
  }
  return Roe<void>{};
}

} // namespace pbr
