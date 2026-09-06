#include "domain/messaging/PeerAnnounceKeyResolve.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/identity/PeerIdUtil.h"

#include "common/directory/DirectoryJson.h"

namespace pbr {

std::optional<std::vector<uint8_t>> ResolvePeerAnnouncePublisherKey(
    const std::string_view tip_peer_id, const std::string_view local_peer_id,
    const std::vector<uint8_t>& local_device_public_key, const PeerSigningKeyStore& signing_key_store) {
  if (tip_peer_id.empty()) {
    return std::nullopt;
  }

  if (!local_peer_id.empty() && tip_peer_id == local_peer_id && !local_device_public_key.empty()) {
    return std::vector<uint8_t>(local_device_public_key.begin(), local_device_public_key.end());
  }

  const std::string kind = ContactIdKindToString(ContactIdKind::PeerId);
  auto record = signing_key_store.Get(kind, std::string(tip_peer_id));
  if (!record || record->signing_public_key_b64.empty()) {
    return std::nullopt;
  }
  auto pk = Base64Decode(record->signing_public_key_b64);
  if (!pk || pk->empty()) {
    return std::nullopt;
  }
  if (auto derived = PeerIdFromMlDsaPublicKey(*pk); !derived || *derived != tip_peer_id) {
    return std::nullopt;
  }
  return *pk;
}

} // namespace pbr
