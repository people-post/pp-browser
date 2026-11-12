#pragma once

#include "lib/storage/SqliteChatStoreGate.h"
#include "lib/storage/SqliteMigrations.h"

#include <string>

#include "common/PbrCompat.h"

namespace pbr {

/**
 * The chat store's two ladders (D105; DATA_LAYOUT § Schema versions): profile.db — the catalog, outbox, sessions and
 * every store that keeps its tables there (groups, thread preferences, calls, …) — and each chat's thread.db.
 * A store may still create a table of its own that is new (CREATE TABLE IF NOT EXISTS, at the current shape); a
 * change to a table that may already exist is a step here.
 */
const SqliteSchemaLadder& ProfileDbLadder();
const SqliteSchemaLadder& ThreadDbLadder();

/**
 * What a profile.db ladder outcome leaves the chat store with: nothing to recover from (created, current, migrated,
 * or reset below the floor — the app says that once), Newer (a downgrade), or NotUpgraded (a step failed and was
 * rolled back, or the file could not be read).
 */
std::optional<ChatStoreProblem> ChatStoreProblemFor(const Roe<SqliteLadderOutcome>& outcome);

/**
 * A chat database older than its floor is reset (profile.db: the whole chat store; thread.db: that chat's
 * messages). Recorded in the profile directory so the app can say so once after start-up.
 */
void NoteChatStoreReset(const std::string& profile_data_dir, const std::string& what);
/** The reset note, once: true (and the note is gone) when a chat database was reset since it was last taken. */
bool TakeChatStoreResetNotice(const std::string& profile_data_dir);

} // namespace pbr
