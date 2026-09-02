#pragma once

#include "common/chat/RelayEnvelope.h"
#include "common/thread/ThreadChannel.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

struct ChatHistoryCursor {
  std::optional<uint64_t> next_min_sender_seq;
  std::optional<uint64_t> next_max_sender_seq;
};

struct ChatHistoryRequest {
  std::string requester_identity_kind;
  std::string requester_identity_value;
  std::string peer_identity_kind;
  std::string peer_identity_value;
  ThreadChannel channel = ThreadChannel::E2e;
  uint32_t session_epoch = 1;
  std::optional<uint64_t> min_sender_seq;
  std::optional<uint64_t> max_sender_seq;
  size_t limit = 50;
  std::string order = "asc";
};

struct ChatHistoryResponse {
  std::string peer_identity_kind;
  std::string peer_identity_value;
  ThreadChannel channel = ThreadChannel::E2e;
  uint32_t session_epoch = 1;
  std::vector<RelayEnvelope> messages;
  bool has_more = false;
  ChatHistoryCursor cursor;
};

} // namespace pbr
