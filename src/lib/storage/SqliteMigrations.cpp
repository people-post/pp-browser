#include "lib/storage/SqliteMigrations.h"

#include <sqlite3.h>

#include <filesystem>
#include <string>

namespace pbr {

namespace {

Roe<void> Exec(sqlite3* db, const std::string& sql) {
  char* err = nullptr;
  if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
    const std::string message = err ? err : "sqlite exec failed";
    sqlite3_free(err);
    return Error(message);
  }
  return {};
}

/** Every page of `from` into `to` (SQLite's online backup): consistent with a write-ahead log, either way round. */
Roe<void> CopyDatabase(sqlite3* from, sqlite3* to) {
  sqlite3_backup* backup = sqlite3_backup_init(to, "main", from, "main");
  if (!backup) {
    return Error(std::string("backup init failed: ") + sqlite3_errmsg(to));
  }
  const int stepped = sqlite3_backup_step(backup, -1);
  sqlite3_backup_finish(backup);
  if (stepped != SQLITE_DONE) {
    return Error(std::string("backup failed: ") + sqlite3_errstr(stepped));
  }
  return {};
}

void RemoveCopy(const std::string& copy_path) {
  std::error_code ec;
  for (const char* suffix : {"", "-wal", "-shm", "-journal"}) {
    std::filesystem::remove(copy_path + suffix, ec);
  }
}

/** `db`'s pages into a fresh file at `copy_path`. */
Roe<void> WriteCopy(sqlite3* db, const std::string& copy_path) {
  RemoveCopy(copy_path); // a copy left by a crash mid-migration: this one supersedes it
  sqlite3* copy = nullptr;
  if (sqlite3_open(copy_path.c_str(), &copy) != SQLITE_OK) {
    if (copy) {
      sqlite3_close(copy);
    }
    return Error("cannot create " + copy_path);
  }
  auto copied = CopyDatabase(db, copy);
  sqlite3_close(copy);
  if (!copied) {
    RemoveCopy(copy_path);
  }
  return copied;
}

Roe<void> RestoreCopy(sqlite3* db, const std::string& copy_path) {
  sqlite3* copy = nullptr;
  if (sqlite3_open_v2(copy_path.c_str(), &copy, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
    if (copy) {
      sqlite3_close(copy);
    }
    return Error("cannot open " + copy_path);
  }
  auto restored = CopyDatabase(copy, db);
  sqlite3_close(copy);
  return restored;
}

/** The ladder's steps cover floor .. current - 1, one each, in order. */
Roe<void> CheckLadder(const SqliteSchemaLadder& ladder) {
  if (ladder.floor < 1 || ladder.floor > ladder.current || !ladder.create) {
    return Error(ladder.name + ": bad schema ladder");
  }
  if (static_cast<int>(ladder.steps.size()) != ladder.current - ladder.floor) {
    return Error(ladder.name + ": schema ladder lacks a step");
  }
  for (size_t i = 0; i < ladder.steps.size(); ++i) {
    if (ladder.steps[i].from != ladder.floor + static_cast<int>(i) || !ladder.steps[i].apply) {
      return Error(ladder.name + ": schema ladder steps out of order");
    }
  }
  return {};
}

/** One step, in its own transaction ending at its target version. Rolled back on failure. */
Roe<void> RunStep(const SqliteMigrationContext& context, const SqliteMigrationStep& step) {
  sqlite3* db = context.db;
  if (auto begun = Exec(db, "BEGIN IMMEDIATE;"); !begun) {
    return begun.error();
  }
  auto applied = step.apply(context);
  if (applied) {
    applied = Exec(db, "PRAGMA user_version = " + std::to_string(step.from + 1) + ";");
  }
  if (applied) {
    applied = Exec(db, "COMMIT;");
  }
  if (!applied) {
    (void)Exec(db, "ROLLBACK;");
  }
  return applied;
}

Roe<void> Create(const SqliteMigrationContext& context, const SqliteSchemaLadder& ladder) {
  sqlite3* db = context.db;
  if (auto begun = Exec(db, "BEGIN IMMEDIATE;"); !begun) {
    return begun.error();
  }
  auto created = ladder.create(context);
  if (created) {
    created = Exec(db, "PRAGMA user_version = " + std::to_string(ladder.current) + ";");
  }
  if (created) {
    created = Exec(db, "COMMIT;");
  }
  if (!created) {
    (void)Exec(db, "ROLLBACK;");
    return Error(ladder.name + ": schema not created: " + created.error().message);
  }
  return {};
}

Roe<void> Migrate(const SqliteMigrationContext& context, const std::string& db_path, const SqliteSchemaLadder& ladder,
                  const int from) {
  sqlite3* db = context.db;
  const std::string copy_path = db_path + ".pre-migration";
  if (auto copied = WriteCopy(db, copy_path); !copied) {
    return Error(ladder.name + ": not migrated (no backup copy): " + copied.error().message);
  }
  for (int version = from; version < ladder.current; ++version) {
    const SqliteMigrationStep& step = ladder.steps[static_cast<size_t>(version - ladder.floor)];
    if (auto stepped = RunStep(context, step); !stepped) {
      std::string message = ladder.name + ": migration from schema " + std::to_string(version) + " failed (" +
                            stepped.error().message + ")";
      if (auto restored = RestoreCopy(db, copy_path); !restored) {
        // Keep the copy: it is the only whole one left.
        return Error(message + "; restoring the copy failed (" + restored.error().message + "), it is kept at " +
                     copy_path);
      }
      RemoveCopy(copy_path);
      return Error(message + "; left at schema " + std::to_string(from));
    }
  }
  RemoveCopy(copy_path);
  return {};
}

} // namespace

Roe<int> SqliteUserVersion(sqlite3* db) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &stmt, nullptr) != SQLITE_OK) {
    return Error(std::string("user_version not read: ") + sqlite3_errmsg(db));
  }
  int version = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    version = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return version;
}

