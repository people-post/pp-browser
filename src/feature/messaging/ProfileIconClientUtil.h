#pragma once

#include "base/net/BlobClient.h"
#include "domain/people/IdentityStore.h"

#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

struct PreparedProfileIcon {
  std::vector<uint8_t> bytes;
  std::string content_type;
  std::string kind;
  std::string file_extension;
};

Roe<BlobPresignResult> UploadPreparedProfileIcon(IBlobClient& blob, IdentityStore& identity,
                                                 const std::string& profile_dir,
                                                 const PreparedProfileIcon& prepared);

Roe<void> ClearHostedProfileIcon(IBlobClient& blob, IdentityStore& identity, const std::string& profile_dir);

} // namespace pbr
