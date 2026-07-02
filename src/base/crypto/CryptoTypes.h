#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pbr {

using ByteVector = std::vector<uint8_t>;

/** Wire AAD / sign channel enum (E023). */
enum class CryptoChannel : uint8_t { E2e = 0, E2ePublic = 1 };

inline std::string CryptoChannelToString(const CryptoChannel channel) {
  switch (channel) {
  case CryptoChannel::E2e:
    return "e2e";
  case CryptoChannel::E2ePublic:
    return "e2e_public";
  }
  return "e2e";
}

/** Communicating-identity scoped key for PSK sessions (E008). */
struct ChatTargetKey {
  std::string peer_identity_kind;
  std::string peer_identity_value;
  CryptoChannel channel = CryptoChannel::E2e;

  bool operator==(const ChatTargetKey& other) const {
    return peer_identity_kind == other.peer_identity_kind &&
           peer_identity_value == other.peer_identity_value && channel == other.channel;
  }
};

struct AadFields {
  CryptoChannel channel = CryptoChannel::E2e;
  std::string peer_contact_id;
  std::string message_id;
  std::string sender_contact_id;
  uint64_t sender_seq = 0;
  uint32_t session_epoch = 0;
  int64_t timestamp = 0;
};

} // namespace pbr
