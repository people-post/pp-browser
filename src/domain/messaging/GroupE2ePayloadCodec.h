#pragma once

#include "domain/messaging/GroupTypes.h"
#include "common/thread/ThreadTypes.h"
#include "foundation/crypto/CryptoTypes.h"
#include "foundation/crypto/IPskSessionStore.h"

#include "common/Error.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

struct GroupMemberTarget {
  std::string member_identity;
  ChatTargetKey target_key;
};

struct GroupEncryptResult {
  std::map<std::string, std::string> member_payloads;
  /** Members skipped because pairwise encrypt/key resolution failed. */
  std::vector<std::string> failed_member_identities;
};

/** D095 — N ciphertexts per group message using pairwise e2e_public keys. */
class GroupE2ePayloadCodec {
public:
  static ChatTargetKey PairTargetKey(const std::string& member_identity);
  static Roe<GroupEncryptResult> EncryptForMembers(const std::string& text, const std::string& group_id,
                                                   const std::string& sender_contact_id,
                                                   const std::string& message_id, uint64_t sender_seq,
                                                   uint32_t session_epoch, int64_t timestamp,
                                                   const std::vector<GroupMemberTarget>& members,
                                                   IPskSessionStore& psk_store,
                                                   const std::function<Roe<ByteVector>(const ChatTargetKey&)>&
                                                       resolve_peer_kem_public,
                                                   const std::optional<std::vector<uint8_t>>& chat_payload_plaintext =
                                                       std::nullopt);
  static Roe<ThreadMessage> DecryptForLocalMember(const RelayEnvelope& envelope,
                                                  const std::string& local_contact_id,
                                                  IPskSessionStore& psk_store,
                                                  const std::optional<ByteVector>& local_kem_private_key);
  static bool IsGroupEnvelope(const RelayEnvelope& envelope);
};

} // namespace pbr