Roe<SqliteLadderOutcome> RunSqliteLadder(sqlite3* db, const std::string& db_path, const SqliteSchemaLadder& ladder,
                                         const ByteVector* dek, std::string thread_peer_account) {
  const SqliteMigrationContext context{db, dek, std::move(thread_peer_account)};
  if (auto checked = CheckLadder(ladder); !checked) {
    return checked.error();
  }
  auto version = SqliteUserVersion(db);
  if (!version) {
    return version.error();
  }
  if (*version == ladder.current) {
    return SqliteLadderOutcome::Current;
  }
  if (*version == 0) {
    if (auto created = Create(context, ladder); !created) {
      return created.error();
    }
    return SqliteLadderOutcome::Created;
  }
  if (*version > ladder.current) {
    return SqliteLadderOutcome::Newer;
  }
  if (*version < ladder.floor) {
    return SqliteLadderOutcome::BelowFloor;
  }
  if (auto migrated = Migrate(context, db_path, ladder, *version); !migrated) {
    return migrated.error();
  }
  return SqliteLadderOutcome::Migrated;
}

Roe<bool> SqliteTableExists(sqlite3* db, const std::string& table) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?;", -1, &stmt, nullptr) !=
      SQLITE_OK) {
    return Error(std::string("table lookup failed: ") + sqlite3_errmsg(db));
  }
  sqlite3_bind_text(stmt, 1, table.c_str(), -1, SQLITE_TRANSIENT);
  const bool exists = sqlite3_step(stmt) == SQLITE_ROW;
  sqlite3_finalize(stmt);
  return exists;
}

Roe<bool> SqliteColumnExists(sqlite3* db, const std::string& table, const std::string& column) {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT 1 FROM pragma_table_info(?) WHERE name = ?;", -1, &stmt, nullptr) != SQLITE_OK) {
    return Error(std::string("column lookup failed: ") + sqlite3_errmsg(db));
  }
  sqlite3_bind_text(stmt, 1, table.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, column.c_str(), -1, SQLITE_TRANSIENT);
  const bool exists = sqlite3_step(stmt) == SQLITE_ROW;
  sqlite3_finalize(stmt);
  return exists;
}

Roe<void> SqliteAddColumnIfMissing(sqlite3* db, const std::string& table, const std::string& column,
                                   const std::string& declaration) {
  auto table_exists = SqliteTableExists(db, table);
  if (!table_exists) {
    return table_exists.error();
  }
  if (!*table_exists) {
    return {}; // its store makes it, at the current shape, when it first opens
  }
  auto column_exists = SqliteColumnExists(db, table, column);
  if (!column_exists) {
    return column_exists.error();
  }
  if (*column_exists) {
    return {};
  }
  return Exec(db, "ALTER TABLE " + table + " ADD COLUMN " + column + " " + declaration + ";");
}

Roe<void> SqliteDropColumnIfPresent(sqlite3* db, const std::string& table, const std::string& column) {
  auto column_exists = SqliteColumnExists(db, table, column);
  if (!column_exists) {
    return column_exists.error();
  }
  if (!*column_exists) {
    return {};
  }
  return Exec(db, "ALTER TABLE " + table + " DROP COLUMN " + column + ";");
}

} // namespace pbr
