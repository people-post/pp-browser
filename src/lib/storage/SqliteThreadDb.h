#pragma once

#include "foundation/crypto/CryptoTypes.h"

#include "common/Error.h"
#include "lib/storage/SqliteChatStoreGate.h"
#include "common/Module.h"

#include <list>
#include <mutex>
#include <optional>
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

  /**
   * Open profile.db (its ladder run with the data key) if not yet: refused before SetDek (the chat store opens only
   * after unlock, D105), and after a failed open (newer, or not upgraded) until the store is reset.
   */
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

  Roe<void> OpenProfileDb() const;
  /** Keeps the chat store shut after a failed open (until it is reset): logged, the gate told, every call refused. */
  Error RecordOpenFailure(ChatStoreProblem problem, std::string message) const;
  Roe<void> OpenProfileDbUnguarded() const;
  Roe<void> EnsureThreadDirectory(const std::string& thread_id) const;
  /** A chat's thread.db, opened and brought through its ladder (a file below the floor is reset first). */
  Roe<sqlite3*> OpenThreadDbFile(const std::string& thread_id) const;
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
  mutable std::mutex init_mutex_;
  mutable std::optional<Error> open_failure_;
};

} // namespace pbr
