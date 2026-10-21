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

/** Authz + local blob serve for `/pp-browser/blob/1.0.0`. */
class ChatBlobResponder {
public:
  static Roe<std::vector<uint8_t>> ServeFetch(IThreadStore& store, const ChatBlobRequest& request,
                                              const std::string& local_relay_user_id,
                                              const std::string& profile_data_dir,
                                              const ChatBlobContentSource& content);

  static Roe<void> ServePush(IThreadStore& store, const ChatBlobRequest& request,
                             const std::string& local_relay_user_id, const std::string& profile_data_dir,
                             const std::vector<uint8_t>& ciphertext);
};

} // namespace pbr
