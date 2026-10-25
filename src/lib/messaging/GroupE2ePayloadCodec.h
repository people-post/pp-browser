#pragma once

#include "lib/messaging/E2ePublicSessionLogic.h"
#include "lib/messaging/GroupTypes.h"
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

/**
 * One member's copy of a group message (G010/G011): a pairwise e2e_public message, numbered in the pair —
 * the pair's session epoch and this sender's own seq for the group in that pair. A member only ever sees
 * its own copy, so numbering per pair is all a receiver needs; a lost counter is the pair's restart (D014).
 */
struct GroupMemberTarget {
  std::string member_identity;
  ChatTargetKey target_key;
  uint32_t session_epoch = 1;
  uint64_t sender_seq = 0;
};

struct GroupEncryptResult {
  std::map<std::string, std::string> member_payloads;
  /** The AutoKey key_init opening a member's pairwise session on this send (none when it had one). */
  std::map<std::string, std::string> member_key_inits;
  /** Members skipped because pairwise encrypt/key resolution failed. */
  std::vector<std::string> failed_member_identities;
};

/** D095 — N ciphertexts per group message using pairwise e2e_public keys. */
class GroupE2ePayloadCodec {
public:
  static ChatTargetKey PairTargetKey(const std::string& member_identity);
  static Roe<GroupEncryptResult> EncryptForMembers(const std::string& text, const std::string& group_id,
                                                   const std::string& sender_contact_id,
                                                   const std::string& message_id, int64_t timestamp,
                                                   const std::vector<GroupMemberTarget>& members,
                                                   IPskSessionStore& psk_store,
                                                   const std::function<Roe<ByteVector>(const ChatTargetKey&)>&
                                                       resolve_peer_kem_public,
                                                   const std::optional<std::vector<uint8_t>>& chat_payload_plaintext =
                                                       std::nullopt);
  /**
   * The local member's payload, read as the pairwise e2e_public message it is: under the pair's session at
   * the envelope's epoch (or the key_init it carries). `read` says which (ReconcileE2ePublicSession).
   */
  static Roe<ThreadMessage> DecryptForLocalMember(const RelayEnvelope& envelope,
                                                  const std::string& local_contact_id,
                                                  IPskSessionStore& psk_store,
                                                  const std::optional<ByteVector>& local_kem_private_key,
                                                  AutoKeyRead* read = nullptr);
  static bool IsGroupEnvelope(const RelayEnvelope& envelope);
};

} // namespace pbr
