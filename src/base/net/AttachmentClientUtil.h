#pragma once

#include "base/messaging/ChatPayloadTypes.h"
#include "base/net/BlobClient.h"
#include "base/people/IdentityStore.h"

#include "common/Error.h"

#include <cstdint>
#include <string>

namespace pbr {

inline constexpr uint64_t kMaxChatAttachmentPlaintextBytes = 4ULL * 1024ULL * 1024ULL;

/** Encrypt plaintext file bytes, upload ciphertext to relay blob storage, return attachment metadata. */
Roe<ChatAttachmentFields> UploadChatAttachmentFromFile(IBlobClient& blob, IdentityStore& identity,
                                                       const std::string& path);

} // namespace pbr
