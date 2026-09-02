#pragma once

#include "common/chat/ChatPayloadTypes.h"
#include "common/chat/MessagingLimits.h"
#include "common/thread/ThreadRecordTypes.h"
#include "base/net/BlobClient.h"
#include "base/net/ServiceClients.h"
#include "domain/people/ContactsStore.h"
#include "domain/people/IdentityStore.h"

#include "common/Error.h"

#include <cstdint>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

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
