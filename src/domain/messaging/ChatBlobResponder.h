#pragma once

#include "foundation/crypto/CryptoTypes.h"
#include "common/thread/IThreadStore.h"
#include "common/thread/ThreadTypes.h"

#include "common/Error.h"

#include <string>
#include <string_view>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** Authz + local blob serve for `/pp-browser/blob/1.0.0`. */
class ChatBlobResponder {
public:
  static Roe<std::vector<uint8_t>> ServeFetch(IThreadStore& store, const ChatBlobRequest& request,
                                              const std::string& local_relay_user_id,
                                              const std::string& profile_data_dir,
                                              const ByteVector* dek = nullptr,
                                              std::string_view profile_id = {});

  static Roe<void> ServePush(IThreadStore& store, const ChatBlobRequest& request,
                             const std::string& local_relay_user_id, const std::string& profile_data_dir,
                             const std::vector<uint8_t>& ciphertext);
};

} // namespace pbr
