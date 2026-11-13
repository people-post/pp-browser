#include "lib/people/PeerCards.h"

#include "common/ValueJson.h"

#include <algorithm>
#include <map>
#include <utility>

namespace pbr {

Object PeerCardRecordToJson(const PeerCardRecord& record) {
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

Roe<PeerCardRecord> PeerCardRecordFromJson(const Object& json) {
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

std::vector<AccountCardRelay> KeptRelays(const PeerCardRecord& record) {
  return record.newest_card ? record.newest_card->relays : std::vector<AccountCardRelay>{};
}

std::optional<int64_t> KeptInitiationFloor(const PeerCardRecord& record) {
  if (!record.newest_card) {
    return std::nullopt;
  }
  return record.newest_card->initiation_floor;
}

Object CarriedCardJson(const PeerCardRecord& record) {
  return record.newest_card ? AccountCardToJson(*record.newest_card) : KeyCardToJson(record.keys);
}

PeerCards::PeerCards(IPeerCardStore& store) : store_(store) {
  redirectLogger("PeerCards");
}

Roe<PeerCardRecord> PeerCards::AddKeyCard(const KeyCard& card, const std::string& source) {
  if (auto checked = VerifyKeyCard(card); !checked) {
    return checked.error();
  }
  std::lock_guard lock(mutex_);
  auto known = store_.Get(card.account_id);
  if (!known) {
    return known.error();
  }
  PeerCardRecord merged = MergeKeyCard(std::move(known.value()), card, source);
  if (auto put = store_.Put(merged); !put) {
    return put.error();
  }
  return merged;
}

Roe<PeerCardRecord> PeerCards::AddAccountCard(const AccountCard& card, int64_t now_ms, const std::string& source) {
  if (auto checked = VerifyAccountCard(card, now_ms); !checked) {
    return checked.error();
  }
  std::function<void()> notify;
  PeerCardRecord merged;
  {
    std::lock_guard lock(mutex_);
    auto floor = store_.Floor(card.keys.account_id);
    if (!floor) {
      return floor.error();
    }
    if (card.issued_at < *floor) {
      return Error("account card: older than the newest this device knew for the account");
    }
    auto known = store_.Get(card.keys.account_id);
    if (!known) {
      return known.error();
    }
    const int64_t before = known.value() ? known.value()->issued_at : 0;
    merged = MergeAccountCard(std::move(known.value()), card, source);
    if (merged.issued_at > before) {
      notify = on_newer_card_;
    }
    if (auto put = store_.Put(merged); !put) {
      return put.error();
    }
  }
  if (notify) {
    notify();
  }
  return merged;
}

Roe<std::optional<PeerCardRecord>> PeerCards::Get(const std::string& account_id) const {
  std::lock_guard lock(mutex_);
  return store_.Get(account_id);
}

Roe<std::vector<PeerCardRecord>> PeerCards::List() const {
  std::lock_guard lock(mutex_);
  return store_.List();
}

Roe<void> PeerCards::RaiseFloors(const std::vector<PeerCardFloor>& floors) {
  std::vector<PeerCardFloor> kept;
  for (const PeerCardFloor& floor : floors) {
    if (!floor.account_id.empty() && floor.issued_at > 0) {
      kept.push_back(floor);
    }
  }
  std::lock_guard lock(mutex_);
  return store_.RaiseFloors(kept);
}

Roe<int64_t> PeerCards::Floor(const std::string& account_id) const {
  std::lock_guard lock(mutex_);
  return store_.Floor(account_id);
}

Roe<std::vector<PeerCardFloor>> PeerCards::NewestKnown() const {
  std::lock_guard lock(mutex_);
  auto records = store_.List();
  if (!records) {
    return records.error();
  }
  auto floors = store_.Floors();
  if (!floors) {
    return floors.error();
  }
  std::map<std::string, int64_t> newest;
  for (const PeerCardRecord& record : *records) {
    if (record.issued_at > 0) {
      newest[record.keys.account_id] = record.issued_at;
    }
  }
  for (const PeerCardFloor& floor : *floors) {
    int64_t& kept = newest[floor.account_id];
    kept = std::max(kept, floor.issued_at);
  }
  std::vector<PeerCardFloor> out;
  for (const auto& [account_id, issued_at] : newest) {
    if (!account_id.empty()) {
      out.push_back({account_id, issued_at});
    }
  }
  return out;
}

void PeerCards::SetOnNewerCard(std::function<void()> callback) {
  std::lock_guard lock(mutex_);
  on_newer_card_ = std::move(callback);
}

} // namespace pbr
