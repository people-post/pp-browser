#include "lib/messaging/PeerCardStore.h"

#include "common/ValueJson.h"

#include <sqlite3.h>

#include <algorithm>
#include <map>
#include <utility>

namespace pbr {

namespace {

Object RecordToJson(const PeerCardRecord& record) {
  Object json = KeyCardToJson(record.keys);
  json.set("nickname", record.nickname);
  json.set("issued_at", record.issued_at);
  std::vector<Value> devices;
  for (const std::string& peer_id : record.devices) {
    devices.emplace_back(peer_id);
  }
  json.set("devices", ArrayValue(std::move(devices)));
  json.set("source", record.source);
  if (record.newest_card) {
    json.set("newest_card", AccountCardToJson(*record.newest_card));
  }
  return json;
}

Roe<PeerCardRecord> RecordFromJson(const Object& json) {
  auto keys = KeyCardFromJson(json);
  if (!keys) {
    return keys.error();
  }
  PeerCardRecord record;
  record.keys = std::move(keys.value());
  record.nickname = json.getString("nickname").value_or("");
  record.issued_at = json.getIf<int64_t>("issued_at").value_or(0);
  if (const Array* devices = json.getArray("devices")) {
    for (const Value& item : devices->elements) {
      if (auto peer_id = asString(item)) {
        record.devices.push_back(*peer_id);
      }
    }
  }
  record.source = json.getString("source").value_or("");
  if (const Object* newest = json.getObject("newest_card")) {
    if (auto card = AccountCardFromJson(*newest)) {
      record.newest_card = std::move(card.value());
    }
  }
  return record;
}

} // namespace

PeerCardRecord MergeKeyCard(std::optional<PeerCardRecord> known, const KeyCard& card, const std::string& source) {
  if (!known) {
    return PeerCardRecord{.keys = card, .source = source};
  }
  if (known->issued_at == 0 && !SameKeys(known->keys, card)) {
    known->keys = card;
    known->source = source;
  }
  return *known;
}

PeerCardRecord MergeAccountCard(std::optional<PeerCardRecord> known, const AccountCard& card,
                                const std::string& source) {
  PeerCardRecord merged = known.value_or(PeerCardRecord{});
  if (known && card.issued_at < known->issued_at) {
    return merged;
  }
  merged.keys = card.keys;
  merged.nickname = card.nickname;
  merged.issued_at = card.issued_at;
  merged.devices = CardDevicePeerIds(card);
  merged.source = source;
  merged.newest_card = card;
  return merged;
}

Object CarriedCardJson(const PeerCardRecord& record) {
  return record.newest_card ? AccountCardToJson(*record.newest_card) : KeyCardToJson(record.keys);
}

PeerCardStore::PeerCardStore(std::string profile_db_path) : profile_db_path_(std::move(profile_db_path)) {
  redirectLogger("PeerCardStore");
}

Roe<sqlite3*> PeerCardStore::OpenDb() const {
  sqlite3* db = nullptr;
  if (sqlite3_open(profile_db_path_.c_str(), &db) != SQLITE_OK) {
    sqlite3_close(db);
    return Error("Failed to open profile.db for peer cards");
  }
  sqlite3_busy_timeout(db, 5000);
  if (!schema_ready_) {
    char* err = nullptr;
    if (sqlite3_exec(db,
                     "CREATE TABLE IF NOT EXISTS peer_cards (account_id TEXT PRIMARY KEY, record_json TEXT NOT NULL, "
                     "updated_at INTEGER NOT NULL);"
                     "CREATE TABLE IF NOT EXISTS peer_card_floors (account_id TEXT PRIMARY KEY, "
                     "issued_at INTEGER NOT NULL);",
                     nullptr, nullptr, &err) != SQLITE_OK) {
      const std::string message = err ? err : "peer_cards schema failed";
      sqlite3_free(err);
      sqlite3_close(db);
      return Error(message);
    }
    schema_ready_ = true;
  }
  return db;
}

Roe<std::optional<PeerCardRecord>> PeerCardStore::GetLocked(sqlite3* db, const std::string& account_id) const {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT record_json FROM peer_cards WHERE account_id = ?;", -1, &stmt, nullptr) !=
      SQLITE_OK) {
    return Error("Failed to prepare peer card read");
  }
  sqlite3_bind_text(stmt, 1, account_id.c_str(), -1, SQLITE_TRANSIENT);
  std::optional<PeerCardRecord> found;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    if (auto json = TryParseObject(text ? text : "")) {
      if (auto record = RecordFromJson(*json)) {
        found = std::move(record.value());
      }
    }
  }
  sqlite3_finalize(stmt);
  return found;
}

Roe<void> PeerCardStore::PutLocked(sqlite3* db, const PeerCardRecord& record) const {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db,
                         "INSERT INTO peer_cards (account_id, record_json, updated_at) VALUES (?, ?, strftime('%s','now')) "
                         "ON CONFLICT(account_id) DO UPDATE SET record_json=excluded.record_json, "
                         "updated_at=excluded.updated_at;",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    return Error("Failed to prepare peer card write");
  }
  const std::string json = DumpJson(RecordToJson(record));
  sqlite3_bind_text(stmt, 1, record.keys.account_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, json.c_str(), -1, SQLITE_TRANSIENT);
  const bool done = sqlite3_step(stmt) == SQLITE_DONE;
  sqlite3_finalize(stmt);
  if (!done) {
    return Error("Failed to write peer card");
  }
  return {};
}

