#pragma once

#include "base/adp/Types.h"
#include "base/mesh/session/SessionCrypto.h"
#include "base/mesh/session/SessionKeys.h"
#include "base/mesh/session/Types.h"

#include "common/Error.h"
#include "common/PbrCompat.h"

#include <cstdint>
#include <span>
#include <vector>

namespace pbr::amp {

/** Established L2 session — seal/open L3 payloads. */
class Session {
public:
  static Roe<Session> FromMaterial(SessionMaterial material, ByteVector master_ikm, ByteVector transcript_hash);

  const SessionMaterial& Material() const { return material_; }

  adp::PeerKey AssocKey() const;

  Roe<std::vector<uint8_t>> Seal(uint32_t channel_id, uint32_t channel_seq,
                                 std::span<const uint8_t> plaintext) const;

  Roe<std::vector<uint8_t>> Open(uint32_t channel_id, uint32_t channel_seq,
                                 std::span<const uint8_t> sealed) const;

  /** Bump epoch and re-derive directional keys (assoc key unchanged). */
  Roe<void> Rekey();

private:
  explicit Session(SessionMaterial material, ByteVector master_ikm, ByteVector transcript_hash);

  Direction OutDirection() const;
  Direction InDirection() const;

  SessionMaterial material_;
  ByteVector master_ikm_;
  ByteVector transcript_hash_;
};

} // namespace pbr::amp
