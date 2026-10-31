#pragma once

#include "common/chat/ChatPayloadTypes.h"
#include "common/thread/ThreadRecordTypes.h"
#include "common/chat/MessagingLimits.h"
#include "domain/attachments/OutgoingAttachmentStore.h"
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
  /**
   * The product path: the ciphertext is staged as a file (`OutgoingAttachmentStore`), streamed from there to the
   * server with `progress`, and stays until the caller removes it, so a failed send can be resumed with the same
   * ciphertext and key. Null: everything stays in memory (tests, one-shot callers).
   */
  OutgoingAttachmentStore* outgoing = nullptr;
  ByteVector dek;
  /** (bytes sent, bytes in all) of the server upload; false cancels. */
  std::function<bool(uint64_t done, uint64_t total)> progress;
  /** Called once the ciphertext is staged, before any network: the record to resume from. */
  std::function<void(const OutgoingAttachmentRecord&)> on_staged;
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

/**
 * Deliver an already staged attachment (a retry, also after the app was killed): the same ciphertext and key, a
 * direct push first when it is small and the contact is reachable, then the server. A record already delivered
 * returns its fields at once. `options.outgoing` and `options.dek` are required.
 */
void ResumeChatAttachmentAsync(IBlobClient& blob, IdentityStore& identity, const OutgoingAttachmentRecord& record,
                               ChatAttachmentUploadOptions options,
                               std::function<void(Roe<ChatAttachmentFields>)> on_done);

/** The same for bytes already in memory (a shrunk picture, a file read by the caller). */
void UploadChatAttachmentAsync(IBlobClient& blob, IdentityStore& identity, const std::vector<uint8_t>& plaintext,
                               const std::string& filename, const std::string& mime,
                               ChatAttachmentUploadOptions options,
                               std::function<void(Roe<ChatAttachmentFields>)> on_done);

} // namespace pbr
