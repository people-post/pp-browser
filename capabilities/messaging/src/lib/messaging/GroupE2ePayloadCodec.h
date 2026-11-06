#pragma once

#include "lib/delivery/E2ePublicSessionLogic.h"
#include "lib/messaging/GroupTypes.h"
#include "common/thread/ThreadTypes.h"
#include "foundation/crypto/CryptoTypes.h"
#include "lib/delivery/IPeerSessionStore.h"

#include "common/Error.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * One copy of a group message (G010/G011): a pairwise e2e_public message, numbered in the pair — the
 * pair's session epoch and this sender's own seq for the group in that pair. The pair is a device pair
 * (M021: `target_key.peer_device` is one of the member's devices), or the member's account when we know
 * no device of theirs yet (an account copy). A copy is only ever read in its pair, so numbering per pair
 * is all a receiver needs; a lost counter is the pair's restart (D014).
 */
struct GroupMemberTarget {
  std::string member_identity;
  PeerSessionKey target_key;
  uint32_t session_epoch = 1;
  uint64_t sender_seq = 0;
};

/** A copy as encrypted for its target. */
struct GroupMemberCopy {
  std::string payload_b64;
  /** The AutoKey key_init opening the pairwise session on this send (none when it had one). */
  std::optional<std::string> key_init_b64;
};

/**
 * G018 item 5: what a group copy carries of its sender's roster, inside the ciphertext — the epoch of the owner's
 * statement it holds and the hash of the unfolded changes it holds (group_change::ChangesHash). A receiver that
 * holds something else checks its members (G013).
 */
struct GroupCopyState {
  uint64_t statement_epoch = 0;
  /** Lowercase hex, 64 characters; empty only from a sender that holds no statement yet. */
  std::string changes_hash;

  bool operator==(const GroupCopyState&) const = default;
};

/** A group copy as its member reads it: the group it names (inside the ciphertext, G018 item 7) and the message. */
struct GroupCopyRead {
  std::string group_id;
  ThreadMessage message;
  /** The sender's roster as the copy states it (G018 item 5). */
  GroupCopyState state;
};

/** A group copy's plaintext, decoded (GroupE2ePayloadCodec::DecodeCopyPlaintext). */
struct GroupCopyPlaintext {
  std::string group_id;
  GroupCopyState state;
  std::vector<uint8_t> chat_payload;
};

struct GroupEncryptResult {
  /** One per target, in order: its copy, or none when it could not be encrypted. */
  std::vector<std::optional<GroupMemberCopy>> copies;
  /** Members with a copy that could not be encrypted (key resolution or encrypt failed). */
  std::vector<std::string> failed_member_identities;
};

/** D095 — N ciphertexts per group message using pairwise e2e_public keys. */
class GroupE2ePayloadCodec {
public:
  /** The pair's session key with `member_identity` — with one of its devices, or its account (M021). */
  static PeerSessionKey PairTargetKey(const std::string& member_identity, const std::string& member_device = {});
  static Roe<GroupEncryptResult> EncryptForMembers(
      const std::string& text, const std::string& group_id, const std::string& sender_contact_id,
      const std::string& message_id, int64_t timestamp, const std::vector<GroupMemberTarget>& members,
      IPeerSessionStore& psk_store,
      const std::function<Roe<ByteVector>(const PeerSessionKey&)>& resolve_peer_kem_public,
      const std::optional<std::vector<uint8_t>>& chat_payload_plaintext = std::nullopt,
      const GroupCopyState& state = {});
  /**
   * The local member's payload, read as the pairwise e2e_public message it is: under the pair's session
   * (`target_key`) at the envelope's epoch (or the key_init it carries). `read` says which
   * (ReconcileE2ePublicSession). The group it is for comes out of the plaintext: the caller checks, now, that
   * we and the sender are on that group's roster.
   */
  static Roe<GroupCopyRead> DecryptForLocalMember(const RelayEnvelope& envelope, const std::string& local_contact_id,
                                                  const PeerSessionKey& target_key, IPeerSessionStore& psk_store,
                                                  const std::optional<ByteVector>& local_kem_private_key,
                                                  AutoKeyRead* read = nullptr);
  static bool IsGroupEnvelope(const RelayEnvelope& envelope);

  /**
   * A group copy's AEAD plaintext (G018 items 5 and 7): `group_copy_version` u8 = 2, `group_id` LenUtf8,
   * `statement_epoch` u64 BE, `changes_hash` LenUtf8, the message's ChatPayload LenBytes, nothing after. The relay
   * sees no group id; the AEAD binds the copy to the group it names.
   */
  static Roe<std::vector<uint8_t>> EncodeCopyPlaintext(const std::string& group_id,
                                                       const std::vector<uint8_t>& chat_payload,
                                                       const GroupCopyState& state = {});
  /** What a copy's plaintext carries; malformed, another version, a bad id or a bad hash: an error. */
  static Roe<GroupCopyPlaintext> DecodeCopyPlaintext(const std::vector<uint8_t>& plaintext);
};

} // namespace pbr
