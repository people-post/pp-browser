#pragma once

#include "common/chat/ChatPayloadTypes.h"
#include "common/thread/IThreadCatalog.h"
#include "domain/net/OrgBackendClients.h"
#include "domain/people/ContactsStore.h"
#include "domain/people/IdentityStore.h"

#include "common/Error.h"

#include <functional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

struct AttachmentFetchContext {
  std::string thread_id;
  IThreadCatalog* store = nullptr;
  ContactsStore* contacts = nullptr;
  IdentityStore* identity = nullptr;
  IChatBlobPeerClient* peer_client = nullptr;
  std::string profile_data_dir;
};

/** True when CDN URL, pending ciphertext, or peer fetch path may apply. */
bool CanFetchAttachment(const ChatAttachmentFields& fields, const AttachmentFetchContext& context);

/** Fetch ciphertext: pending local → peer-direct → CDN GET. */
Roe<std::vector<uint8_t>> FetchAttachmentCiphertext(const ChatAttachmentFields& fields,
                                                    const AttachmentFetchContext& context);
void FetchAttachmentCiphertextAsync(const ChatAttachmentFields& fields, const AttachmentFetchContext& context,
                                    std::function<void(Roe<std::vector<uint8_t>>)> on_done);

/** Decrypt verified attachment bytes (local → peer → CDN ladder). */
Roe<std::vector<uint8_t>> FetchAndDecryptAttachment(const ChatAttachmentFields& fields,
                                                    const AttachmentFetchContext& context);
void FetchAndDecryptAttachmentAsync(const ChatAttachmentFields& fields, const AttachmentFetchContext& context,
                                    std::function<void(Roe<std::vector<uint8_t>>)> on_done);

} // namespace pbr
