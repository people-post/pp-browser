#include "lib/messaging/GroupE2ePayloadCodec.h"

#include "foundation/crypto/AutoKeyEstablishment.h"
#include "foundation/crypto/CryptoUtil.h"
#include "lib/messaging/E2eRelayPayloadCodec.h"
#include "common/chat/MessagingLimits.h"
#include "common/directory/DirectoryJson.h"
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

ChatTargetKey GroupE2ePayloadCodec::PairTargetKey(const std::string& member_identity, const std::string& member_device) {
  ChatTargetKey key;
  key.peer_identity_kind = ContactIdKindToString(mesh::ContactIdKind::Account);
  key.peer_identity_value = member_identity;
  key.channel = CryptoChannel::E2ePublic;
  key.peer_device = member_device;
  return key;
}

bool GroupE2ePayloadCodec::IsGroupEnvelope(const RelayEnvelope& envelope) {
  return envelope.route.kind == "group" && envelope.route.group_id.has_value();
}

Roe<GroupEncryptResult> GroupE2ePayloadCodec::EncryptForMembers(
    const std::string& text, const std::string& group_id, const std::string& sender_contact_id,
    const std::string& message_id, const int64_t timestamp,
    const std::vector<GroupMemberTarget>& members, IPskSessionStore& psk_store,
    const std::function<Roe<ByteVector>(const ChatTargetKey&)>& resolve_peer_kem_public,
    const std::optional<std::vector<uint8_t>>& chat_payload_plaintext) {
  (void)group_id;
  GroupEncryptResult result;
  bool any = false;
  const auto failed = [&result](const GroupMemberTarget& member) {
    result.copies.emplace_back();
    result.failed_member_identities.push_back(member.member_identity);
  };
  for (const GroupMemberTarget& member : members) {
    if (member.member_identity == sender_contact_id && member.target_key.peer_device.empty()) {
      result.copies.emplace_back();  // never an account copy to ourselves
      continue;
    }
    // Each member's copy is a pairwise e2e_public message at the pair's own epoch, opened (and announced
    // until the peer is heard under it) exactly as a 1:1 send opens it (G011).
    auto loaded = psk_store.Load(member.target_key);
    if (!loaded) {
      failed(member);
      continue;
    }
    const uint32_t key_epoch = member.session_epoch;
    ByteVector master_psk;
    std::optional<std::string> key_init_b64;
    auto stored = psk_store.ResolveMasterPskForEpoch(member.target_key, key_epoch);
    if (!stored) {
      failed(member);
      continue;
    }
    if (stored->has_value()) {
      auto decoded = Base64Decode(**stored);
      if (!decoded) {
        failed(member);
        continue;
      }
      master_psk = std::move(*decoded);
      if (loaded->has_value() && (*loaded)->session_epoch == key_epoch) {
        key_init_b64 = (*loaded)->auto_key_init_pending;  // ours, not confirmed yet: keep announcing it
      }
    } else {
      auto peer_public = resolve_peer_kem_public(member.target_key);
      if (!peer_public) {
        failed(member);
        continue;
      }
      auto ensured = EnsureE2ePublicMasterPsk(psk_store, member.target_key, key_epoch, *peer_public);
      if (!ensured) {
        failed(member);
        continue;
      }
      master_psk = std::move(ensured->master_psk);
      key_init_b64 = std::move(ensured->key_init_b64);
    }

    E2eEncryptParams params;
    params.text = text;
    params.channel = CryptoChannel::E2ePublic;
    params.peer_contact_id = member.member_identity;
    params.sender_contact_id = sender_contact_id;
    params.message_id = message_id;
    params.sender_seq = member.sender_seq;
    params.session_epoch = key_epoch;
    params.timestamp = timestamp;
    auto encrypted =
        (chat_payload_plaintext && !chat_payload_plaintext->empty())
            ? E2eRelayPayloadCodec::EncryptChatPayloadWithAutoKey(params, *chat_payload_plaintext, master_psk,
                                                                  key_init_b64)
            : E2eRelayPayloadCodec::EncryptTextWithAutoKey(params, master_psk, key_init_b64);
    if (!encrypted) {
      failed(member);
      continue;
    }
    GroupMemberCopy copy{.payload_b64 = std::move(encrypted->payload_b64)};
    if (encrypted->key_init_b64 && !encrypted->key_init_b64->empty()) {
      copy.key_init_b64 = std::move(*encrypted->key_init_b64);
    }
    result.copies.push_back(std::move(copy));
    any = true;
  }
  if (!any && !result.failed_member_identities.empty()) {
    return Error("Failed to encrypt for any group member");
  }
  return result;
}

Roe<ThreadMessage> GroupE2ePayloadCodec::DecryptForLocalMember(const RelayEnvelope& envelope,
                                                               const std::string& local_contact_id,
                                                               const ChatTargetKey& target_key,
                                                               IPskSessionStore& psk_store,
                                                               const std::optional<ByteVector>& local_kem_private_key,
                                                               AutoKeyRead* read) {
  // The copy this device reads (SelectRelayCopy: its device's, or the account copy), read as the direct
  // e2e_public message it is.
  if (envelope.body.e2e.payload_b64.empty()) {
    return Error("No copy for this device");
  }
  RelayEnvelope direct_envelope = envelope;
  direct_envelope.route.kind = "direct";
  direct_envelope.route.channel = ThreadChannel::E2ePublic;
  direct_envelope.route.group_id = std::nullopt;
  return E2eRelayPayloadCodec::DecryptEnvelope(direct_envelope, local_contact_id, target_key, psk_store,
                                               local_kem_private_key, read);
}

} // namespace pbr
