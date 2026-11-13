#include "lib/storage/ProfileStorage.h"

#include "lib/storage/SqliteCallMediaKeyStore.h"
#include "lib/storage/SqliteCallSessionStore.h"
#include "lib/storage/SqliteControlLaneSeenStore.h"
#include "lib/storage/SqliteRelayGrantStore.h"
#include "lib/storage/SqliteGroupInviteLinkStore.h"
#include "lib/storage/SqliteGroupRosterStore.h"
#include "lib/storage/SqliteObjectIndex.h"
#include "lib/storage/SqlitePeerCardStore.h"
#include "lib/storage/SqlitePeerSessionStore.h"
#include "lib/storage/SqliteThreadLocalPrefsStore.h"
#include "lib/storage/SqliteThreadMentionsStore.h"
#include "lib/storage/SqliteThreadSchema.h"
#include "lib/storage/SqliteThreadStore.h"

#include <filesystem>

namespace pbr {

namespace {

/** A SQLite database's bytes on disk: the file and its write-ahead log and shared-memory index. */
uint64_t SqliteFileBytes(const std::filesystem::path& db) {
  uint64_t total = 0;
  for (const char* suffix : {"", "-wal", "-shm"}) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(db.string() + suffix, ec);
    if (!ec) {
      total += size;
    }
  }
  return total;
}

/** The message databases of a profile: profile.db and every chat's thread.db. */
uint64_t SqliteMessageStoreBytes(const std::string& profile_data_dir) {
  uint64_t total = SqliteFileBytes(SqliteThreadProfileDbFile(profile_data_dir));
  std::error_code ec;
  for (std::filesystem::directory_iterator it(SqliteThreadsRoot(profile_data_dir), ec), end; !ec && it != end;
       it.increment(ec)) {
    std::error_code dir_ec;
    if (it->is_directory(dir_ec) && !dir_ec) {
      total += SqliteFileBytes(SqliteThreadDbFile(profile_data_dir, it->path().filename().string()));
    }
  }
  return total;
}

} // namespace

Roe<ProfileStores> OpenSqliteProfileStores(const std::string& profile_data_dir, const std::string& profile_id) {
  // Nothing is opened yet: the chat store opens (and its ladder runs) when the profile's data key arrives at unlock;
  // until then every store on the file refuses (chat_store_gate).
  auto threads = std::make_unique<SqliteThreadStore>(profile_data_dir);
  ProfileStores stores;
  stores.profile_db_path = threads->ProfileDbPath();
  auto peer_sessions = std::make_unique<SqlitePeerSessionStore>(stores.profile_db_path, profile_id);
  auto invite_links = std::make_unique<SqliteGroupInviteLinkStore>(stores.profile_db_path, profile_id);
  stores.at_rest = {threads.get(), peer_sessions.get(), invite_links.get()};
  stores.invite_links = std::move(invite_links);
  stores.threads = std::move(threads);
  stores.peer_sessions = std::move(peer_sessions);
  stores.control_lane_seen = std::make_unique<SqliteControlLaneSeenStore>(stores.profile_db_path);
  stores.relay_grants = std::make_unique<SqliteRelayGrantStore>(stores.profile_db_path);
  stores.rosters = std::make_unique<SqliteGroupRosterStore>(stores.profile_db_path);
  stores.thread_prefs = std::make_unique<SqliteThreadLocalPrefsStore>(stores.profile_db_path);
  stores.thread_mentions = std::make_unique<SqliteThreadMentionsStore>(stores.profile_db_path);
  stores.peer_cards = std::make_unique<SqlitePeerCardStore>(stores.profile_db_path);
  stores.message_store_bytes = [profile_data_dir]() { return SqliteMessageStoreBytes(profile_data_dir); };
  return stores;
}

std::unique_ptr<IObjectIndex> OpenSqliteObjectIndex(const std::string& profile_data_dir) {
  return std::make_unique<SqliteObjectIndex>(profile_data_dir);
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
