#pragma once

#include "common/chat/ChatPayloadTypes.h"
#include "common/thread/IThreadCatalog.h"
#include "lib/net/OrgBackendClients.h"
#include "lib/people/ContactsStore.h"
#include "lib/people/IdentityStore.h"

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
  /** (bytes received, bytes expected) of a CDN download; false cancels it. */
  std::function<bool(uint64_t done, uint64_t total)> progress;
  /** Tests only: allow a plain-http / loopback CDN url (a peer-controlled url is https + public otherwise). */
  bool allow_local_cdn_for_test = false;
};

/** Where a CDN download is written before it is read back: `{data_dir}/blob_cipher/incoming/<hash>.part`. */
std::string IncomingCiphertextDir(const std::string& profile_data_dir);
/** Remove leftovers of interrupted downloads (run at start). */
void CleanIncomingCiphertext(const std::string& profile_data_dir);

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
