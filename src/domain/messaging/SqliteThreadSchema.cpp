#include "domain/messaging/SqliteThreadSchema.h"

#include <sqlite3.h>

#include <string>

namespace pbr {

Roe<void> SqliteThreadExecSql(sqlite3* db, const char* sql) {
  char* err = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
    const std::string message = err ? err : "sqlite exec failed";
    sqlite3_free(err);
    return Error(message);
  }
  return {};
}

Roe<void> SqliteThreadApplyUserVersion(sqlite3* db, int version) {
  if (auto result = SqliteThreadExecSql(db, ("PRAGMA user_version = " + std::to_string(version) + ";").c_str());
      !result) {
    return result.error();
  }
  return {};
}

Roe<void> SqliteThreadConfigureDb(sqlite3* db) {
  if (auto result = SqliteThreadExecSql(db, "PRAGMA journal_mode=WAL;"); !result) {
    return result.error();
  }
  if (auto result = SqliteThreadExecSql(db, "PRAGMA foreign_keys=ON;"); !result) {
    return result.error();
  }
  // Sibling CallSessionStore / CallMediaKeyStore open short-lived connections to the same file.
  sqlite3_busy_timeout(db, 5000);
  return {};
}

} // namespace pbr
