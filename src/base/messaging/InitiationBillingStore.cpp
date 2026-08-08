#include "base/messaging/InitiationBillingStore.h"

#include "base/data/AtomicFileWrite.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

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
  nlohmann::json root = nlohmann::json::parse(in, nullptr, false);
  if (root.is_discarded() || !root.is_object()) {
    return Error("Invalid initiation_billing.json");
  }
  const nlohmann::json* peers = &root;
  if (root.contains("peers") && root["peers"].is_object()) {
    peers = &root["peers"];
  }
  for (auto it = peers->begin(); it != peers->end(); ++it) {
    if (!it.value().is_object()) {
      continue;
    }
    InitiationPeerBilling row;
    row.state = InitiationBillingStateFromString(it.value().value("state", "closed"));
    row.floor_minor = it.value().value("floor_minor", static_cast<int64_t>(0));
    row.offer_minor = it.value().value("offer_minor", static_cast<int64_t>(0));
    row.currency = it.value().value("currency", std::string(kPricingCurrencyId));
    rows_[it.key()] = std::move(row);
  }
  return {};
}

Roe<void> InitiationBillingStore::SaveUnlocked() const {
  nlohmann::json peers = nlohmann::json::object();
  for (const auto& [peer, row] : rows_) {
    peers[peer] = {{"state", InitiationBillingStateToString(row.state)},
                   {"floor_minor", row.floor_minor},
                   {"offer_minor", row.offer_minor},
                   {"currency", row.currency}};
  }
  const nlohmann::json root = {{"schema_version", 1}, {"peers", std::move(peers)}};
  return AtomicFileWrite::Write(Path(), root.dump(2));
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
