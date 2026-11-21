#pragma once

// A relay with a 1-of-1 operator set for tests (release-compatibility SIGNED_FORMATS § 3): its relay id, its
// certificates and the identity answers a fetch returns. Test binaries only; keys come from fixed seeds.

#include "lib/net/RelayCertificate.h"
#include "lib/net/RelayIdentity.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/crypto/TypedBlob.h"

#include "crypto/PeerId.h"

#include <cstdint>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr::test {

/** One relay: an operator key from `operator_seed`, transport keys from any seed, a fixed signing key. */
class TestRelay {
public:
  explicit TestRelay(const uint8_t operator_seed)
      : operator_(Keys(operator_seed)), signing_(Keys(static_cast<uint8_t>(operator_seed ^ 0xA5))) {
    set_ = MakeOperatorSet(1, {TypedSigningPublicKey(operator_.public_key).value()}).value();
  }

  RelayId Id() const { return set_.Id(); }
  std::string IdText() const { return Id().ToString(); }

  /** The PeerId of the transport key from `transport_seed`: the audience its certificates name. */
  static std::string PeerIdOf(const uint8_t transport_seed) {
    return pp::PeerIdFromMlDsaPublicKey(Keys(transport_seed).public_key).value();
  }

  /** Certificate `sequence` (base64) for the transport key from `transport_seed`, listing `origins`. */
  std::string Certificate(const uint64_t sequence, const std::vector<std::string>& origins,
                          const uint8_t transport_seed, const int64_t expires_at = 0,
                          const int64_t issued_at = 1'790'000'000'000) const {
    RelayCertificateBody body{.founding = set_,
                              .changes = {},
                              .sequence = sequence,
                              .issued_at = issued_at,
                              .expires_at = expires_at,
                              .origins = origins,
                              .transport_key = TypedSigningPublicKey(Keys(transport_seed).public_key).value(),
                              .signing_key = TypedSigningPublicKey(signing_.public_key).value(),
                              .ext = {}};
    return Base64Encode(SignRelayCertificate(body, {{.signer_index = 0, .secret_key = operator_.secret_key}}).value());
  }

  /** `GET /v1/relay-identity`'s answer carrying `certificate_b64`, as the relay names itself. */
  RelayIdentity Identity(const std::string& certificate_b64) const {
    return RelayIdentity{.relay_id = Id(), .certificate_b64 = certificate_b64};
  }

private:
  static MlDsaKeyPair Keys(const uint8_t seed) {
    return MlDsa::GenerateKeyPairFromSeed(ByteVector(pp::kMlDsa65SeedBytes, seed)).value();
  }

  MlDsaKeyPair operator_;
  MlDsaKeyPair signing_;
  OperatorSet set_;
};

} // namespace pbr::test
