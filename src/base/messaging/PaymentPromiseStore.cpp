#include "base/messaging/PaymentPromiseStore.h"

#include "base/data/AtomicFileWrite.h"
#include "base/messaging/PaymentPromiseCodec.h"
#include "common/ValueJson.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

PaymentPromiseStore::PaymentPromiseStore(std::string data_dir) : data_dir_(std::move(data_dir)) {
  redirectLogger("PaymentPromiseStore");
}

std::string PaymentPromiseStore::Path() const {
  return (std::filesystem::path(data_dir_) / "payment_promises.json").string();
}

Roe<void> PaymentPromiseStore::Load() {
  std::lock_guard lock(mutex_);
  rows_.clear();
  const std::string path = Path();
  if (!std::filesystem::exists(path)) {
    return {};
  }
  std::ifstream in(path);
  if (!in) {
    return Error("Failed to open payment_promises.json");
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  auto root = TryParseObject(buffer.str());
  if (!root) {
    return Error("Invalid payment_promises.json");
  }
  const Array* promises = root->getArray("promises");
  if (!promises) {
    return Error("payment_promises.json missing promises array");
  }
  for (const Value& entry : promises->elements) {
    const Object* obj = asObject(entry);
    if (!obj) {
      continue;
    }
    auto decoded = PaymentPromiseCodec::Decode(DumpJson(*obj));
    if (!decoded) {
      return decoded.error();
    }
    rows_[decoded->promise_id] = std::move(*decoded);
  }
  return {};
}

Roe<void> PaymentPromiseStore::SaveUnlocked() const {
  std::vector<Value> promise_values;
  promise_values.reserve(rows_.size());
  for (const auto& [id, row] : rows_) {
    (void)id;
    auto encoded = PaymentPromiseCodec::Encode(row);
    if (!encoded) {
      return encoded.error();
    }
    auto obj = TryParseObject(*encoded);
    if (!obj) {
      return Error("Failed to re-parse encoded payment promise");
    }
    promise_values.push_back(ObjectValue(std::move(*obj)));
  }
  Object root;
  root.setJsonUInt("schema_version", 1);
  root.set("promises", ArrayValue(std::move(promise_values)));
  return AtomicFileWrite::Write(Path(), DumpJson(root, 2));
}

Roe<void> PaymentPromiseStore::Save() const {
  std::lock_guard lock(mutex_);
  return SaveUnlocked();
}

Roe<std::optional<PaymentPromise>> PaymentPromiseStore::Get(const std::string& promise_id) const {
  std::lock_guard lock(mutex_);
  const auto it = rows_.find(promise_id);
  if (it == rows_.end()) {
    return std::optional<PaymentPromise>{};
  }
  return std::optional<PaymentPromise>{it->second};
}

Roe<std::vector<PaymentPromise>> PaymentPromiseStore::List() const {
  std::lock_guard lock(mutex_);
  std::vector<PaymentPromise> out;
  out.reserve(rows_.size());
  for (const auto& [id, row] : rows_) {
    (void)id;
    out.push_back(row);
  }
  return out;
}

Roe<std::vector<PaymentPromise>> PaymentPromiseStore::ListForAccount(const std::string& account_id) const {
  std::lock_guard lock(mutex_);
  std::vector<PaymentPromise> out;
  for (const auto& [id, row] : rows_) {
    (void)id;
    if (row.payer_account_id == account_id || row.payee_account_id == account_id) {
      out.push_back(row);
    }
  }
  return out;
}

Roe<void> PaymentPromiseStore::Upsert(PaymentPromise promise) {
  if (promise.promise_id.empty()) {
    return Error("promise_id required");
  }
  std::lock_guard lock(mutex_);
  rows_[promise.promise_id] = std::move(promise);
  return SaveUnlocked();
}

Roe<bool> PaymentPromiseStore::Remove(const std::string& promise_id) {
  std::lock_guard lock(mutex_);
  const auto erased = rows_.erase(promise_id);
  if (erased == 0) {
    return false;
  }
  auto saved = SaveUnlocked();
  if (!saved) {
    return saved.error();
  }
  return true;
}

Roe<void> PaymentPromiseStore::MarkLocalAvoid(const std::string& promise_id, const bool avoided) {
  std::lock_guard lock(mutex_);
  const auto it = rows_.find(promise_id);
  if (it == rows_.end()) {
    return Error("payment promise not found");
  }
  it->second.local_avoid = avoided;
  return SaveUnlocked();
}

bool PaymentPromiseStore::HasLocalAvoidAgainst(const std::string& local_account_id,
                                               const std::string& other_account_id) const {
  std::lock_guard lock(mutex_);
  for (const auto& [id, row] : rows_) {
    (void)id;
    if (!row.local_avoid) {
      continue;
    }
    const bool local_is_payer = row.payer_account_id == local_account_id;
    const bool local_is_payee = row.payee_account_id == local_account_id;
    if (!local_is_payer && !local_is_payee) {
      continue;
    }
    const std::string& counterparty = local_is_payer ? row.payee_account_id : row.payer_account_id;
    if (counterparty == other_account_id) {
      return true;
    }
  }
  return false;
}

} // namespace pbr
