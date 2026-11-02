#pragma once

#include "common/thread/IThreadStore.h"
#include "foundation/crypto/IDekConsumer.h"
#include "lib/delivery/IControlLaneSeenStore.h"
#include "lib/delivery/IPeerSessionStore.h"
#include "lib/messaging/IGroupRosterStore.h"
#include "lib/messaging/IThreadLocalPrefsStore.h"
#include "lib/messaging/IThreadMentionsStore.h"

#include "common/Error.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * A profile's stores, as messaging uses them (CR024): ports only. The app's storage opens them on its database
 * (`src/lib/storage`); tests hand in in-memory ones.
 */
struct ProfileStores {
  std::unique_ptr<IThreadStore> threads;
  std::unique_ptr<IPeerSessionStore> peer_sessions;
  std::unique_ptr<IControlLaneSeenStore> control_lane_seen;
  std::unique_ptr<IGroupRosterStore> rosters;
  std::unique_ptr<IThreadLocalPrefsStore> thread_prefs;
  std::unique_ptr<IThreadMentionsStore> thread_mentions;
  /** The stores above that are encrypted at rest: the hub hands them the profile's data key. */
  std::vector<IDekConsumer*> at_rest;
  /** The profile database, for the stores not behind ports yet (calls', people's cards: CR024 stages 3–4). */
  std::string profile_db_path;
};

/** Opens a profile's stores (`profile_data_dir`, `profile_id`): the app's storage, or a test's in-memory ones. */
using OpenProfileStores =
    std::function<Roe<ProfileStores>(const std::string& profile_data_dir, const std::string& profile_id)>;

} // namespace pbr