Roe<PeerCardRecord> PeerCardStore::AddKeyCard(const KeyCard& card, const std::string& source) {
  if (auto checked = VerifyKeyCard(card); !checked) {
    return checked.error();
  }
  std::lock_guard lock(mutex_);
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  auto known = GetLocked(*db, card.account_id);
  if (!known) {
    sqlite3_close(*db);
    return known.error();
  }
  PeerCardRecord merged = MergeKeyCard(std::move(known.value()), card, source);
  auto put = PutLocked(*db, merged);
  sqlite3_close(*db);
  if (!put) {
    return put.error();
  }
  return merged;
}

Roe<PeerCardRecord> PeerCardStore::AddAccountCard(const AccountCard& card, int64_t now_ms, const std::string& source) {
  if (auto checked = VerifyAccountCard(card, now_ms); !checked) {
    return checked.error();
  }
  std::function<void()> notify;
  PeerCardRecord merged;
  {
    std::lock_guard lock(mutex_);
    auto db = OpenDb();
    if (!db) {
      return db.error();
    }
    auto floor = FloorLocked(*db, card.keys.account_id);
    if (!floor) {
      sqlite3_close(*db);
      return floor.error();
    }
    if (card.issued_at < *floor) {
      sqlite3_close(*db);
      return Error("account card: older than the newest this device knew for the account");
    }
    auto known = GetLocked(*db, card.keys.account_id);
    if (!known) {
      sqlite3_close(*db);
      return known.error();
    }
    const int64_t before = known.value() ? known.value()->issued_at : 0;
    merged = MergeAccountCard(std::move(known.value()), card, source);
    if (merged.issued_at > before) {
      notify = on_newer_card_;
    }
    auto put = PutLocked(*db, merged);
    sqlite3_close(*db);
    if (!put) {
      return put.error();
    }
  }
  if (notify) {
    notify();
  }
  return merged;
}

Roe<int64_t> PeerCardStore::FloorLocked(sqlite3* db, const std::string& account_id) const {
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT issued_at FROM peer_card_floors WHERE account_id = ?;", -1, &stmt, nullptr) !=
      SQLITE_OK) {
    return Error("Failed to prepare peer card floor read");
  }
  sqlite3_bind_text(stmt, 1, account_id.c_str(), -1, SQLITE_TRANSIENT);
  int64_t floor = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    floor = sqlite3_column_int64(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return floor;
}

Roe<void> PeerCardStore::RaiseFloors(const std::vector<PeerCardFloor>& floors) {
  std::lock_guard lock(mutex_);
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(*db,
                         "INSERT INTO peer_card_floors (account_id, issued_at) VALUES (?, ?) "
                         "ON CONFLICT(account_id) DO UPDATE SET issued_at=MAX(issued_at, excluded.issued_at);",
                         -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(*db);
    return Error("Failed to prepare peer card floor write");
  }
  bool ok = true;
  for (const PeerCardFloor& floor : floors) {
    if (floor.account_id.empty() || floor.issued_at <= 0) {
      continue;
    }
    sqlite3_reset(stmt);
    sqlite3_bind_text(stmt, 1, floor.account_id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, floor.issued_at);
    ok = ok && sqlite3_step(stmt) == SQLITE_DONE;
  }
  sqlite3_finalize(stmt);
  sqlite3_close(*db);
  if (!ok) {
    return Error("Failed to write peer card floor");
  }
  return {};
}

Roe<int64_t> PeerCardStore::Floor(const std::string& account_id) const {
  std::lock_guard lock(mutex_);
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  auto floor = FloorLocked(*db, account_id);
  sqlite3_close(*db);
  return floor;
}

Roe<std::vector<PeerCardFloor>> PeerCardStore::NewestKnown() const {
  auto records = List();
  if (!records) {
    return records.error();
  }
  std::map<std::string, int64_t> newest;
  for (const PeerCardRecord& record : *records) {
    if (record.issued_at > 0) {
      newest[record.keys.account_id] = record.issued_at;
    }
  }
  std::lock_guard lock(mutex_);
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(*db, "SELECT account_id, issued_at FROM peer_card_floors;", -1, &stmt, nullptr) !=
      SQLITE_OK) {
    sqlite3_close(*db);
    return Error("Failed to prepare peer card floor list");
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const auto* id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    int64_t& kept = newest[id ? id : ""];
    kept = std::max<int64_t>(kept, sqlite3_column_int64(stmt, 1));
  }
  sqlite3_finalize(stmt);
  sqlite3_close(*db);
  std::vector<PeerCardFloor> out;
  for (const auto& [account_id, issued_at] : newest) {
    if (!account_id.empty()) {
      out.push_back({account_id, issued_at});
    }
  }
  return out;
}

void PeerCardStore::SetOnNewerCard(std::function<void()> callback) {
  std::lock_guard lock(mutex_);
  on_newer_card_ = std::move(callback);
}

Roe<std::optional<PeerCardRecord>> PeerCardStore::Get(const std::string& account_id) const {
  std::lock_guard lock(mutex_);
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  auto found = GetLocked(*db, account_id);
  sqlite3_close(*db);
  return found;
}

Roe<std::vector<PeerCardRecord>> PeerCardStore::List() const {
  std::lock_guard lock(mutex_);
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(*db, "SELECT record_json FROM peer_cards;", -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(*db);
    return Error("Failed to prepare peer card list");
  }
  std::vector<PeerCardRecord> records;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    if (auto json = TryParseObject(text ? text : "")) {
      if (auto record = RecordFromJson(*json)) {
        records.push_back(std::move(record.value()));
      }
    }
  }
  sqlite3_finalize(stmt);
  sqlite3_close(*db);
  return records;
}

} // namespace pbr
