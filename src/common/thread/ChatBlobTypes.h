#pragma once

#include "common/thread/ThreadChannel.h"

#include <string>

namespace pbr {

enum class ChatBlobOp { Fetch, Push };

struct ChatBlobRequest {
  ChatBlobOp op = ChatBlobOp::Fetch;
  std::string requester_identity_kind;
  std::string requester_identity_value;
  std::string peer_identity_kind;
  std::string peer_identity_value;
  std::string thread_id;
  std::string content_hash_hex;
  ThreadChannel channel = ThreadChannel::E2e;
};

} // namespace pbr
