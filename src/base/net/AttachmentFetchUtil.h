#pragma once

#include "base/messaging/ChatPayloadTypes.h"
#include "base/messaging/IThreadStore.h"
#include "base/messaging/ThreadTypes.h"
#include "base/net/ServiceClients.h"
#include "base/people/ContactsStore.h"
#include "base/people/IdentityStore.h"

#include "common/Error.h"

#include <string>
#include <vector>

namespace pbr {

struct AttachmentFetchContext {
  std::string thread_id;
  IThreadStore* store = nullptr;
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

/** Decrypt verified attachment bytes (local → peer → CDN ladder). */
Roe<std::vector<uint8_t>> FetchAndDecryptAttachment(const ChatAttachmentFields& fields,
                                                    const AttachmentFetchContext& context);

} // namespace pbr
