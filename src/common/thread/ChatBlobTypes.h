#pragma once

#include "common/thread/ThreadChannel.h"

#include <string>

namespace pbr {

/** R019 peer-direct attachment blobs — `/pp-browser/blob/1.0.0`. */
inline constexpr const char* kBlobProtocolId = "/pp-browser/blob/1.0.0";
inline constexpr const char* kChatBlobProtocolId = kBlobProtocolId;

enum class ChatBlobOp { Fetch, Push, FetchPublic };

struct ChatBlobRequest {
  ChatBlobOp op = ChatBlobOp::Fetch;
  std::string requester_identity_kind;
  std::string requester_identity_value;
  std::string peer_identity_kind;
  std::string peer_identity_value;
  std::string thread_id;
  std::string content_hash_hex;
  ThreadChannel channel = ThreadChannel::E2e;
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

} // namespace pbr
