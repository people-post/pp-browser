#pragma once

#include "foundation/crypto/CryptoTypes.h"
#include "common/thread/IThreadStore.h"
#include "common/thread/ThreadTypes.h"

#include "common/Error.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** Where served content comes from: the profile's content store, wired by the feature that serves. */
struct ChatBlobContentSource {
  /** A chat attachment's plaintext by its content hash (private content); none: not served. */
  std::function<Roe<ByteVector>(const std::vector<uint8_t>& content_hash)> private_plaintext;
  /** A pinned public object's bytes by content id; none: public fetches refused. */
  std::function<Roe<ByteVector>(const ByteVector& content_id)> pinned_public;
};

class IPeerSigningKeyResolver;

/**
 * Receiver-side admission for a direct push: true to keep it. Called after the request is authenticated, with
 * the account that signed it and the ciphertext size; false refuses the push (the sender then uses the server).
 * Empty: every push is admitted.
 */
using ChatBlobPushGate = std::function<bool(const std::string& sender, uint64_t ciphertext_bytes)>;

/**
 * Who is asking: the authenticated Amp peer the request came on, and the keys to check that the
 * request is signed for that peer by the account it names (PeerRequestAuth).
 */
struct ChatBlobRequester {
  std::string remote_peer_id;
  IPeerSigningKeyResolver* signing_keys = nullptr;
  int64_t now_ms = 0;
};

/**
 * Authz + local blob serve for `/pp/blob/1.0.0`. Fetch serves only attachments this device
 * sent in the signing requester's thread; Push stores into that thread.
 */
class ChatBlobResponder {
public:
  static Roe<std::vector<uint8_t>> ServeFetch(IThreadStore& store, const ChatBlobRequest& request,
                                              const std::string& local_relay_user_id,
                                              const std::string& profile_data_dir,
                                              const ChatBlobContentSource& content, const ChatBlobRequester& requester);

  static Roe<void> ServePush(IThreadStore& store, const ChatBlobRequest& request,
                             const std::string& local_relay_user_id, const std::string& profile_data_dir,
                             const std::vector<uint8_t>& ciphertext, const ChatBlobRequester& requester,
                             const ChatBlobPushGate& push_gate = {});
};

} // namespace pbr
