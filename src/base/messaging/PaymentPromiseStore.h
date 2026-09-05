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
 * Local durable store for payment promise receipts (P002 / P003).
 * Persists under profile `payment_promises.json`.
 *
 * Committed receipts live in `promises[]`. Inbound remote receipts stage in
 * `pending_inbound[]` until the user Accepts or Ignores them (P003).
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

  /** Stage a remote signed receipt without committing it (P003). */
  Roe<void> StageInbound(PaymentPromise promise);
  Roe<std::optional<PaymentPromise>> GetPendingInbound(const std::string& promise_id) const;
  Roe<std::vector<PaymentPromise>> ListPendingInbound() const;
  /** Move pending → committed (overwrites any existing committed row). */
  Roe<PaymentPromise> AcceptInbound(const std::string& promise_id);
  /** Drop a staged inbound receipt without committing. */
  Roe<bool> IgnoreInbound(const std::string& promise_id);

  /** Mark local_avoid on the receipt (does not touch ContactsStore). */
  Roe<void> MarkLocalAvoid(const std::string& promise_id, bool avoided = true);

  /** True if any receipt marks local_avoid against this account as counterparty of local_account_id. */
  bool HasLocalAvoidAgainst(const std::string& local_account_id, const std::string& other_account_id) const;

private:
  std::string Path() const;
  Roe<void> SaveUnlocked() const;
  static Roe<void> DecodeArrayInto(const Array* arr, std::unordered_map<std::string, PaymentPromise>& out);

  std::string data_dir_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, PaymentPromise> rows_;
  std::unordered_map<std::string, PaymentPromise> pending_inbound_;
};

} // namespace pbr
