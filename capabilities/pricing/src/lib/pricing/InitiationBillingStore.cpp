#include "lib/pricing/InitiationBillingStore.h"

#include "common/AtomicFileWrite.h"
#include "common/ValueJson.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include "common/PbrCompat.h"

namespace pbr {

InitiationBillingStore::InitiationBillingStore(std::string data_dir) : data_dir_(std::move(data_dir)) {
  redirectLogger("InitiationBillingStore");
}

std::string InitiationBillingStore::Path() const {
  return (std::filesystem::path(data_dir_) / "initiation_billing.json").string();
}

Roe<void> InitiationBillingStore::Load() {
  std::lock_guard lock(mutex_);
  rows_.clear();
  refused_.reset();
  auto loaded = LoadUnlocked();
  if (!loaded) {
    // The file is kept as it is: this run's changes live in memory only (a newer app's rows, or a file to look at).
    refused_ = loaded.error().message;
    log().warning << "initiation_billing.json not read, so not written: " << *refused_;
  }
  return loaded;
}

Roe<void> InitiationBillingStore::LoadUnlocked() {
  const std::string path = Path();
  if (!std::filesystem::exists(path)) {
    return {};
  }
  std::ifstream in(path);
  if (!in) {
    return Error("Failed to open initiation_billing.json");
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  auto root = TryParseObject(buffer.str());
  if (!root) {
    return Error("Invalid initiation_billing.json");
  }
  const std::optional<uint64_t> version = root->getNonNegInt("schema_version");
  if (!version || *version < kFloorSchemaVersion || *version > kSchemaVersion) {
    return Error("Unsupported schema version " + (version ? std::to_string(*version) : std::string("(none)")) +
                 " in initiation_billing.json");
  }
  const Object* peers = root->getObject("peers");
  if (!peers) {
    return Error("initiation_billing.json holds no peers");
  }
  for (const auto& [key, value] : peers->fields()) {
    const Object* row_obj = asObject(value);
    if (!row_obj) {
      continue;
    }
    InitiationPeerBilling row;
    row.state = InitiationBillingStateFromString(row_obj->getString("state").value_or("closed"));
    row.floor_minor = row_obj->getIf<int64_t>("floor_minor").value_or(0);
    row.offer_minor = row_obj->getIf<int64_t>("offer_minor").value_or(0);
    row.currency = row_obj->getString("currency").value_or(std::string(kPricingCurrencyId));
    row.relay_floor_minor = row_obj->getIf<int64_t>("relay_floor_minor"); // additive: none before a relay said one
    row.relay_floor_card_issued_at = row_obj->getIf<int64_t>("relay_floor_card_issued_at").value_or(0);
    rows_[key] = std::move(row);
  }
  return {};
}

Roe<void> InitiationBillingStore::SaveUnlocked() const {
  if (refused_) {
    return Error("initiation_billing.json was not read (" + *refused_ + "): not written over");
  }
  Object peers;
  for (const auto& [peer, row] : rows_) {
    Object entry;
    entry.set("state", InitiationBillingStateToString(row.state));
    entry.set("floor_minor", row.floor_minor);
    entry.set("offer_minor", row.offer_minor);
    entry.set("currency", row.currency);
    if (row.relay_floor_minor) {
      entry.set("relay_floor_minor", *row.relay_floor_minor);
      entry.set("relay_floor_card_issued_at", row.relay_floor_card_issued_at);
    }
    peers.set(peer, entry);
  }
  Object root;
  root.setJsonUInt("schema_version", kSchemaVersion);
  root.set("peers", peers);
  return AtomicFileWrite::Write(Path(), DumpJson(root, 2));
}

Roe<void> InitiationBillingStore::Save() const {
  std::lock_guard lock(mutex_);
  return SaveUnlocked();
}

void InitiationBillingStore::SetCardFloors(CardFloors card_floors) {
  std::lock_guard lock(mutex_);
  card_floors_ = std::move(card_floors);
}

std::optional<InitiationCardFloor> InitiationBillingStore::CardFloorOf(const std::string& peer_identity) const {
  CardFloors card_floors;
  {
    std::lock_guard lock(mutex_);
    card_floors = card_floors_;
  }
  return card_floors && !peer_identity.empty() ? card_floors(peer_identity) : std::nullopt; // outside the lock
}

InitiationPeerBilling InitiationBillingStore::Get(const std::string& peer_identity) const {
  const std::optional<InitiationCardFloor> card = CardFloorOf(peer_identity);
  InitiationPeerBilling row;
  {
    std::lock_guard lock(mutex_);
    if (const auto it = rows_.find(peer_identity); it != rows_.end()) {
      row = it->second;
    }
  }
  row.floor_minor = PeerInitiationFloor(row, card);
  return row;
}

Roe<void> InitiationBillingStore::Set(const std::string& peer_identity, InitiationPeerBilling row) {
  std::lock_guard lock(mutex_);
  rows_[peer_identity] = std::move(row);
  return SaveUnlocked();
}

Roe<void> InitiationBillingStore::SetFloor(const std::string& peer_identity, int64_t floor_minor) {
  std::lock_guard lock(mutex_);
  InitiationPeerBilling& row = rows_[peer_identity];
  row.floor_minor = floor_minor;
  if (row.currency.empty()) {
    row.currency = kPricingCurrencyId;
  }
  return SaveUnlocked();
}

Roe<bool> InitiationBillingStore::SetRelayFloor(const std::string& peer_identity, const int64_t floor_minor) {
  const std::optional<InitiationCardFloor> card = CardFloorOf(peer_identity);
  std::lock_guard lock(mutex_);
  InitiationPeerBilling& row = rows_[peer_identity];
  row.relay_floor_minor = floor_minor;
  row.relay_floor_card_issued_at = card ? card->issued_at : 0;
  if (auto saved = SaveUnlocked(); !saved) {
    return saved.error();
  }
  return !card || card->floor_minor != floor_minor;
}

Roe<void> InitiationBillingStore::MarkOffered(const std::string& peer_identity, int64_t offer_minor,
                                              int64_t floor_minor) {
  std::lock_guard lock(mutex_);
  InitiationPeerBilling& row = rows_[peer_identity];
  row.state = InitiationBillingState::Offered;
  row.offer_minor = offer_minor;
  row.floor_minor = floor_minor;
  row.currency = kPricingCurrencyId;
  return SaveUnlocked();
}

Roe<void> InitiationBillingStore::MarkOpen(const std::string& peer_identity) {
  std::lock_guard lock(mutex_);
  InitiationPeerBilling& row = rows_[peer_identity];
  row.state = InitiationBillingState::Open;
  return SaveUnlocked();
}

Roe<void> InitiationBillingStore::MarkClosed(const std::string& peer_identity) {
  std::lock_guard lock(mutex_);
  InitiationPeerBilling& row = rows_[peer_identity];
  row.state = InitiationBillingState::Closed;
  row.offer_minor = 0;
  return SaveUnlocked();
}

int64_t PeerInitiationFloor(const InitiationPeerBilling& row, const std::optional<InitiationCardFloor>& card) {
  if (row.relay_floor_minor && (!card || card->issued_at <= row.relay_floor_card_issued_at)) {
    return *row.relay_floor_minor;
  }
  return card ? card->floor_minor : row.floor_minor;
}

bool InitiationBillingStore::IsOpen(const std::string& peer_identity) const {
  std::lock_guard lock(mutex_);
  const auto it = rows_.find(peer_identity);
  if (it == rows_.end()) {
    return false;
  }
  return it->second.state == InitiationBillingState::Open;
}

} // namespace pbr
