#include "lib/net/ProfileIconClient.h"

#include "lib/people/ProfileIconCache.h"
#include "common/Utilities.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

Roe<std::string> RequireRegisteredAccountId(IdentityStore& identity) {
  auto loaded = identity.Get();
  if (!loaded) {
    return loaded.error();
  }
  if (!loaded->registered || loaded->account_id.empty()) {
    return Error("Register on the network before changing your profile icon");
  }
  return loaded->account_id;
}

} // namespace

Roe<BlobPresignResult> UploadPreparedProfileIcon(IBlobClient& blob, IdentityStore& identity,
                                                 const std::string& profile_dir, const PreparedProfileIcon& prepared) {
  auto account_id = RequireRegisteredAccountId(identity);
  if (!account_id) {
    return account_id.error();
  }
  if (prepared.bytes.empty()) {
    return Error("Profile icon is empty");
  }

  const std::string body(reinterpret_cast<const char*>(prepared.bytes.data()), prepared.bytes.size());
  auto uploaded = UploadRelayBlobBytes(blob, *account_id, prepared.content_type, BlobPurpose::Icon, body);
  if (!uploaded) {
    return uploaded.error();
  }

  if (auto attached = blob.SetProfileIcon(*account_id, "", uploaded.value().blob_id, prepared.kind); !attached) {
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
  auto account_id = RequireRegisteredAccountId(identity);
  if (!account_id) {
    return account_id.error();
  }
  if (auto cleared = blob.SetProfileIcon(*account_id, "", "", ""); !cleared) {
    return cleared.error();
  }
  if (auto cache = ClearProfileIconCache(profile_dir); !cache) {
    return cache.error();
  }
  return Roe<void>{};
}

} // namespace pbr
