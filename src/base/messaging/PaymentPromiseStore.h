#pragma once

#include "base/data/PaymentPromiseTypes.h"
#include "common/Error.h"
#include "common/Module.h"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * Local durable store for payment promise receipts (P002).
 * Persists under profile `payment_promises.json`. No settlement / wire yet.
 */
class PaymentPromiseStore : public Module {
public:
  explicit PaymentPromiseStore(std::string data_dir);

  Roe<void> Load();
  Roe<void> Save() const;

  Roe<std::optional<PaymentPromise>> Get(const std::string& promise_id) const;
  Roe<std::vector<PaymentPromise>> List() const;
  /** Promises involving account_id as payer or payee. */
  Roe<std::vector<PaymentPromise>> ListForAccount(const std::string& account_id) const;

  Roe<void> Upsert(PaymentPromise promise);
  Roe<bool> Remove(const std::string& promise_id);

  /** Mark local_avoid on the receipt (does not touch ContactsStore). */
  Roe<void> MarkLocalAvoid(const std::string& promise_id, bool avoided = true);

  /** True if any receipt marks local_avoid against this account as counterparty of local_account_id. */
  bool HasLocalAvoidAgainst(const std::string& local_account_id, const std::string& other_account_id) const;

private:
  std::string Path() const;
  Roe<void> SaveUnlocked() const;

  std::string data_dir_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, PaymentPromise> rows_;
};

} // namespace pbr
