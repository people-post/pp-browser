#pragma once

#include "common/thread/IThreadStore.h"
#include "common/Error.h"
#include "common/PbrCompat.h"

#include <optional>
#include <string>

namespace pbr {

/**
 * Mint or reuse the e2e_public Direct used for call/group control to `peer_identity`.
 * When `prefer_thread_id` is already that peer's e2e_public Direct (e.g. active-call origin),
 * return it instead of creating a second catalog row.
 */
Roe<std::string> ResolveOrCreateE2ePublicDirectThread(IThreadStore& store,
                                                     const std::string& peer_identity,
                                                     const std::optional<std::string>& prefer_thread_id,
                                                     const std::string& contact_id,
                                                     const std::string& dm_title);

} // namespace pbr
