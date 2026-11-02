#pragma once

#include "lib/messaging/ProfileStores.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The app's storage (CR024): a profile's stores on its database, profile.db and the per-thread files. The
 * database is opened first — created, or an older one brought to the current schema — so no store meets an old
 * one. The app hands this to the hub (`ConversationsHub::SetStorage`).
 */
Roe<ProfileStores> OpenSqliteProfileStores(const std::string& profile_data_dir, const std::string& profile_id);

} // namespace pbr
