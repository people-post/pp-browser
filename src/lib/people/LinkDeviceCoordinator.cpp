#include "lib/people/LinkDeviceCoordinator.h"
#include "common/PbrCompat.h"

namespace pbr {

Roe<std::string> LinkDeviceCoordinator::ExportJson(IdentityStore& identity, DataKeyVault& vault,
                                                  std::string account_card_json, const int64_t now_ms,
                                                  const int64_t ttl_ms) {
  auto loaded = identity.Get();
  if (!loaded) {
    return loaded.error();
  }
  auto dek = vault.Dek();
  if (!dek) {
    return dek.error();
  }
  return LinkDeviceExchange::ExportJson(*loaded, *dek, std::move(account_card_json), now_ms, ttl_ms);
}

Roe<LinkDeviceImportResult> LinkDeviceCoordinator::Import(IdentityStore& identity, DataKeyVault& vault,
                                                          ProfileSecretsEngine* secrets,
                                                          const std::string& bundle_json, const std::string_view pin,
                                                          const int64_t now_ms) {
  auto imported = LinkDeviceExchange::Import(identity, vault, bundle_json, pin, now_ms);
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
