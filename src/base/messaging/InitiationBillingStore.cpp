#include "base/messaging/InitiationBillingStore.h"

#include "base/data/AtomicFileWrite.h"
#include "common/ValueJson.h"

#include <filesystem>
#include <fstream>
#include <sstream>

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
  const Object* peers = &*root;
  if (const Object* nested = root->getObject("peers")) {
    peers = nested;
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
    rows_[key] = std::move(row);
  }
  return {};
}

Roe<void> InitiationBillingStore::SaveUnlocked() const {
  Object peers;
  for (const auto& [peer, row] : rows_) {
    Object entry;
    entry.set("state", InitiationBillingStateToString(row.state));
    entry.set("floor_minor", row.floor_minor);
    entry.set("offer_minor", row.offer_minor);
    entry.set("currency", row.currency);
    peers.set(peer, entry);
  }
  Object root;
  root.setJsonUInt("schema_version", 1);
  root.set("peers", peers);
  return AtomicFileWrite::Write(Path(), DumpJson(root, 2));
}

Roe<void> InitiationBillingStore::Save() const {
  std::lock_guard lock(mutex_);
  return SaveUnlocked();
}

InitiationPeerBilling InitiationBillingStore::Get(const std::string& peer_identity) const {
  std::lock_guard lock(mutex_);
  const auto it = rows_.find(peer_identity);
  if (it == rows_.end()) {
    return {};
  }
  return it->second;
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

bool InitiationBillingStore::IsOpen(const std::string& peer_identity) const {
  std::lock_guard lock(mutex_);
  const auto it = rows_.find(peer_identity);
  if (it == rows_.end()) {
    return false;
  }
  return it->second.state == InitiationBillingState::Open;
}

} // namespace pbr
