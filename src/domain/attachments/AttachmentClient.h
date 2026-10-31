#pragma once

#include "common/chat/ChatPayloadTypes.h"
#include "common/thread/ThreadRecordTypes.h"
#include "common/chat/MessagingLimits.h"
#include "lib/messaging/AttachmentLimits.h"
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
  /** The most plaintext bytes accepted (the caller's AttachmentLimits for this kind of file). */
  uint64_t max_plaintext_bytes = kDefaultAttachmentSendMaxBytes;
};

/**
 * Whether to try the peer-direct push first: only a 1:1 E2E thread, and only up to
 * kMaxDirectAttachmentBytes (the direct path has a 15 s timeout and the peer's responder refuses more).
 * A group, and anything bigger, goes through the server only.
 */
bool AttachmentTriesPeerDirect(const std::optional<Thread>& thread, uint64_t plaintext_bytes);

/** Peer-direct when allowed and reachable (1:1 E2E), else CDN PUT+retain (R015 / R019). */
Roe<ChatAttachmentFields> UploadChatAttachmentFromFile(IBlobClient& blob, IdentityStore& identity,
                                                       const std::string& path,
                                                       const ChatAttachmentUploadOptions& options = {});

void UploadChatAttachmentFromFileAsync(IBlobClient& blob, IdentityStore& identity, const std::string& path,
                                       ChatAttachmentUploadOptions options,
                                       std::function<void(Roe<ChatAttachmentFields>)> on_done);

/** The same for bytes already in memory (a shrunk picture, a file read by the caller). */
void UploadChatAttachmentAsync(IBlobClient& blob, IdentityStore& identity, const std::vector<uint8_t>& plaintext,
                               const std::string& filename, const std::string& mime,
                               ChatAttachmentUploadOptions options,
                               std::function<void(Roe<ChatAttachmentFields>)> on_done);

} // namespace pbr
