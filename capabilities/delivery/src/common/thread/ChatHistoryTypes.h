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
  /**
   * Peer-direct (Amp) requests only: who asks, from which device link, when — signed with the
   * requester's account key (PeerRequestAuth). The responder serves the account's thread only when
   * the signature verifies and requester_peer_id is the authenticated link's peer.
   */
  std::string requester_account_id;
  std::string requester_peer_id;
  int64_t timestamp_ms = 0;
  std::string signature;
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
