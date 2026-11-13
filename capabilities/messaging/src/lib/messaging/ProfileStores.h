#pragma once

#include "common/thread/IThreadStore.h"
#include "foundation/crypto/IDekConsumer.h"
#include "lib/delivery/IControlLaneSeenStore.h"
#include "lib/delivery/IPeerSessionStore.h"
#include "lib/delivery/IRelayGrantStore.h"
#include "lib/messaging/IGroupInviteLinkStore.h"
#include "lib/messaging/IGroupRosterStore.h"
#include "lib/messaging/IThreadLocalPrefsStore.h"
#include "lib/messaging/IThreadMentionsStore.h"
#include "lib/people/PeerCards.h"

#include "common/Error.h"

#include <cstdint>
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
  /** The relay grants we hold and gave (multi-relay MR005); none: sends go as a stranger's, none are issued. */
  std::unique_ptr<IRelayGrantStore> relay_grants;
  std::unique_ptr<IGroupRosterStore> rosters;
  /** The invite links this device made, their secrets encrypted (G021); none: links are made but not shown again. */
  std::unique_ptr<IGroupInviteLinkStore> invite_links;
  std::unique_ptr<IThreadLocalPrefsStore> thread_prefs;
  std::unique_ptr<IThreadMentionsStore> thread_mentions;
  /** Peers' cards (people's rows; PeerCards is the logic over them). */
  std::unique_ptr<IPeerCardStore> peer_cards;
  /** The stores above that are encrypted at rest: the hub hands them the profile's data key. */
  std::vector<IDekConsumer*> at_rest;
  /** The profile database, for the calls stack's stores (`OpenSqliteCallStores`). */
  std::string profile_db_path;
  /** Bytes the message databases take on disk now (Me → Storage's "chats" share); none: not known. Blocking. */
  std::function<uint64_t()> message_store_bytes;
};

/** Opens a profile's stores (`profile_data_dir`, `profile_id`): the app's storage, or a test's in-memory ones. */
using OpenProfileStores =
    std::function<Roe<ProfileStores>(const std::string& profile_data_dir, const std::string& profile_id)>;

} // namespace pbr
