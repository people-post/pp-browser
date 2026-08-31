#include "base/mesh/session/Session.h"

#include <cstring>

namespace pbr::amp {

Session::Session(SessionMaterial material, ByteVector master_ikm, ByteVector transcript_hash)
    : material_(std::move(material)),
      master_ikm_(std::move(master_ikm)),
      transcript_hash_(std::move(transcript_hash)) {}

Roe<Session> Session::FromMaterial(SessionMaterial material, ByteVector master_ikm, ByteVector transcript_hash) {
  if (material.k_assoc.size() != kAssocKeyBytes || material.k_send.size() != kSessionKeyBytes
      || material.k_recv.size() != kSessionKeyBytes) {
    return Error("amp session: bad key sizes");
  }
  if (master_ikm.empty()) {
    return Error("amp session: empty master ikm");
  }
  if (transcript_hash.empty()) {
    return Error("amp session: empty transcript hash");
  }
  return Session(std::move(material), std::move(master_ikm), std::move(transcript_hash));
}

adp::PeerKey Session::AssocKey() const {
  adp::PeerKey key;
  std::memcpy(key.bytes.data(), material_.k_assoc.data(), kAssocKeyBytes);
  return key;
}

Direction Session::OutDirection() const {
  return material_.initiator ? Direction::InitiatorToResponder : Direction::ResponderToInitiator;
}

Direction Session::InDirection() const {
  return material_.initiator ? Direction::ResponderToInitiator : Direction::InitiatorToResponder;
}

Roe<std::vector<uint8_t>> Session::Seal(const uint32_t channel_id, const uint32_t channel_seq,
                                        std::span<const uint8_t> plaintext) const {
  return SessionCrypto::Seal(material_.k_send, material_.session_epoch, channel_id, channel_seq, OutDirection(),
                           plaintext);
}

Roe<std::vector<uint8_t>> Session::Open(const uint32_t channel_id, const uint32_t channel_seq,
                                       std::span<const uint8_t> sealed) const {
  return SessionCrypto::Open(material_.k_recv, material_.session_epoch, channel_id, channel_seq, InDirection(), sealed);
}

Roe<void> Session::Rekey() {
  material_.session_epoch += 1;
  auto refreshed = SessionKeys::Derive(master_ikm_, transcript_hash_, material_.initiator, material_.session_epoch);
  if (!refreshed) {
    return refreshed.error();
  }
  material_.k_send = std::move(refreshed->k_send);
  material_.k_recv = std::move(refreshed->k_recv);
  return Roe<void>();
}

} // namespace pbr::amp
