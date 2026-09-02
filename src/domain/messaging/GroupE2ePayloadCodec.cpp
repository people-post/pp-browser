#include "domain/messaging/GroupE2ePayloadCodec.h"

#include "foundation/crypto/AutoKeyEstablishment.h"
#include "foundation/crypto/CryptoUtil.h"
#include "domain/messaging/E2eRelayPayloadCodec.h"
#include "common/chat/MessagingLimits.h"
#include "common/directory/DirectoryJson.h"
#include "common/PbrCompat.h"

namespace pbr {

ChatTargetKey GroupE2ePayloadCodec::PairTargetKey(const std::string& member_identity) {
  ChatTargetKey key;
  key.peer_identity_kind = ContactIdKindToString(ContactIdKind::Account);
  key.peer_identity_value = member_identity;
  key.channel = CryptoChannel::E2ePublic;
  return key;
}

bool GroupE2ePayloadCodec::IsGroupEnvelope(const RelayEnvelope& envelope) {
  return envelope.route.kind == "group" && envelope.route.group_id.has_value();
}

Roe<GroupEncryptResult> GroupE2ePayloadCodec::EncryptForMembers(
    const std::string& text, const std::string& group_id, const std::string& sender_contact_id,
    const std::string& message_id, const uint64_t sender_seq, const uint32_t session_epoch, const int64_t timestamp,
    const std::vector<GroupMemberTarget>& members, IPskSessionStore& psk_store,
    const std::function<Roe<ByteVector>(const ChatTargetKey&)>& resolve_peer_kem_public,
    const std::optional<std::vector<uint8_t>>& chat_payload_plaintext) {
  (void)group_id;
  GroupEncryptResult result;
  for (const GroupMemberTarget& member : members) {
    if (member.member_identity == sender_contact_id) {
      continue;
    }
    ByteVector master_psk;
    std::optional<std::string> key_init_b64;
    auto master_psk_b64 = psk_store.ResolveMasterPskForEpoch(member.target_key, session_epoch);
    if (!master_psk_b64) {
      result.failed_member_identities.push_back(member.member_identity);
      continue;
    }
    if (!master_psk_b64->has_value()) {
      auto peer_public = resolve_peer_kem_public(member.target_key);
      if (!peer_public) {
        result.failed_member_identities.push_back(member.member_identity);
        continue;
      }
      auto established = AutoKeyEstablishment::EncapsulateForRecipient(*peer_public);
      if (!established) {
        result.failed_member_identities.push_back(member.member_identity);
        continue;
      }
      master_psk = std::move(established->master_psk);
      key_init_b64 = std::move(established->key_init_b64);
      PskSessionRecord record;
      record.key = member.target_key;
      record.session_epoch = session_epoch;
      record.master_psk_b64 = Base64Encode(master_psk);
      if (auto saved = psk_store.Save(record); !saved) {
        result.failed_member_identities.push_back(member.member_identity);
        continue;
      }
    } else {
      auto decoded = Base64Decode(**master_psk_b64);
      if (!decoded) {
        result.failed_member_identities.push_back(member.member_identity);
        continue;
      }
      master_psk = std::move(*decoded);
    }

    E2eEncryptParams params;
    params.text = text;
    params.channel = CryptoChannel::E2ePublic;
    params.peer_contact_id = member.member_identity;
    params.sender_contact_id = sender_contact_id;
    params.message_id = message_id;
    params.sender_seq = sender_seq;
    params.session_epoch = session_epoch;
    params.timestamp = timestamp;
    auto encrypted =
        (chat_payload_plaintext && !chat_payload_plaintext->empty())
            ? E2eRelayPayloadCodec::EncryptChatPayloadWithAutoKey(params, *chat_payload_plaintext, master_psk,
                                                                  key_init_b64)
            : E2eRelayPayloadCodec::EncryptTextWithAutoKey(params, master_psk, key_init_b64);
    if (!encrypted) {
      result.failed_member_identities.push_back(member.member_identity);
      continue;
    }
    result.member_payloads[member.member_identity] = std::move(encrypted->payload_b64);
  }
  if (result.member_payloads.empty() && !result.failed_member_identities.empty()) {
    return Error("Failed to encrypt for any group member");
  }
  return result;
}

Roe<ThreadMessage> GroupE2ePayloadCodec::DecryptForLocalMember(const RelayEnvelope& envelope,
                                                               const std::string& local_contact_id,
                                                               IPskSessionStore& psk_store,
                                                               const std::optional<ByteVector>& local_kem_private_key) {
  if (!envelope.body.e2e.member_payloads.has_value()) {
    return Error("Missing group member_payloads");
  }
  const auto it = envelope.body.e2e.member_payloads->find(local_contact_id);
  if (it == envelope.body.e2e.member_payloads->end()) {
    return Error("No ciphertext for local member");
  }
  RelayEnvelope direct_envelope = envelope;
  direct_envelope.route.kind = "direct";
  direct_envelope.route.channel = ThreadChannel::E2ePublic;
  direct_envelope.route.group_id = std::nullopt;
  direct_envelope.body.e2e.payload_b64 = it->second;
  direct_envelope.body.e2e.member_payloads = std::nullopt;

  ChatTargetKey target_key;
  target_key.peer_identity_kind = ContactIdKindToString(ContactIdKind::Account);
  target_key.peer_identity_value = envelope.sender_contact_id;
  target_key.channel = CryptoChannel::E2ePublic;
  return E2eRelayPayloadCodec::DecryptEnvelope(direct_envelope, local_contact_id, target_key, psk_store,
                                               local_kem_private_key);
}

} // namespace pbr
