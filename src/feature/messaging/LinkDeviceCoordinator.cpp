#include "feature/messaging/LinkDeviceCoordinator.h"

namespace pbr {

Roe<std::vector<LinkDevicePublicPsk>> LinkDeviceCoordinator::CollectPublicPsks(IPskSessionStore& psk_store) {
  auto listed = psk_store.List();
  if (!listed) {
    return listed.error();
  }
  std::vector<LinkDevicePublicPsk> rows;
  for (const PskSessionRecord& record : *listed) {
    if (record.key.channel != CryptoChannel::E2ePublic || !record.master_psk_b64) {
      continue;
    }
    LinkDevicePublicPsk row;
    row.key = record.key;
    row.session_epoch = record.session_epoch;
    row.master_psk_b64 = *record.master_psk_b64;
    row.psk_verified_at = record.psk_verified_at;
    row.retired_psks = record.retired_psks;
    rows.push_back(std::move(row));
  }
  return rows;
}

Roe<void> LinkDeviceCoordinator::ApplyPublicPsks(IPskSessionStore& psk_store,
                                                const std::vector<LinkDevicePublicPsk>& rows) {
  for (const LinkDevicePublicPsk& row : rows) {
    if (row.key.channel != CryptoChannel::E2ePublic) {
      return Error("Link import must not apply private e2e PSKs");
    }
    PskBundleV1 bundle;
    bundle.channel = row.key.channel;
    bundle.active_epoch = row.session_epoch;
    bundle.master_psk_b64 = row.master_psk_b64;
    bundle.retired_epochs = row.retired_psks;
    if (auto imported = psk_store.ImportPskBundle(row.key, bundle); !imported) {
      return imported.error();
    }
  }
  return {};
}

Roe<std::string> LinkDeviceCoordinator::ExportJson(IdentityStore& identity, DataKeyVault& vault,
                                                  IPskSessionStore& psk_store, const int64_t now_ms) {
  auto loaded = identity.Get();
  if (!loaded) {
    return loaded.error();
  }
  auto dek = vault.Dek();
  if (!dek) {
    return dek.error();
  }
  auto public_psks = CollectPublicPsks(psk_store);
  if (!public_psks) {
    return public_psks.error();
  }
  return LinkDeviceExchange::ExportJson(*loaded, *dek, std::move(*public_psks), now_ms);
}

Roe<LinkDeviceImportResult> LinkDeviceCoordinator::Import(IdentityStore& identity, DataKeyVault& vault,
                                                          IPskSessionStore& psk_store, ProfileSecretsService* secrets,
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
  if (auto applied = ApplyPublicPsks(psk_store, imported->public_psks); !applied) {
    return applied.error();
  }
  return imported;
}

} // namespace pbr
