#include "lib/messaging/GroupE2ePayloadCodec.h"

#include "lib/delivery/AutoKeyEstablishment.h"
#include "foundation/crypto/CryptoUtil.h"
#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/delivery/ChatPayloadValidator.h"
#include "lib/delivery/E2eRelayPayloadCodec.h"
#include "common/chat/MessagingLimits.h"
#include "common/directory/DirectoryJson.h"
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

#include <algorithm>
#include <sstream>

namespace pbr {

PeerSessionKey GroupE2ePayloadCodec::PairTargetKey(const std::string& member_identity, const std::string& member_device) {
  PeerSessionKey key;
  key.peer_identity_kind = ContactIdKindToString(mesh::ContactIdKind::Account);
  key.peer_identity_value = member_identity;
  key.channel = CryptoChannel::E2ePublic;
  key.peer_device = member_device;
  return key;
}

bool GroupE2ePayloadCodec::IsGroupEnvelope(const RelayEnvelope& envelope) {
  return envelope.route.kind == kRelayRouteGroup;
}

namespace {

constexpr uint8_t kGroupCopyVersion = 1;
/** A group id is ours (`group:` + a UUID) or another app's: bounded, and plain text (it is stored and bound). */
constexpr size_t kMaxGroupIdBytes = 128;

bool PlainGroupId(const std::string& group_id) {
  if (group_id.empty() || group_id.size() > kMaxGroupIdBytes) {
    return false;
  }
  return std::none_of(group_id.begin(), group_id.end(),
                      [](const char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7F; });
}

} // namespace

Roe<std::vector<uint8_t>> GroupE2ePayloadCodec::EncodeCopyPlaintext(const std::string& group_id,
                                                                    const std::vector<uint8_t>& chat_payload) {
  if (!PlainGroupId(group_id)) {
    return Error("Invalid group id");
  }
  std::ostringstream oss;
  {
    OutputArchive ar(oss);
    ar & kGroupCopyVersion;
    WireLenUtf8 id{group_id};
    ar & id;
    WireLenBytes payload{chat_payload};
    ar & payload;
  }
  const std::string packed = oss.str();
  return std::vector<uint8_t>(packed.begin(), packed.end());
}

Roe<std::pair<std::string, std::vector<uint8_t>>> GroupE2ePayloadCodec::DecodeCopyPlaintext(
    const std::vector<uint8_t>& plaintext) {
  const std::string data(plaintext.begin(), plaintext.end());
  std::istringstream iss(data);
  InputArchive ar(iss);
  uint8_t version = 0;
  ar & version;
  if (ar.failed() || version != kGroupCopyVersion) {
    return Error("Unsupported group copy version");
  }
  WireLenUtf8 group_id;
  ar & group_id;
  WireLenBytes chat_payload;
  ar & chat_payload;
  if (ar.failed() || !ar.exactEnd()) {
    return Error("Malformed group copy");
  }
  if (!PlainGroupId(group_id.value)) {
    return Error("Invalid group id in group copy");
  }
  return std::make_pair(std::move(group_id.value), std::move(chat_payload.value));
}

Roe<GroupEncryptResult> GroupE2ePayloadCodec::EncryptForMembers(
    const std::string& text, const std::string& group_id, const std::string& sender_contact_id,
    const std::string& message_id, const int64_t timestamp,
    const std::vector<GroupMemberTarget>& members, IPeerSessionStore& psk_store,
    const std::function<Roe<ByteVector>(const PeerSessionKey&)>& resolve_peer_kem_public,
    const std::optional<std::vector<uint8_t>>& chat_payload_plaintext) {
  // One plaintext for every copy (G018 item 7): the group id, then the message's ChatPayload.
  std::vector<uint8_t> chat_payload;
  if (chat_payload_plaintext && !chat_payload_plaintext->empty()) {
    chat_payload = *chat_payload_plaintext;
  } else {
    if (auto valid = ChatPayloadValidator::ValidateOutboundText(text); !valid) {
      return valid.error();
    }
    auto encoded = ChatPayloadCodec::EncodeText(text);
    if (!encoded) {
      return encoded.error();
    }
    chat_payload = std::move(*encoded);
  }
  auto plaintext = EncodeCopyPlaintext(group_id, chat_payload);
  if (!plaintext) {
    return plaintext.error();
  }
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
    params.channel = CryptoChannel::E2ePublic;
    params.peer_contact_id = member.member_identity;
    params.sender_contact_id = sender_contact_id;
    params.message_id = message_id;
    params.sender_seq = member.sender_seq;
    params.session_epoch = key_epoch;
    params.timestamp = timestamp;
    auto encrypted = E2eRelayPayloadCodec::EncryptChatPayloadWithAutoKey(params, *plaintext, master_psk, key_init_b64);
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

Roe<GroupCopyRead> GroupE2ePayloadCodec::DecryptForLocalMember(
    const RelayEnvelope& envelope, const std::string& local_contact_id, const PeerSessionKey& target_key,
    IPeerSessionStore& psk_store, const std::optional<ByteVector>& local_kem_private_key, AutoKeyRead* read) {
  // The copy this device reads (SelectRelayCopy: its device's, or the account copy), read as the direct
  // e2e_public message it is.
  if (envelope.body.e2e.payload_b64.empty()) {
    return Error("No copy for this device");
  }
  RelayEnvelope direct_envelope = envelope;
  direct_envelope.route.kind = "direct";
  direct_envelope.route.channel = ThreadChannel::E2ePublic;
  auto plaintext = E2eRelayPayloadCodec::DecryptEnvelopePlaintext(direct_envelope, local_contact_id, target_key,
                                                                  psk_store, local_kem_private_key, read);
  if (!plaintext) {
    return plaintext.error();
  }
  auto copy = DecodeCopyPlaintext(*plaintext);
  if (!copy) {
    return copy.error();
  }
  auto message = E2eRelayPayloadCodec::DecodeChatPayload(copy->second);
  if (!message) {
    return message.error();
  }
  return GroupCopyRead{.group_id = std::move(copy->first), .message = std::move(*message)};
}

} // namespace pbr
