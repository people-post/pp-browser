#pragma once

#include "common/Error.h"
#include "common/PbrCompat.h"
#include "feature/settings/SettingsPortsViews.h"

#include <string>
#include <vector>

namespace pbr {

class IChatBlobPeerClient;

/**
 * Me → Storage CAS workflows (P3/P4). Domain owns tip/store/index; feature owns
 * settings-facing orchestration. Application wires secrets/messaging readiness only.
 */
Roe<std::vector<CasLibraryItemView>> ListCasLibraryForSettings(const std::string& profile_dir,
                                                               std::string filter);

Roe<void> ShareCasPubliclyForSettings(const std::string& profile_dir, const std::string& profile_id,
                                      const ByteVector& dek, const std::string& private_content_id_hex);

Roe<void> UnpublishCasForSettings(const std::string& profile_dir, const std::string& profile_id,
                                  const std::string& public_content_id_hex);

Roe<void> FetchCasPublicTipForSettings(const std::string& profile_dir, const std::string& profile_id,
                                       IChatBlobPeerClient& blob, const std::string& local_relay_user_id,
                                       const std::string& tip, const std::string& peer_relay_user_id);

} // namespace pbr
