#pragma once

#include "base/messaging/IThreadStore.h"
#include "base/messaging/ThreadTypes.h"

#include "common/Error.h"

#include <string>
#include <vector>

namespace pbr {

/** Authz + local blob serve for `/pp-browser/chat-blob/1.0.0`. */
class ChatBlobResponder {
public:
  static Roe<std::vector<uint8_t>> ServeFetch(IThreadStore& store, const ChatBlobRequest& request,
                                              const std::string& local_relay_user_id,
                                              const std::string& profile_data_dir);

  static Roe<void> ServePush(IThreadStore& store, const ChatBlobRequest& request,
                             const std::string& local_relay_user_id, const std::string& profile_data_dir,
                             const std::vector<uint8_t>& ciphertext);
};

} // namespace pbr
