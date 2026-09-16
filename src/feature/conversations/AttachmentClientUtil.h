#pragma once

#include "common/chat/ChatPayloadTypes.h"
#include "common/chat/MessagingLimits.h"
#include "common/thread/ThreadRecordTypes.h"
#include "domain/net/BlobClient.h"
#include "domain/net/OrgBackendClients.h"
#include "domain/people/ContactsStore.h"
#include "domain/people/IdentityStore.h"

#include "common/Error.h"

#include <cstdint>
#include <functional>
#include <optional>
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
  /** Owned copy — required for async upload (no stack Thread*). */
  std::optional<Thread> thread;
  std::string thread_id;
};

/** Peer-direct when reachable (1:1 E2E), else CDN PUT+retain (R015 / R019). */
Roe<ChatAttachmentFields> UploadChatAttachmentFromFile(IBlobClient& blob, IdentityStore& identity,
                                                       const std::string& path,
                                                       const ChatAttachmentUploadOptions& options = {});

void UploadChatAttachmentFromFileAsync(IBlobClient& blob, IdentityStore& identity, const std::string& path,
                                       ChatAttachmentUploadOptions options,
                                       std::function<void(Roe<ChatAttachmentFields>)> on_done);

} // namespace pbr
