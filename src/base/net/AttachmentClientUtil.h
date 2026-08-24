#pragma once

#include "base/messaging/ChatPayloadTypes.h"
#include "base/messaging/ThreadTypes.h"
#include "base/net/BlobClient.h"
#include "base/net/ServiceClients.h"
#include "base/people/ContactsStore.h"
#include "base/people/IdentityStore.h"

#include "common/Error.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pbr {

inline constexpr uint64_t kMaxChatAttachmentPlaintextBytes = 4ULL * 1024ULL * 1024ULL;

struct PreparedChatAttachment {
  ChatAttachmentFields fields;
  std::vector<uint8_t> ciphertext;
};

/** Encrypt file bytes without uploading. */
Roe<PreparedChatAttachment> PrepareChatAttachmentFromFile(const std::string& path);

struct ChatAttachmentUploadOptions {
  IChatBlobPeerClient* peer_client = nullptr;
  ContactsStore* contacts = nullptr;
  const Thread* thread = nullptr;
  std::string thread_id;
};

/** Peer-direct when reachable (1:1 E2E), else CDN PUT+retain (R015 / R019). */
Roe<ChatAttachmentFields> UploadChatAttachmentFromFile(IBlobClient& blob, IdentityStore& identity,
                                                       const std::string& path,
                                                       const ChatAttachmentUploadOptions& options = {});

} // namespace pbr
