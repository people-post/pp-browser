#pragma once

#include "foundation/crypto/CryptoTypes.h"

#include "common/Error.h"
#include "common/Module.h"

#include <list>
#include <mutex>
#include <string>
#include <unordered_map>

#include "common/PbrCompat.h"

struct sqlite3;

namespace pbr {

/**
 * Profile + per-thread SQLite connections, DEK, and LRU cache for SqliteThreadStore (D044).
 * Lock order when both DBs are touched: profile_mutex() first, then thread cache (inside OpenThreadDb).
 */
class SqliteThreadDb : public Module {
public:
  explicit SqliteThreadDb(std::string data_dir);
  ~SqliteThreadDb() override;

  Roe<void> SetDek(ByteVector dek);
  void ClearDek();
  Roe<void> RequireDek() const;
  /** Copy under dek_mutex — for TranscriptCipher calls. */
  ByteVector CopyDek() const;

  const std::string& data_dir() const { return data_dir_; }
  const std::string& profile_id() const { return profile_id_; }
  std::string ProfileDbPath() const;

  Roe<void> EnsureInitialized() const;
  Roe<sqlite3*> OpenThreadDb(const std::string& thread_id) const;
  void CloseThreadDb(const std::string& thread_id) const;
  void Flush() const;

  /** D044 — acquire before touching profile.db (and before OpenThreadDb when both). */
  std::mutex& profile_mutex() const { return profile_mutex_; }
  /**
   * Open profile.db if needed. Safe without profile_mutex when only the initiator races
   * (EnsureInitialized); callers that mutate catalog/outbox must hold profile_mutex().
   */
  Roe<void> EnsureProfileOpen() const;
  /** Valid after EnsureProfileOpen / EnsureInitialized; hold profile_mutex when mutating. */
  sqlite3* profile_db() const { return profile_db_; }

private:
  struct ThreadDbHandle {
    sqlite3* db = nullptr;
  };

  Roe<void> WipeLegacyJsonIfPresent() const;
  Roe<void> OpenProfileDb() const;
  Roe<void> OpenProfileDbUnguarded() const;
  Roe<void> EnsureThreadDirectory(const std::string& thread_id) const;
  Roe<void> RepairOrphanThreadDirs() const;
  void TouchThreadLru(const std::string& thread_id) const;
  void EvictThreadDbsIfNeeded() const;

  std::string data_dir_;
  std::string profile_id_;
  ByteVector dek_;
  mutable std::mutex dek_mutex_;
  mutable std::mutex profile_mutex_;
  mutable sqlite3* profile_db_ = nullptr;
  mutable std::mutex thread_cache_mutex_;
  mutable std::unordered_map<std::string, ThreadDbHandle> thread_dbs_;
  mutable std::list<std::string> thread_lru_;
  mutable bool initialized_ = false;
};

} // namespace pbr
