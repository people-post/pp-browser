#include "lib/messaging/PeerCardStore.h"

#include "common/ValueJson.h"

#include <sqlite3.h>

#include <algorithm>
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
  const bool newest = !known || card.issued_at >= known->issued_at;
  std::vector<std::string> devices;
  const auto add = [&devices](const std::vector<std::string>& from) {
    for (const std::string& peer_id : from) {
      if (devices.size() < kMaxAccountCardDevices &&
          std::find(devices.begin(), devices.end(), peer_id) == devices.end()) {
        devices.push_back(peer_id);
      }
    }
  };
  if (newest) {
    add(card.devices);
    add(merged.devices);
    merged.keys = card.keys;
    merged.nickname = card.nickname;
    merged.issued_at = card.issued_at;
    merged.source = source;
    merged.newest_card = card;
  } else {
    add(merged.devices);
    add(card.devices);
  }
  merged.devices = std::move(devices);
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
                     "updated_at INTEGER NOT NULL);",
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
  std::lock_guard lock(mutex_);
  auto db = OpenDb();
  if (!db) {
    return db.error();
  }
  auto known = GetLocked(*db, card.keys.account_id);
  if (!known) {
    sqlite3_close(*db);
    return known.error();
  }
  PeerCardRecord merged = MergeAccountCard(std::move(known.value()), card, source);
  auto put = PutLocked(*db, merged);
  sqlite3_close(*db);
  if (!put) {
    return put.error();
  }
  return merged;
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
