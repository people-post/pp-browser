#include "lib/storage/ProfileStorage.h"

#include "lib/storage/SqliteCallMediaKeyStore.h"
#include "lib/storage/SqliteCallSessionStore.h"
#include "lib/storage/SqliteControlLaneSeenStore.h"
#include "lib/storage/SqliteGroupRosterStore.h"
#include "lib/storage/SqlitePeerSessionStore.h"
#include "lib/storage/SqliteThreadLocalPrefsStore.h"
#include "lib/storage/SqliteThreadMentionsStore.h"
#include "lib/storage/SqliteThreadStore.h"

namespace pbr {

Roe<ProfileStores> OpenSqliteProfileStores(const std::string& profile_data_dir, const std::string& profile_id) {
  auto threads = std::make_unique<SqliteThreadStore>(profile_data_dir);
  if (auto opened = threads->Open(); !opened) {
    return opened.error();
  }
  ProfileStores stores;
  stores.profile_db_path = threads->ProfileDbPath();
  auto peer_sessions = std::make_unique<SqlitePeerSessionStore>(stores.profile_db_path, profile_id);
  stores.at_rest = {threads.get(), peer_sessions.get()};
  stores.threads = std::move(threads);
  stores.peer_sessions = std::move(peer_sessions);
  stores.control_lane_seen = std::make_unique<SqliteControlLaneSeenStore>(stores.profile_db_path);
  stores.rosters = std::make_unique<SqliteGroupRosterStore>(stores.profile_db_path);
  stores.thread_prefs = std::make_unique<SqliteThreadLocalPrefsStore>(stores.profile_db_path);
  stores.thread_mentions = std::make_unique<SqliteThreadMentionsStore>(stores.profile_db_path);
  return stores;
}

Roe<CallStores> OpenSqliteCallStores(const std::string& profile_db_path, const std::string& profile_id) {
  CallStores stores;
  auto media_keys = std::make_unique<SqliteCallMediaKeyStore>(profile_db_path, profile_id);
  stores.at_rest = {media_keys.get()};
  stores.media_keys = std::move(media_keys);
  stores.sessions = std::make_unique<SqliteCallSessionStore>(profile_db_path);
  return stores;
}

} // namespace pbr
