#include "foundation/crypto/PskSessionRestart.h"

namespace pbr {

Roe<void> MarkPublicSessionsForRestart(IPskSessionStore& store) {
  auto records = store.List();
  if (!records) {
    return records.error();
  }
  for (PskSessionRecord& record : *records) {
    if (record.key.channel != CryptoChannel::E2ePublic || record.restart_pending) {
      continue;
    }
    record.restart_pending = true;
    if (auto saved = store.Save(record); !saved) {
      return saved.error();
    }
  }
  return {};
}

} // namespace pbr
