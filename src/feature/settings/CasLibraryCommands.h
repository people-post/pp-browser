#pragma once

#include "lib/content/CasStore.h"
#include "lib/delivery/RelayClients.h"
#include "common/Error.h"
#include "common/PbrCompat.h"
#include "feature/settings/SettingsPortsViews.h"
#include "foundation/crypto/CryptoTypes.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pbr {

class IChatBlobPeerClient;

/**
 * Me → Storage CAS workflows (P3/P4) on the profile's content store (the hub's). Content owns
 * tip/store/index; feature owns settings-facing orchestration. Application wires readiness only.
 */
Roe<std::vector<CasLibraryItemView>> ListCasLibraryForSettings(const CasStore& content, std::string filter);

Roe<void> ShareCasPubliclyForSettings(CasStore& content, const ByteVector& dek,
                                      const std::string& private_content_id_hex);

Roe<void> UnpublishCasForSettings(CasStore& content, const std::string& public_content_id_hex);

/**
 * Fetch a peer's public CAS tip and cache it. Never waits on the mesh: the blob fetch completes
 * asynchronously, the cache write runs on a worker, and `on_done` runs once (on that worker, or
 * inline for an immediate refusal) — callers hop to their own thread.
 */
void FetchCasPublicTipForSettingsAsync(std::shared_ptr<CasStore> content, IChatBlobPeerClient& blob,
                                       const std::string& local_relay_user_id, const std::string& tip,
                                       const std::string& peer_relay_user_id, std::function<void(Roe<void>)> on_done);

} // namespace pbr
