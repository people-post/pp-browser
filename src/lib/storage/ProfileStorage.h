#pragma once

#include "lib/calls/CallStores.h"
#include "lib/content/IObjectIndex.h"
#include "lib/messaging/ProfileStores.h"

#include <memory>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The app's storage (CR024): a profile's stores on its database, profile.db and the per-thread files. Nothing is
 * opened here: the database opens when the profile is unlocked (the thread store gets the data key, D105) —
 * created, or an older one brought to the current schema — before any other store may touch it, so none meets an
 * old one. The app hands this to the hub (`ConversationsHub::SetStorage`).
 */
Roe<ProfileStores> OpenSqliteProfileStores(const std::string& profile_data_dir, const std::string& profile_id);

/**
 * The profile's content index (CR024 stage 4), `object_index.db` beside the profile database. The app hands this to
 * the hub (`ConversationsHub::SetContentStorage`), which builds the profile's content store on it.
 */
std::unique_ptr<IObjectIndex> OpenSqliteObjectIndex(const std::string& profile_data_dir);

/** A profile's call stores on its database (CR024 stage 3), for the calls stack (`CallStack::InitializeStores`). */
Roe<CallStores> OpenSqliteCallStores(const std::string& profile_db_path, const std::string& profile_id);

} // namespace pbr
