#include "lib/net/RelayPinStore.h"

#include "lib/net/RelayOrigins.h"

#include "common/AtomicFileWrite.h"
#include "common/BoundedRead.h"
#include "common/Logger.h"
#include "common/PlatformLimits.h"
#include "common/ValueJson.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <utility>

namespace pbr {

namespace {

constexpr int64_t kSchemaVersion = 1;

logging::Logger& PinLog() {
  static logging::Logger log = logging::getLogger("RelayPins");
  return log;
}

std::filesystem::path PinsPath(const std::string& profile_data_dir) {
  return std::filesystem::path(profile_data_dir) / "relay_pins.json";
}

} // namespace

std::string RelayPinsToJson(const std::vector<RelayPin>& pins) {
  Object root;
  root.set("schema_version", kSchemaVersion);
  std::vector<Value> rows;
  for (const RelayPin& pin : pins) {
    Object row;
    row.set("origin", pin.origin);
    row.set("peer_id", pin.peer_id);
    row.set("pinned_at_ms", pin.pinned_at_ms);
    if (pin.Changed()) {
      row.set("presented_peer_id", pin.presented_peer_id);
      row.set("presented_at_ms", pin.presented_at_ms);
    }
    rows.push_back(ObjectValue(std::move(row)));
  }
  root.set("pins", makeArray(std::move(rows)));
  return DumpJson(root);
}

std::optional<std::vector<RelayPin>> RelayPinsFromJson(const std::string& text) {
  const auto root = TryParseObject(text);
  if (!root || root->getIf<int64_t>("schema_version").value_or(0) != kSchemaVersion) {
    return std::nullopt;
  }
  const Array* rows = root->getArray("pins");
  if (!rows) {
    return std::nullopt;
  }
  std::vector<RelayPin> pins;
  for (const Value& item : rows->elements) {
    const Object* row = asObject(item);
    if (!row) {
      continue;
    }
    RelayPin pin{.origin = RelayOriginKey(row->getString("origin").value_or("")),
                 .peer_id = row->getString("peer_id").value_or(""),
                 .pinned_at_ms = row->getIf<int64_t>("pinned_at_ms").value_or(0),
                 .presented_peer_id = row->getString("presented_peer_id").value_or(""),
                 .presented_at_ms = row->getIf<int64_t>("presented_at_ms").value_or(0)};
    // The rules hold on disk too: a second pin of one origin or of one PeerId is dropped.
    const bool clash = std::any_of(pins.begin(), pins.end(), [&pin](const RelayPin& kept) {
      return kept.origin == pin.origin || kept.peer_id == pin.peer_id;
    });
    if (pin.origin.empty() || pin.peer_id.empty() || clash || pins.size() >= RelayPinStore::kMaxPins) {
      continue;
    }
    pins.push_back(std::move(pin));
  }
  return pins;
}

RelayPinStore::RelayPinStore(std::string profile_data_dir) : profile_data_dir_(std::move(profile_data_dir)) {}

void RelayPinStore::Load() {
  std::lock_guard lock(mutex_);
  pins_.clear();
  if (profile_data_dir_.empty()) {
    return;
  }
  std::ifstream in(PinsPath(profile_data_dir_));
  if (!in) {
    return; // none yet: every relay is pinned on first use
  }
  const auto text = ReadBounded(in, kMaxProfileJsonFileBytes);
  auto pins = text ? RelayPinsFromJson(*text) : std::nullopt;
  if (!pins) {
    PinLog().error << "relay_pins.json refused (another schema_version, or not JSON): the relays are pinned again";
    return;
  }
  pins_ = std::move(*pins);
}

void RelayPinStore::SetKeep(std::function<bool(const std::string& origin)> keep) {
  std::lock_guard lock(mutex_);
  keep_ = std::move(keep);
}

std::vector<RelayPin>::iterator RelayPinStore::FindLocked(const std::string& origin) {
  const std::string key = RelayOriginKey(origin);
  return std::find_if(pins_.begin(), pins_.end(), [&key](const RelayPin& pin) { return pin.origin == key; });
}

std::vector<RelayPin>::const_iterator RelayPinStore::FindLocked(const std::string& origin) const {
  const std::string key = RelayOriginKey(origin);
  return std::find_if(pins_.begin(), pins_.end(), [&key](const RelayPin& pin) { return pin.origin == key; });
}

std::vector<RelayPin>::const_iterator RelayPinStore::HolderLocked(const std::string& peer_id) const {
  return std::find_if(pins_.begin(), pins_.end(), [&peer_id](const RelayPin& pin) { return pin.peer_id == peer_id; });
}

std::optional<RelayPin> RelayPinStore::PinFor(const std::string& origin) const {
  std::lock_guard lock(mutex_);
  const auto found = FindLocked(origin);
  return found == pins_.end() ? std::nullopt : std::optional<RelayPin>(*found);
}

std::optional<std::string> RelayPinStore::HolderOf(const std::string& peer_id) const {
  std::lock_guard lock(mutex_);
  const auto found = HolderLocked(peer_id);
  return found == pins_.end() ? std::nullopt : std::optional<std::string>(found->origin);
}

std::vector<RelayPin> RelayPinStore::All() const {
  std::lock_guard lock(mutex_);
  return pins_;
}

void RelayPinStore::AddLocked(RelayPin pin) {
  if (pins_.size() >= kMaxPins) {
    // The oldest pin of a relay no one keeps (removed from our list since): pinned again by fetch if it comes back.
    auto oldest = pins_.end();
    for (auto it = pins_.begin(); it != pins_.end(); ++it) {
      if ((!keep_ || !keep_(it->origin)) && (oldest == pins_.end() || it->pinned_at_ms < oldest->pinned_at_ms)) {
        oldest = it;
      }
    }
    if (oldest == pins_.end()) {
      PinLog().warning << "relay pins full of kept relays; " << pin.origin << " not pinned";
      return;
    }
    pins_.erase(oldest);
  }
  pins_.push_back(std::move(pin));
}

PinAnswer RelayPinStore::Present(const std::string& origin, const std::string& peer_id, const int64_t now_ms) {
  std::lock_guard lock(mutex_);
  const std::string key = RelayOriginKey(origin);
  if (const auto holder = HolderLocked(peer_id); holder != pins_.end() && holder->origin != key) {
    return PinAnswer{.outcome = PinOutcome::Taken, .holder = holder->origin};
  }
  const auto found = FindLocked(key);
  if (found == pins_.end()) {
    AddLocked(RelayPin{.origin = key, .peer_id = peer_id, .pinned_at_ms = now_ms});
    SaveLocked();
    return PinAnswer{.outcome = PinOutcome::Pinned};
  }
  if (found->peer_id == peer_id) {
    if (found->Changed()) {
      // It presents its pinned identity again (the change was undone at the relay): in force once more.
      found->presented_peer_id.clear();
      found->presented_at_ms = 0;
      SaveLocked();
    }
    return PinAnswer{.outcome = PinOutcome::Same};
  }
  if (found->presented_peer_id != peer_id) {
    found->presented_peer_id = peer_id;
    found->presented_at_ms = now_ms;
    SaveLocked();
  }
  return PinAnswer{.outcome = PinOutcome::Changed};
}

PinAnswer RelayPinStore::Repin(const std::string& origin, const std::string& peer_id, const int64_t now_ms) {
  std::lock_guard lock(mutex_);
  const std::string key = RelayOriginKey(origin);
  if (const auto holder = HolderLocked(peer_id); holder != pins_.end() && holder->origin != key) {
    return PinAnswer{.outcome = PinOutcome::Taken, .holder = holder->origin};
  }
  const auto found = FindLocked(key);
  if (found == pins_.end()) {
    AddLocked(RelayPin{.origin = key, .peer_id = peer_id, .pinned_at_ms = now_ms});
  } else {
    *found = RelayPin{.origin = key, .peer_id = peer_id, .pinned_at_ms = now_ms};
  }
  SaveLocked();
  return PinAnswer{.outcome = PinOutcome::Pinned};
}

bool RelayPinStore::TakeOwnCard(const std::vector<OwnCardRelay>& relays, const int64_t now_ms) {
  std::lock_guard lock(mutex_);
  bool changed = false;
  for (const OwnCardRelay& relay : relays) {
    const std::string key = RelayOriginKey(relay.origin);
    if (key.empty() || relay.peer_id.empty()) {
      continue;
    }
    const auto found = FindLocked(key);
    if (found != pins_.end() && found->peer_id == relay.peer_id && !found->Changed()) {
      continue;
    }
    // Our own account's word: another origin's pin of this PeerId goes, and this origin's pin follows the card.
    std::erase_if(pins_, [&](const RelayPin& pin) { return pin.origin == key || pin.peer_id == relay.peer_id; });
    AddLocked(RelayPin{.origin = key, .peer_id = relay.peer_id, .pinned_at_ms = now_ms});
    changed = true;
  }
  if (changed) {
    SaveLocked();
  }
  return changed;
}

void RelayPinStore::Drop(const std::string& origin) {
  std::lock_guard lock(mutex_);
  const auto found = FindLocked(origin);
  if (found == pins_.end()) {
    return;
  }
  pins_.erase(found);
  SaveLocked();
}

void RelayPinStore::SaveLocked() const {
  if (profile_data_dir_.empty()) {
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(profile_data_dir_, ec);
  if (auto written = AtomicFileWrite::Write(PinsPath(profile_data_dir_).string(), RelayPinsToJson(pins_)); !written) {
    PinLog().error << "relay_pins.json not written: " << written.error().message;
  }
}

} // namespace pbr
