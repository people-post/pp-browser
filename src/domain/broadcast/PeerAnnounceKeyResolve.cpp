#include "domain/broadcast/PeerAnnounceKeyResolve.h"

#include "foundation/crypto/CryptoUtil.h"
#include "crypto/PeerId.h"

#include "common/directory/DirectoryJson.h"
#include "common/MeshAlias.h"

namespace pbr {

std::optional<std::vector<uint8_t>> ResolvePeerAnnouncePublisherKey(
    const std::string_view tip_peer_id, const std::string_view local_peer_id,
    const std::vector<uint8_t>& local_device_public_key, const StoredSigningKeyLookup& stored_signing_key) {
  if (tip_peer_id.empty()) {
    return std::nullopt;
  }

  if (!local_peer_id.empty() && tip_peer_id == local_peer_id && !local_device_public_key.empty()) {
    return std::vector<uint8_t>(local_device_public_key.begin(), local_device_public_key.end());
  }

  if (!stored_signing_key) {
    return std::nullopt;
  }
  const auto stored =
      stored_signing_key(ContactIdKindToString(mesh::ContactIdKind::PeerId), std::string(tip_peer_id));
  if (!stored || stored->empty()) {
    return std::nullopt;
  }
  auto pk = Base64Decode(*stored);
  if (!pk || pk->empty()) {
    return std::nullopt;
  }
  if (auto derived = pp::PeerIdFromMlDsaPublicKey(*pk); !derived || *derived != tip_peer_id) {
    return std::nullopt;
  }
  return *pk;
}

} // namespace pbr
