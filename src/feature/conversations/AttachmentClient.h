#pragma once

#include "common/chat/ChatPayloadTypes.h"
#include "common/thread/ThreadRecordTypes.h"
#include "lib/messaging/ChatAttachmentPrepare.h"
#include "lib/net/BlobClient.h"
#include "lib/net/OrgBackendClients.h"
#include "lib/people/ContactsStore.h"
#include "lib/people/IdentityStore.h"

#include "common/Error.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

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
