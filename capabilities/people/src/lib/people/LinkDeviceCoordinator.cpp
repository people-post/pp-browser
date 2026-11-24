#include "lib/people/LinkDeviceCoordinator.h"
#include "common/PbrCompat.h"

namespace pbr {

Roe<ByteVector> LinkDeviceCoordinator::SealReply(IdentityStore& identity, const std::span<const uint8_t> offer,
                                                 std::optional<RelayAddress> relay, const int64_t now_ms,
                                                 const int64_t ttl_ms) {
  auto loaded = identity.Get();
  if (!loaded) {
    return loaded.error();
  }
  return LinkDeviceExchange::SealReply(*loaded, offer, std::move(relay), now_ms, ttl_ms);
}

Roe<LocalIdentity> LinkDeviceCoordinator::ImportAccount(IdentityStore& identity, DataKeyVault& vault,
                                                        ProfileSecretsEngine* secrets, const ByteVector& seed,
                                                        const std::string_view card_intent, const std::string& nickname,
                                                        const SecretText& pin) {
  auto imported = LinkDeviceExchange::ImportAccount(identity, vault, seed, card_intent, nickname, pin);
  if (!imported) {
    return imported.error();
  }
  if (secrets != nullptr) {
    if (auto redistributed = secrets->RedistributeUnlockedDek(); !redistributed) {
      return redistributed.error();
    }
  }
  return imported;
}

} // namespace pbr
