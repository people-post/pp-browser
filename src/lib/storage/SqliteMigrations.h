#pragma once

#include "common/Error.h"
#include "foundation/crypto/CryptoTypes.h"

#include <functional>
#include <string>
#include <vector>

#include "common/PbrCompat.h"

struct sqlite3;

namespace pbr {

/**
 * What a step works with: the database, and the profile's data key (the ladder runs after unlock, so a step may
 * rewrite encrypted columns; none does yet). `dek` is null only where no key is given (a store's own tests).
 */
struct SqliteMigrationContext {
  sqlite3* db = nullptr;
  const ByteVector* dek = nullptr;
};

/** One rung of a database's ladder: from `from` to `from + 1`. The runner wraps it in its own transaction. */
struct SqliteMigrationStep {
  int from = 0;
  std::function<Roe<void>(const SqliteMigrationContext& context)> apply;
};

/**
 * How one SQLite database's schema moves (DATA_LAYOUT § Schema versions): its `user_version` says which schema it
 * has. `current` is this build's; `floor` the oldest it still upgrades — the latest app carries every step from the
 * floor up, since a phone cannot install the versions between. `steps` holds one step for each version from `floor`
 * to `current - 1`, in order. `create` makes the current schema on an empty file.
 */
struct SqliteSchemaLadder {
  /** For messages: "profile.db", "thread.db". */
  std::string name;
  int floor = 0;
  int current = 0;
  std::function<Roe<void>(const SqliteMigrationContext& context)> create;
  std::vector<SqliteMigrationStep> steps;
};

enum class SqliteLadderOutcome {
  /** An empty file (version 0): the current schema made. */
  Created,
  /** Already at the current version: nothing done. */
  Current,
  /** Brought up from a version between the floor and the current one. */
  Migrated,
  /** Older than the floor: nothing done; the caller resets the store. */
  BelowFloor,
  /** Newer than this build's: nothing done, refused (a downgrade); the caller says so. */
  Newer,
};

Roe<int> SqliteUserVersion(sqlite3* db);

/**
 * Bring `db` (open on the file `db_path`) to `ladder.current`, with `dek` for the steps. A newer version than this
 * build's is left alone (Newer). To
 * migrate, the database is first copied beside itself (`db_path` + ".pre-migration", through SQLite's backup API, so
 * a write-ahead log is included); each step then runs in a transaction that sets `user_version` to its target at its
 * end. A failed step is rolled back and the copy restored, so the database stays at the version it had; the copy is
 * deleted either way. Not safe against another connection writing meanwhile: run it before the stores open theirs.
 */
Roe<SqliteLadderOutcome> RunSqliteLadder(sqlite3* db, const std::string& db_path, const SqliteSchemaLadder& ladder,
                                         const ByteVector* dek = nullptr);

/** For steps: whether `table` exists, and whether it has `column`. */
Roe<bool> SqliteTableExists(sqlite3* db, const std::string& table);
Roe<bool> SqliteColumnExists(sqlite3* db, const std::string& table, const std::string& column);
/** For steps: add `column` (`declaration` is its type and constraints) to `table` when the table exists without it. */
Roe<void> SqliteAddColumnIfMissing(sqlite3* db, const std::string& table, const std::string& column,
                                   const std::string& declaration);
/** For steps: drop `column` from `table` when the table has it. */
Roe<void> SqliteDropColumnIfPresent(sqlite3* db, const std::string& table, const std::string& column);

} // namespace pbr
