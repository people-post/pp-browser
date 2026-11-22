#pragma once

#include "common/ExtArea.h"
#include "common/ProtocolIds.h"
#include "common/thread/ThreadChannel.h"

#include <cstdint>
#include <optional>
#include <string>

namespace pbr {

/** R019 peer-direct attachment blobs (the wire id is common/ProtocolIds.h's). */
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
   * requester's account key (PeerRequestAuth v2, release-compatibility SIGNED_FORMATS § 2.7). The responder serves
   * the account's thread only when the signature verifies and requester_peer_id is the authenticated link's peer.
   */
  std::string requester_account_id;
  std::string requester_peer_id;
  int64_t timestamp_ms = 0;
  /** The typed signature, base64. */
  std::string signature;
  /** `auth_version`, read before anything else (missing: none sent); PeerRequestAuth::Sign sets it. */
  std::optional<int64_t> auth_version;
  /** The signed extension area (`ext_b64`): empty at release 1, kept as received. */
  pp::ExtArea ext;
  /** Set when `ext_b64` did not read (why): refused as malformed, after the version (PeerRequestAuth::Verify). */
  std::string ext_error;
};

} // namespace pbr
