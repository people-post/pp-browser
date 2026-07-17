#include "base/messaging/AutoKeyEnvelopeResolver.h"

#include "base/crypto/AutoKeyEstablishment.h"
#include "base/crypto/CryptoConstants.h"
#include "base/crypto/CryptoUtil.h"
#include "base/messaging/ThreadChannel.h"

namespace pbr {

Roe<ByteVector> ResolveOrDeriveMasterPsk(const RelayEnvelope& envelope, const ChatTargetKey& target_key,
                                         IPskSessionStore& psk_store, const ByteVector& local_private_key) {
  auto existing = psk_store.ResolveMasterPskForEpoch(target_key, envelope.session_epoch);
  if (!existing) {
    return existing.error();
  }
  if (existing->has_value()) {
    auto decoded = Base64Decode(**existing);
    if (!decoded) {
      return decoded.error();
    }
    if (decoded->size() != kMasterPskSize) {
      return Error("Invalid stored master PSK size");
    }
    return *decoded;
  }

  if (envelope.route.channel != ThreadChannel::E2ePublic) {
    return Error("No PSK for envelope session epoch");
  }
  if (!envelope.body.e2e.key_init_b64 || envelope.body.e2e.key_init_b64->empty()) {
    return Error("Missing key_init_b64 for auto-key decrypt");
  }
  return AutoKeyEstablishment::DeriveMasterPskFromKeyInit(local_private_key, *envelope.body.e2e.key_init_b64);
}

} // namespace pbr
